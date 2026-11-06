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
#include <libusb.h>
#include "protocol.h"

#define DIRTYJTAG_DEFAULT_ADDRESS "1209.c0ca"
#define DIRTYJTAG_USB_INTERFACE   0

static const uint32_t scanopts[] = {
  SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
  SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
  SR_CONF_CONN | SR_CONF_GET,
  SR_CONF_NUM_LOGIC_CHANNELS | SR_CONF_GET,
  // SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_LIST,
  SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
  SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
};

static const int32_t trigger_matches[] = {
  SR_TRIGGER_ZERO,
  SR_TRIGGER_ONE,
  // SR_TRIGGER_RISING,
  // SR_TRIGGER_FALLING,
  // SR_TRIGGER_EDGE,
};

static const uint64_t samplerates[] = {
  SAMPLE_RATE,
};

static struct sr_dev_driver gmm7550_ila_driver_info;

/* Unique per-device connection ID: USB port path, or bus.address as fallback */
static char *get_connection_id(libusb_context *ctx, struct sr_usb_dev_inst *usb)
{
  libusb_device **devlist;
  char path[64];
  ssize_t cnt, i;

  cnt = libusb_get_device_list(ctx, &devlist);
  for (i = 0; i < cnt; i++) {
    if (libusb_get_bus_number(devlist[i]) != usb->bus ||
        libusb_get_device_address(devlist[i]) != usb->address)
      continue;
    if (usb_get_port_path(devlist[i], path, sizeof(path)) == SR_OK) {
      libusb_free_device_list(devlist, 1);
      return g_strdup(path);
    }
    break;
  }
  if (cnt >= 0)
    libusb_free_device_list(devlist, 1);

  return g_strdup_printf("%d.%d", usb->bus, usb->address);
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
  struct drv_context *drvc = di->context;
  const char *conn = DIRTYJTAG_DEFAULT_ADDRESS;
  struct sr_config *cfg;
  GSList *l, *usb_devices;
  GSList *devices = NULL;

  struct sr_usb_dev_inst *usb;
  struct sr_dev_inst *sdi;

  drvc->instances = NULL;

  for (l = options; l; l = l->next) {
    cfg = l->data;
    switch (cfg->key) {
    case SR_CONF_CONN:
      conn = g_variant_get_string(cfg->data, NULL);
      break;
    }
  }

  usb_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);

  if (!usb_devices)
    return NULL;

  for (l=usb_devices; l; l=l->next) {
    usb = l->data;

    sdi = g_malloc0(sizeof(struct sr_dev_inst));

    sdi->status = SR_ST_INACTIVE;
    sdi->inst_type = SR_INST_USB;
    sdi->conn = usb;
    sdi->connection_id = get_connection_id(drvc->sr_ctx->libusb_ctx, usb);

    sdi->vendor = g_strdup("GMM-7550");
    sdi->model = g_strdup("gmm7550-ila");
    sdi->version = g_strdup("v0.5");
    sdi->driver = di;

    struct dev_context *devc = g_malloc0(sizeof(struct dev_context));
    devc->limit_samples = LIMIT_SAMPLES;
    devc->capture_ratio = 25;
    sdi->priv = devc;

    for (uint32_t i = 0; i < NUM_CHANNELS; i++) {
      char *name = g_strdup_printf("D%d", i);
      sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, name);
      g_free(name);
    }
    devices = g_slist_append(devices, sdi);
  }
  g_slist_free(usb_devices);
  return std_scan_complete(di, devices);
}

static int dev_open(struct sr_dev_inst *sdi)
{
  struct sr_usb_dev_inst *usb = sdi->conn;
  struct dev_context    *devc = sdi->priv;
  struct sr_dev_driver    *di = sdi->driver;
  struct drv_context    *drvc = di->context;
  int ret;

  ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
  if (ret < 0)
    return SR_ERR;

  if (libusb_kernel_driver_active(usb->devhdl, DIRTYJTAG_USB_INTERFACE) == 1) {
    ret = libusb_detach_kernel_driver(usb->devhdl, DIRTYJTAG_USB_INTERFACE);
    if (ret < 0) {
      sr_err("Canot detach kernel driver: %s.",
             libusb_error_name(ret));
      return SR_ERR;
    }
    devc->detached_kernel_driver = TRUE;
  }

  ret = libusb_claim_interface(usb->devhdl, DIRTYJTAG_USB_INTERFACE);
  if (ret < 0) {
    sr_err("Cannot claim interface: %s.", libusb_error_name(ret));
    return SR_ERR;
  }

  uint32_t id = gmm7550_ila_get_id(usb->devhdl);
  devc->cur_instr = -1; /* IR set without the cache, or by another tool */
  if (ILA_ID == id) {
    return SR_OK;
  } else {
    sr_err("Unexpected ILA JTAG ID: %08x (expected: %08x).", id, ILA_ID);
    return SR_ERR;
  }
}

