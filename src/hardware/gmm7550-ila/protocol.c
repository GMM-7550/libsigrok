/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2020, 2026 Anton Kuzmin <ak@gmm7550.dev>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "protocol.h"

#define DJ_USB_OUT_ENDPOINT 0x01
#define DJ_USB_IN_ENDPOINT  0x82
#define DJ_USB_TIMEOUT      1000 /* 100ms */
#define DJ_USB_BUFFER_SIZE  64

#define DJ_JTAG_FREQ 5000000 /* == 5 MHz */

/* DirtyJTAG commands */
#define DJ_CMD_STOP   0x00
#define DJ_CMD_INFO   0x01
#define DJ_CMD_FREQ   0x02
#define DJ_CMD_XFER   0x03
#define DJ_CMD_SETSIG 0x04
#define DJ_CMD_GETSIG 0x05
#define DJ_CMD_CLK    0x06

#define MAX_CMD_CNT   32

/* DirtyJTAG signal definitions */
#define DJ_SIG_TCK (1 << 1)
#define DJ_SIG_TDI (1 << 2)
#define DJ_SIG_TDO (1 << 3)
#define DJ_SIG_TMS (1 << 4)

#define ILA_IR_LENGTH    4
#define ILA_INSTR_ID     0x1 /* 0001 */
#define ILA_INSTR_CMD    0x4 /* 0100 */
#define ILA_INSTR_STATUS 0x5 /* 0101 */
#define ILA_INSTR_DATA   0x6 /* 0110 */
#define ILA_INSTR_ADDR   0x7 /* 0111 */
#define ILA_INSTR_TDATA  0x8 /* 1000 */
#define ILA_INSTR_TMASK  0x9 /* 1001 */
#define ILA_INSTR_TPOST  0xa /* 1010 */
#define ILA_INSTR_TADDR  0xe /* 1110 */

#define ILA_ID_LENGTH   32
#define ILA_CMD_LENGTH  8
#define ILA_ADDR_LENGTH 11
#define ILA_DATA_LENGTH 20

static int dj_usb_send(libusb_device_handle *dev, uint8_t *data, int len)
{
  int ret, sent;
  GString *text;

  if (!data || (len == 0))
    return SR_OK;

  text = sr_hexdump_new(data, len);
  sr_dbg("USB sent: %s", text->str);
  sr_hexdump_free(text);

  ret = libusb_bulk_transfer(dev,
                             DJ_USB_OUT_ENDPOINT,
                             data,  len,
                             &sent, DJ_USB_TIMEOUT);
  if (ret < 0) {
    sr_err("USB transmit error: %s.", libusb_error_name(ret));
    return SR_ERR_IO;
  }
  if (sent != len) {
    sr_err("USB short send: %d/%d bytes.",
           sent, len);
    return SR_ERR_IO;
  }
  return SR_OK;
}

static int dj_usb_recv(libusb_device_handle *dev, uint8_t *data, int len)
{
  int ret, rcvd;
  GString *text;

  if (!data)
    return SR_ERR_ARG;

  ret = libusb_bulk_transfer(dev,
                             DJ_USB_IN_ENDPOINT,
                             data,  len,
                             &rcvd, DJ_USB_TIMEOUT);
  if (ret < 0) {
    if (ret == LIBUSB_ERROR_TIMEOUT)
      sr_dbg("USB receive error: %s.", libusb_error_name(ret));
    else
      sr_err("USB receive error: %s.", libusb_error_name(ret));
    return SR_ERR_IO;
  }

  text = sr_hexdump_new(data, rcvd);
  sr_dbg("USB recv: %s", text->str);
  sr_hexdump_free(text);

  if (rcvd != len) {
    sr_err("USB short recv: %d/%d bytes.",
           rcvd, len);
    return SR_ERR_IO;
  }
  return SR_OK;
}

static int dj_init(libusb_device_handle *dev)
{
  const uint16_t freq_kHz = DJ_JTAG_FREQ / 1000; /* kHz */
  int cmd_cnt = 0;
  uint8_t cmd[MAX_CMD_CNT];

  cmd[cmd_cnt++] = DJ_CMD_FREQ;
  cmd[cmd_cnt++] = (freq_kHz >> 8) & 0xff;
  cmd[cmd_cnt++] = freq_kHz & 0xff;

  cmd[cmd_cnt++] = DJ_CMD_SETSIG;
  cmd[cmd_cnt++] = DJ_SIG_TDI | DJ_SIG_TMS | DJ_SIG_TCK; /* mask */
  cmd[cmd_cnt++] = DJ_SIG_TDI | DJ_SIG_TMS;              /* value */

  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = DJ_SIG_TDI | DJ_SIG_TMS;
  cmd[cmd_cnt++] = 5; /* go to Test-Logic-Reset state from any other state */

  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = DJ_SIG_TDI;
  cmd[cmd_cnt++] = 1; /* Run-Test/Idle */

  return dj_usb_send(dev, cmd, cmd_cnt);
}

