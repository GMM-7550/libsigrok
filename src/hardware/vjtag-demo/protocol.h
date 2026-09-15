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

#ifndef LIBSIGROK_HARDWARE_VJTAG_DEMO_PROTOCOL_H
#define LIBSIGROK_HARDWARE_VJTAG_DEMO_PROTOCOL_H

#include <stdint.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "vjtag-demo"

#define NUM_CHANNELS  8
#define LIMIT_SAMPLES 1024
#define SAMPLE_RATE   SR_MHZ(25)

struct vjtag_demo_tcp {
	char *address;
	char *port;
	int socket;
};

struct dev_context {
        uint64_t limit_samples;
	uint64_t capture_ratio;
        uint8_t  t_mask;
        uint8_t  t_data;
        //uint8_t  t_edge_r;
        //uint8_t  t_edge_f;
        uint8_t * raw_sample_buf;
};

SR_PRIV struct vjtag_demo_tcp *vjtag_demo_tcp_new(void);
SR_PRIV void vjtag_demo_tcp_free(struct vjtag_demo_tcp *tcp);
SR_PRIV int vjtag_demo_tcp_open(struct vjtag_demo_tcp *tcp);
SR_PRIV int vjtag_demo_tcp_close(struct vjtag_demo_tcp *tcp);
SR_PRIV char *vjtag_demo_recv(const int fd);
SR_PRIV char *vjtag_demo_run_cmd(int fd, const char *cmd);
SR_PRIV int vjtag_demo_check_id(struct vjtag_demo_tcp *tcp);
SR_PRIV int vjtag_demo_convert_trigger(const struct sr_dev_inst *sdi);
SR_PRIV uint32_t vjtag_demo_rw_reg(int fd, uint8_t r, uint8_t l, uint32_t d);
SR_PRIV int vjtag_demo_configure_trigger(int fd, struct dev_context *devc);
SR_PRIV int vjtag_demo_configure_pre_store(int fd, struct dev_context *devc);
SR_PRIV int vjtag_demo_acquisition_start(int fd);
SR_PRIV int vjtag_demo_acquisition_stop(int fd);
SR_PRIV int vjtag_demo_receive_data(int fd, int revents, void *cb_data);

#endif
