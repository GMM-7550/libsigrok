/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2020 Anton Kuzmin <anton.kuzmin@cs.fau.de>
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
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_DEMO_DEV,
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_NUM_LOGIC_CHANNELS | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_LIST,
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

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc;
	GSList *devices;
	const char *conn = NULL;

	struct sr_config *src;
	GSList *l;

	devices = NULL;
	drvc = di->context;
	drvc->instances = NULL;

	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}

	if (!conn)
		return NULL;

	struct gmm7550_ila_tcp *tcp = gmm7550_ila_tcp_new();

	char **strs = g_strsplit(conn, "/", 3);
	tcp->address = g_strdup(strs[1]);
	tcp->port = g_strdup(strs[2]);
	g_strfreev(strs);

	if (!tcp->address)
		return NULL;

	if (gmm7550_ila_tcp_open(tcp) != SR_OK)
		return NULL;

	if (gmm7550_ila_check_id(tcp) != SR_OK)
		return NULL;

	struct sr_dev_inst *sdi = g_malloc0(sizeof(struct sr_dev_inst));

	sdi->status = SR_ST_INACTIVE;
	sdi->vendor = g_strdup("GMM-7550");
	sdi->model = g_strdup("gmm7550-ila");
	sdi->version = g_strdup("v0.1");
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

	sdi->inst_type = SR_INST_USER;
	sdi->conn = tcp;

	gmm7550_ila_tcp_close(tcp);

	devices = g_slist_append(devices, sdi);

	return std_scan_complete(di, devices);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	struct drv_context *drvc = di->context;
	struct sr_dev_inst *sdi;
	GSList *l;

	if (drvc) {
		for (l = drvc->instances; l; l = l->next) {
			sdi = l->data;
			struct gmm7550_ila_tcp *tcp = sdi->conn;
			if (tcp) {
				gmm7550_ila_tcp_close(tcp);
				gmm7550_ila_tcp_free(tcp);
				g_free(tcp);
			}
			sdi->conn = NULL;
		}
	}

	return std_dev_clear(di);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	if (!sdi || !sdi->conn)
		return SR_ERR;

	return gmm7550_ila_tcp_open(sdi->conn);
}

static int dev_close(struct sr_dev_inst *sdi)
{
	if (!sdi || !sdi->conn)
		return SR_ERR;

	return gmm7550_ila_tcp_close(sdi->conn);
}

static int config_get(uint32_t key, GVariant **data,
		      const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	// (void)sdi;
	(void)data;
	(void)cg;
	struct dev_context *devc;
	struct gmm7550_ila_tcp *tcp;

	switch (key) {
	case SR_CONF_CONN:
		if (!sdi || !sdi->conn)
			return SR_ERR_ARG;
		tcp = sdi->conn;
		*data = g_variant_new_printf("%s:%s", tcp->address, tcp->port);
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
	// (void)sdi;
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
	struct gmm7550_ila_tcp *tcp = sdi->conn;
	struct dev_context *devc = sdi->priv;

	gmm7550_ila_convert_trigger(sdi);
	gmm7550_ila_configure_trigger(tcp->socket, devc);
	gmm7550_ila_configure_pre_store(tcp->socket, devc);

	sr_session_source_add(sdi->session, tcp->socket, G_IO_IN, 100,
			      gmm7550_ila_receive_data, (struct sr_dev_inst *)sdi);

	gmm7550_ila_acquisition_start(tcp->socket);

	std_session_send_df_header(sdi);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct gmm7550_ila_tcp *tcp = sdi->conn;
	//struct dev_context *devc = sdi->priv;

	std_session_send_df_end(sdi);
	sr_session_source_remove(sdi->session, tcp->socket);

	gmm7550_ila_acquisition_stop(tcp->socket);

	return SR_OK;
}

static struct sr_dev_driver gmm7550_ila_driver_info = {
	.name = "gmm7550-ila",
	.longname = "GMM-7550 GateMate FPGA ILA Driver",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
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