static uint32_t dj_shift(libusb_device_handle *dev, uint32_t in, int len)
{
  uint8_t cmd[MAX_CMD_CNT], rcv;
  int cmd_cnt;
  uint32_t out = 0;
  int i, ret;

  for (i=len; i; i--) {
    cmd_cnt = 0;
    cmd[cmd_cnt++] = DJ_CMD_CLK;
    cmd[cmd_cnt++] = ((i==1) ? DJ_SIG_TMS : 0) | ((in&1) ? DJ_SIG_TDI : 0);
    cmd[cmd_cnt++] = 1;
    in >>= 1;

    (void) dj_usb_send(dev, cmd, cmd_cnt);

    cmd[0] = DJ_CMD_GETSIG;
    (void) dj_usb_send(dev, cmd, 1);
    ret = dj_usb_recv(dev, &rcv, 1);

    if (ret != SR_OK) {
      sr_err("USB receive error: %s.", libusb_error_name(ret));
      rcv = 0;
    }
    out >>= 1;
    out |= (rcv & DJ_SIG_TDO) ? 0x80000000 : 0;
  } /* Exit1-xR */

  cmd_cnt = 0;
  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = DJ_SIG_TMS;
  cmd[cmd_cnt++] = 1; /* Update-xR */

  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = 0;
  cmd[cmd_cnt++] = 1; /* Run-Test/Idle */

  (void) dj_usb_send(dev, cmd, cmd_cnt);

  out >>= 32 - len;
  return out;
}

static int gmm7550_ila_set_ir(libusb_device_handle *dev, uint8_t instr)
{
  int cmd_cnt = 0;
  uint8_t cmd[MAX_CMD_CNT];

  /* Run-Test/Idle -> ... */
  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = DJ_SIG_TMS;
  cmd[cmd_cnt++] = 2; /* Select-IR-Scan */

  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = 0;
  cmd[cmd_cnt++] = 2; /* Shift-IR */

  (void) dj_usb_send(dev, cmd, cmd_cnt);
  (void) dj_shift(dev, instr, ILA_IR_LENGTH); /* Run-Test/Idle */

  return SR_OK;
}

static uint32_t gmm7550_ila_get_set_dr(libusb_device_handle *dev, uint32_t data, int len)
{
  int cmd_cnt = 0;
  uint8_t cmd[MAX_CMD_CNT];

  /* Run-Test/Idle -> ... */
  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = DJ_SIG_TMS;
  cmd[cmd_cnt++] = 1; /* Select-DR-Scan */

  cmd[cmd_cnt++] = DJ_CMD_CLK;
  cmd[cmd_cnt++] = 0;
  cmd[cmd_cnt++] = 2; /* Shift-DR */

  (void) dj_usb_send(dev, cmd, cmd_cnt);
  return dj_shift(dev, data, len); /* Run-Test/Idle */
}

SR_PRIV uint32_t gmm7550_ila_get_id(libusb_device_handle *dev)
{
  (void) dj_init(dev);
  (void) gmm7550_ila_set_ir(dev, ILA_INSTR_ID);
  return gmm7550_ila_get_set_dr(dev, 0, ILA_ID_LENGTH);
}

SR_PRIV int gmm7550_ila_convert_trigger(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_trigger *trigger;
	struct sr_trigger_stage *stage;
	struct sr_trigger_match *match;

	const GSList *l, *m;

	devc = sdi->priv;

	devc->t_mask = 0;
	devc->t_data = 0;

	if (!(trigger = sr_session_trigger_get(sdi->session)))
		return SR_OK;

	for (l = trigger->stages; l; l = l->next) {
		stage = l->data;
		for (m = stage->matches; m; m = m->next) {
			match = m->data;
			uint32_t bit = 1 << (match->channel->index);
			if (!match->channel->enabled)
				continue;

			if (match->match == SR_TRIGGER_ONE) {
				devc->t_mask |= bit;
				devc->t_data |= bit;
			} else if (match->match == SR_TRIGGER_ZERO) {
				devc->t_mask |= bit;
				devc->t_data &= ~bit;
			}
		}
	}

	sr_dbg("Trigger mask: 0x%05x", devc->t_mask);
	sr_dbg("Trigger data: 0x%05x", devc->t_data);

	return SR_OK;
}

static uint32_t gmm7550_ila_rw_reg(const struct sr_dev_inst *sdi, uint8_t r, uint8_t l, uint32_t d)
{
  struct sr_usb_dev_inst *sr_usb = sdi->conn;
  struct libusb_device_handle *usb = sr_usb->devhdl;

  gmm7550_ila_set_ir(usb, r);
  return gmm7550_ila_get_set_dr(usb, d, l);
}

