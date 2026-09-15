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

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "protocol.h"

#define COMMAND_TOKEN 0x1A
#define BUF_SIZE 256

#define TAP_NAME " max10.fpga "

SR_PRIV char socket_buf[BUF_SIZE];

SR_PRIV struct vjtag_demo_tcp *vjtag_demo_tcp_new(void)
{
	struct vjtag_demo_tcp *tcp;

	tcp = g_malloc0(sizeof(struct vjtag_demo_tcp));
	tcp->address = NULL;
	tcp->port = NULL;
	tcp->socket = -1;

	return tcp;
}

SR_PRIV void vjtag_demo_tcp_free(struct vjtag_demo_tcp *tcp)
{
	g_free(tcp->address);
	g_free(tcp->port);
}

SR_PRIV int vjtag_demo_tcp_open(struct vjtag_demo_tcp *tcp)
{
	struct addrinfo hints;
	struct addrinfo *results, *res;
	int err;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	err = getaddrinfo(tcp->address, tcp->port, &hints, &results);

	if (err) {
		sr_err("Address lookup failed: %s:%s: %s", tcp->address,
		       tcp->port, gai_strerror(err));
		return SR_ERR;
	}

	for (res = results; res; res = res->ai_next) {
		if ((tcp->socket = socket(res->ai_family, res->ai_socktype,
					  res->ai_protocol)) < 0)
			continue;
		if (connect(tcp->socket, res->ai_addr, res->ai_addrlen) != 0) {
			close(tcp->socket);
			tcp->socket = -1;
			continue;
		}
		break;
	}

	freeaddrinfo(results);

	if (tcp->socket < 0) {
		sr_err("Failed to connect to %s:%s: %s", tcp->address, tcp->port,
		       g_strerror(errno));
		return SR_ERR;
	}

	return SR_OK;
}

SR_PRIV int vjtag_demo_tcp_close(struct vjtag_demo_tcp *tcp)
{
	int socket = tcp->socket;
	tcp->socket = -1;

	if (socket > 0) {
		(void)vjtag_demo_run_cmd(socket, "exit");
		if (close(socket) < 0) {
			sr_err("Close failed: %s", g_strerror(errno));
			return SR_ERR;
		}
	}
	return SR_OK;
}

SR_PRIV char *vjtag_demo_recv(int fd)
{
	char *b = socket_buf;
	int r;
	int rc = 0;

	while (rc < BUF_SIZE-1) {
		r = recv(fd, b, 1, 0);
		if (r < 0) {
			sr_err("Receive error: %s", g_strerror(errno));
			return NULL;
		}
		if (*b == COMMAND_TOKEN) {
			*b = '\0';
			sr_dbg("-> %s", socket_buf);
			break;
		}
		rc++; b++;
	}

	return socket_buf;
}

SR_PRIV char *vjtag_demo_run_cmd(const int fd, const char *cmd)
{
	char *b;
	const char *c;
	int len;
	int res;

	len = strlen(cmd);

	if (len > BUF_SIZE-1)
		return NULL;

	c = cmd;
	b = socket_buf;

	while ((*b = *c++)) b++;
	*b = COMMAND_TOKEN;

	res = send(fd, (const char *)socket_buf, len+1, 0);

	if (res < 0) {
		sr_err("Send error: %s", g_strerror(errno));
		return NULL;
	}

	if (res < len+1) {
		sr_dbg("Send error: sent %d of %d bytes", res, len+1);
		return NULL;
	}

	sr_dbg("<- %s", cmd);

	return vjtag_demo_recv(fd);
}

SR_PRIV int vjtag_demo_check_id(struct vjtag_demo_tcp *tcp)
{
	char *res;
	int fd = tcp->socket;

	res = vjtag_demo_run_cmd(fd, "irscan" TAP_NAME "0x006"); /* IDCODE */
	if (!res)
		return SR_ERR;
	res = vjtag_demo_run_cmd(fd, "drscan" TAP_NAME "32 0");
	if (!res)
		return SR_ERR;

	if (strcmp(res, "031050DD")) /* Altera/Intel 10M50DA device ID */
		return SR_ERR;

	return SR_OK;
}

SR_PRIV int vjtag_demo_convert_trigger(const struct sr_dev_inst *sdi)
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

	sr_dbg("Trigger mask: 0x%02x", devc->t_mask);
	sr_dbg("Trigger data: 0x%02x", devc->t_data);

	return SR_OK;
}

SR_PRIV uint32_t vjtag_demo_rw_reg(int fd, uint8_t r, uint8_t l, uint32_t d)
{
	char cmd[64];
	uint32_t ret = 0;

	sprintf(cmd, "vir_scan" TAP_NAME "0x%02x", 0x10 + (r & 0x0f));
	vjtag_demo_run_cmd(fd, cmd);

	sprintf(cmd, "vdr_scan" TAP_NAME "%d 0x%x", l, d);
	sscanf(vjtag_demo_run_cmd(fd, cmd), "%x", &ret);
	return ret;
}

SR_PRIV int vjtag_demo_configure_trigger(int fd, struct dev_context *devc)
{
	vjtag_demo_rw_reg(fd, 1, 8, 0x80);
	vjtag_demo_rw_reg(fd, 1, 8, 0x00);

	vjtag_demo_rw_reg(fd, 4, 8, devc->t_mask);
	vjtag_demo_rw_reg(fd, 5, 8, devc->t_data);

	return SR_OK;
}

SR_PRIV int vjtag_demo_configure_pre_store(int fd, struct dev_context *devc)
{
	uint16_t post_cnt;

	post_cnt = ((devc->limit_samples - 1) * (100 - devc->capture_ratio)) / 100;
	vjtag_demo_rw_reg(fd, 9, 10, post_cnt);

	return SR_OK;
}

SR_PRIV int vjtag_demo_acquisition_start(int fd)
{
	vjtag_demo_rw_reg(fd, 1, 8, 0x01);
	return SR_OK;
}

SR_PRIV int vjtag_demo_acquisition_stop(int fd)
{
	vjtag_demo_rw_reg(fd, 1, 8, 0);
	return SR_OK;
}

SR_PRIV int vjtag_demo_receive_data(int fd, int revents, void *cb_data)
{
	(void)revents;

	const struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct vjtag_demo_tcp *tcp;

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

	la_status = vjtag_demo_rw_reg(tcp->socket, 0, 8, 0);

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

		t_addr = vjtag_demo_rw_reg(tcp->socket, 0x0A, 10, 0);
		sr_dbg("trigger addr = 0x%x", t_addr);

		post_cnt = vjtag_demo_rw_reg(fd, 9, 10, 0);
		sr_dbg("post counter = %d", post_cnt);

		pre_cnt = LIMIT_SAMPLES - post_cnt;
		start_addr = t_addr - pre_cnt;
		if (start_addr < 0)
			start_addr += LIMIT_SAMPLES;

		/* Send pre-trigger data
		 */
		vjtag_demo_rw_reg(tcp->socket, 0x08, 10, start_addr);
		b = devc->raw_sample_buf;
		for (i = pre_cnt; i > 0; i--)
			*b++ = vjtag_demo_rw_reg(tcp->socket, 0x02, 8, 0);

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
		vjtag_demo_rw_reg(tcp->socket, 0x08, 10, t_addr);
		b = devc->raw_sample_buf;
		for (i = post_cnt; i > 0; i--)
			*b++ = vjtag_demo_rw_reg(tcp->socket, 0x02, 8, 0);

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

	return TRUE;
}