static int dev_close(struct sr_dev_inst *sdi)
{
  struct sr_usb_dev_inst *usb = sdi->conn;
  struct dev_context *devc = sdi->priv;
  int ret;

  if (!usb) return SR_OK;
  if (!usb->devhdl) return SR_OK;

  ret = libusb_release_interface(usb->devhdl, DIRTYJTAG_USB_INTERFACE);
  if (ret) {
    sr_err("Cannot release interface: %s.", libusb_error_name(ret));
    return SR_ERR;
  }

  if (devc->detached_kernel_driver) {
    ret = libusb_attach_kernel_driver(usb->devhdl, DIRTYJTAG_USB_INTERFACE);
    if (ret) {
      sr_err("Cannot attach kernel driver: %s.",
             libusb_error_name(ret));
      return SR_ERR;
    }
    devc->detached_kernel_driver = FALSE;
  }

  libusb_close(usb->devhdl);
  usb->devhdl = NULL;
  return SR_OK;
}

static int config_get(uint32_t key, GVariant **data,
                      const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
  (void)data;
  (void)cg;
  struct dev_context *devc;

  switch (key) {
  case SR_CONF_CONN:
    if (!sdi || !sdi->connection_id)
      return SR_ERR_ARG;
    *data = g_variant_new_string(sdi->connection_id);
    break;
  case SR_CONF_NUM_LOGIC_CHANNELS:
    if (!sdi)
      return SR_ERR;
    *data = g_variant_new_uint32(g_slist_length(sdi->channels));
    break;
  case SR_CONF_SAMPLERATE:
    *data = g_variant_new_uint64(SAMPLE_RATE);
    break;
  case SR_CONF_CAPTURE_RATIO:
    if (!sdi)
      return SR_ERR;
    devc = sdi->priv;
    *data = g_variant_new_uint64(devc->capture_ratio);
    break;
  default:
    return SR_ERR_NA;
  }
  return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
                      const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
  (void)key;
  (void)data;
  (void)cg;

  struct dev_context *devc = sdi->priv;

  switch (key) {
  case SR_CONF_CAPTURE_RATIO:
    devc->capture_ratio = g_variant_get_uint64(data);
    break;
  default:
    return SR_ERR_NA;
  }
  return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
                       const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
  switch (key) {
  case SR_CONF_SCAN_OPTIONS:
  case SR_CONF_DEVICE_OPTIONS:
    return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
  case SR_CONF_TRIGGER_MATCH:
    *data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
    break;
  case SR_CONF_SAMPLERATE:
    *data = std_gvar_samplerates(ARRAY_AND_SIZE(samplerates));
    break;
  default:
    return SR_ERR_NA;
  }
  return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
  struct dev_context *devc = sdi->priv;
  struct sr_dev_driver *di = sdi->driver;
  struct drv_context *drvc = di->context;

  devc->cur_instr = -1; /* FPGA may have been reset or reconfigured */
  gmm7550_ila_convert_trigger(sdi);
  gmm7550_ila_configure_trigger(sdi, devc);
  gmm7550_ila_configure_pre_store(sdi, devc);

  usb_source_add(sdi->session, drvc->sr_ctx, 100,
                 gmm7550_ila_receive_data, (void *)sdi);

  gmm7550_ila_acquisition_start(sdi);

  std_session_send_df_header(sdi);

  return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
  struct sr_dev_driver *di = sdi->driver;
  struct drv_context *drvc = di->context;

  gmm7550_ila_acquisition_stop(sdi);

  std_session_send_df_end(sdi);
  usb_source_remove(sdi->session, drvc->sr_ctx);

  return SR_OK;
}

static struct sr_dev_driver gmm7550_ila_driver_info = {
  .name = "gmm7550-ila",
  .longname = "GMM-7550 GateMate FPGA ILA",
  .api_version = 1,
  .init = std_init,
  .cleanup = std_cleanup,
  .scan = scan,
  .dev_list = std_dev_list,
  .dev_clear = std_dev_clear,
  .config_get = config_get,
  .config_set = config_set,
  .config_list = config_list,
  .dev_open = dev_open,
  .dev_close = dev_close,
  .dev_acquisition_start = dev_acquisition_start,
  .dev_acquisition_stop = dev_acquisition_stop,
  .context = NULL,
};
SR_REGISTER_DEV_DRIVER(gmm7550_ila_driver_info);