SR_PRIV int gmm7550_ila_configure_trigger(const struct sr_dev_inst *sdi, struct dev_context *devc)
{
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_CMD, ILA_CMD_LENGTH, 0x80);
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_CMD, ILA_CMD_LENGTH, 0x00);

	gmm7550_ila_rw_reg(sdi, ILA_INSTR_TMASK, ILA_DATA_LENGTH, devc->t_mask);
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_TDATA, ILA_DATA_LENGTH, devc->t_data);

	return SR_OK;
}

SR_PRIV int gmm7550_ila_configure_pre_store(const struct sr_dev_inst *sdi, struct dev_context *devc)
{
	uint16_t post_cnt;

	post_cnt = ((devc->limit_samples - 1) * (100 - devc->capture_ratio)) / 100;
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_TPOST, ILA_ADDR_LENGTH, post_cnt);

	return SR_OK;
}

SR_PRIV int gmm7550_ila_acquisition_start(const struct sr_dev_inst *sdi)
{
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_CMD, ILA_CMD_LENGTH, 0x01);
	return SR_OK;
}

SR_PRIV int gmm7550_ila_acquisition_stop(const struct sr_dev_inst *sdi)
{
	gmm7550_ila_rw_reg(sdi, ILA_INSTR_CMD, ILA_CMD_LENGTH, 0);
	return SR_OK;
}

SR_PRIV int gmm7550_ila_receive_data(int fd, int revents, void *cb_data)
{
  (void)fd;
  (void)revents;

  struct sr_dev_inst *sdi;
  struct dev_context *devc;
  int i;
  uint32_t *b;
  uint8_t la_status;
  uint16_t t_addr;
  int16_t start_addr;
  uint16_t post_cnt, pre_cnt;

  if (!(sdi = cb_data))
    return FALSE;

  if (!(devc = sdi->priv))
    return FALSE;

  struct sr_datafeed_packet packet;
  struct sr_datafeed_logic logic;

  la_status = gmm7550_ila_rw_reg(sdi, ILA_INSTR_STATUS, ILA_CMD_LENGTH, 0);

  sr_dbg("receive_data(), status = %02x", la_status);

  if (la_status & 0x80) { /* Acqusiont complete */

    if (!devc->raw_sample_buf) {
      devc->raw_sample_buf =
        g_try_malloc(4 * devc->limit_samples);
      if (!devc->raw_sample_buf) {
        sr_err("Sample buffer malloc failed.");
        sr_dev_acquisition_stop(sdi);
        return FALSE;
      }
    }

    t_addr = gmm7550_ila_rw_reg(sdi, ILA_INSTR_TADDR, ILA_ADDR_LENGTH, 0);
    sr_dbg("trigger addr = 0x%x", t_addr);

    post_cnt = gmm7550_ila_rw_reg(sdi, ILA_INSTR_TPOST, ILA_ADDR_LENGTH, 0);
    sr_dbg("post counter = %d", post_cnt);

    pre_cnt = LIMIT_SAMPLES - post_cnt;

    start_addr = t_addr - pre_cnt;
    sr_dbg("pre counter = %d", pre_cnt);
    if (start_addr < 0)
      start_addr += LIMIT_SAMPLES;

    /* Send pre-trigger data
     */
    gmm7550_ila_rw_reg(sdi, ILA_INSTR_CMD, ILA_CMD_LENGTH, 0x10); /* enable address auto-increment */
    gmm7550_ila_rw_reg(sdi, ILA_INSTR_ADDR, ILA_ADDR_LENGTH, start_addr);
    b = (uint32_t *)devc->raw_sample_buf;
    for (i = pre_cnt; i > 0; i--) {
      *b++ = gmm7550_ila_rw_reg(sdi, ILA_INSTR_DATA, ILA_DATA_LENGTH, 0);
    }

    packet.type = SR_DF_LOGIC;
    packet.payload = &logic;
    logic.length = pre_cnt;
    logic.unitsize = 4;
    logic.data = devc->raw_sample_buf;
    sr_session_send(cb_data, &packet);

    /* Mark trigger position
     */
    packet.type = SR_DF_TRIGGER;
    sr_session_send(cb_data, &packet);

    /* Sent post-trigger data
     */
    gmm7550_ila_rw_reg(sdi, ILA_INSTR_ADDR, ILA_ADDR_LENGTH, t_addr);
    b = (uint32_t *)devc->raw_sample_buf;
    for (i = post_cnt; i > 0; i--) {
      *b++ = gmm7550_ila_rw_reg(sdi, ILA_INSTR_DATA, ILA_DATA_LENGTH, 0);
    }

    packet.type = SR_DF_LOGIC;
    packet.payload = &logic;
    logic.length = post_cnt;
    logic.unitsize = 4;
    logic.data = devc->raw_sample_buf;
    sr_session_send(cb_data, &packet);

    g_free(devc->raw_sample_buf);
    devc->raw_sample_buf = NULL;

    sr_dbg("Triggered, stopping acquisition.");
    sr_dev_acquisition_stop(sdi);
  }
  return TRUE;
}
