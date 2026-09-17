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

static int dj_usb_send(const struct sr_dev_inst *sdi, uint8_t *data, int len)
{
  struct sr_usb_dev_inst *usb = sdi->conn;
  int ret, sent;
  GString *text;

  if (!data || (len == 0))
    return SR_OK;

  if (!usb)
    return SR_ERR_ARG;

  text = sr_hexdump_new(data, len);
  sr_dbg("USB sent: %s", text->str);
  sr_hexdump_free(text);

  ret = libusb_interrupt_transfer(usb->devhdl,
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

static int dj_usb_recv(const struct sr_dev_inst *sdi, uint8_t *data, int len)
{
  struct sr_usb_dev_inst *usb = sdi->conn;
  int ret, rcvd;
  GString *text;

  if (!data || !usb)
    return SR_ERR_ARG;

  ret = libusb_interrupt_transfer(usb->devhdl,
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

SR_PRIV int gmm7550_ila_check_id(const struct sr_dev_inst *sdi)
{
#if 0
  char *res;
	res = gmm7550_ila_run_cmd(fd, "irscan" TAP_NAME "0x006"); /* IDCODE */
	if (!res)
		return SR_ERR;
	res = gmm7550_ila_run_cmd(fd, "drscan" TAP_NAME "32 0");
	if (!res)
		return SR_ERR;

	if (strcmp(res, "031050DD")) /* Altera/Intel 10M50DA device ID */
		return SR_ERR;
#endif
	return SR_OK;
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
			uint8_t bit = 1 << (match->channel->index % 8);
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

SR_PRIV uint32_t gmm7550_ila_rw_reg(const struct sr_dev_inst *sdi, uint8_t r, uint8_t l, uint32_t d)
{
#if 1
  return 0;
#else
	char cmd[64];
	uint32_t ret = 0;

	sprintf(cmd, "vir_scan" TAP_NAME "0x%02x", 0x10 + (r & 0x0f));
	gmm7550_ila_run_cmd(fd, cmd);

	sprintf(cmd, "vdr_scan" TAP_NAME "%d 0x%x", l, d);
	sscanf(gmm7550_ila_run_cmd(fd, cmd), "%x", &ret);
	return ret;
#endif
}

SR_PRIV int gmm7550_ila_configure_trigger(const struct sr_dev_inst *sdi, struct dev_context *devc)
{
	gmm7550_ila_rw_reg(sdi, 1, 8, 0x80);
	gmm7550_ila_rw_reg(sdi, 1, 8, 0x00);

	gmm7550_ila_rw_reg(sdi, 4, 8, devc->t_mask);
	gmm7550_ila_rw_reg(sdi, 5, 8, devc->t_data);

	return SR_OK;
}

SR_PRIV int gmm7550_ila_configure_pre_store(const struct sr_dev_inst *sdi, struct dev_context *devc)
{
	uint16_t post_cnt;

	post_cnt = ((devc->limit_samples - 1) * (100 - devc->capture_ratio)) / 100;
	gmm7550_ila_rw_reg(sdi, 9, 10, post_cnt);

	return SR_OK;
}

SR_PRIV int gmm7550_ila_acquisition_start(const struct sr_dev_inst *sdi)
{
	gmm7550_ila_rw_reg(sdi, 1, 8, 0x01);
	return SR_OK;
}

SR_PRIV int gmm7550_ila_acquisition_stop(const struct sr_dev_inst *sdi)
{
	gmm7550_ila_rw_reg(sdi, 1, 8, 0);
	return SR_OK;
}

SR_PRIV int gmm7550_ila_receive_data(const struct sr_dev_inst *sdi, int revents, void *cb_data)
{
#if 0
	(void)revents;

	const struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct gmm7550_ila_tcp *tcp;

	int i;
	uint8_t *b;
	uint8_t la_status;
	uint16_t t_addr;
	int16_t start_addr;
	uint16_t post_cnt, pre_cnt;

	if (!(sdi = cb_data))
		return FALSE;

	if (!(devc = sdi->priv))
		return FALSE;

	if (!(tcp = sdi->conn))
		return FALSE;

	fd = tcp->socket;

	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;

	la_status = gmm7550_ila_rw_reg(tcp->socket, 0, 8, 0);

	sr_dbg("receive_data(), status = %02x", la_status);

	if (la_status & 0x80) { /* Acqusiont complete */

		if (!devc->raw_sample_buf) {
			devc->raw_sample_buf =
				g_try_malloc(devc->limit_samples);
			if (!devc->raw_sample_buf) {
				sr_err("Sample buffer malloc failed.");
				return FALSE;
			}
		}

		t_addr = gmm7550_ila_rw_reg(tcp->socket, 0x0A, 10, 0);
		sr_dbg("trigger addr = 0x%x", t_addr);

		post_cnt = gmm7550_ila_rw_reg(fd, 9, 10, 0);
		sr_dbg("post counter = %d", post_cnt);

		pre_cnt = LIMIT_SAMPLES - post_cnt;
		start_addr = t_addr - pre_cnt;
		if (start_addr < 0)
			start_addr += LIMIT_SAMPLES;

		/* Send pre-trigger data
		 */
		gmm7550_ila_rw_reg(tcp->socket, 0x08, 10, start_addr);
		b = devc->raw_sample_buf;
		for (i = pre_cnt; i > 0; i--)
			*b++ = gmm7550_ila_rw_reg(tcp->socket, 0x02, 8, 0);

		packet.type = SR_DF_LOGIC;
		packet.payload = &logic;
		logic.length = pre_cnt;
		logic.unitsize = 1;
		logic.data = devc->raw_sample_buf;
		sr_session_send(cb_data, &packet);

		/* Mark trigger position
		 */
		packet.type = SR_DF_TRIGGER;
		sr_session_send(cb_data, &packet);

		/* Sent post-trigger data
		 */
		gmm7550_ila_rw_reg(tcp->socket, 0x08, 10, t_addr);
		b = devc->raw_sample_buf;
		for (i = post_cnt; i > 0; i--)
			*b++ = gmm7550_ila_rw_reg(tcp->socket, 0x02, 8, 0);

		packet.type = SR_DF_LOGIC;
		packet.payload = &logic;
		logic.length = post_cnt;
		logic.unitsize = 1;
		logic.data = devc->raw_sample_buf;
		sr_session_send(cb_data, &packet);

		g_free(devc->raw_sample_buf);
		devc->raw_sample_buf = NULL;

		sr_dbg("Triggered, stopping acquisition.");
		sr_session_source_remove(sdi->session, fd);
		std_session_send_df_end(sdi);
	}
#endif
	return TRUE;
}
