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

#ifndef LIBSIGROK_HARDWARE_GMM7550_ILA_PROTOCOL_H
#define LIBSIGROK_HARDWARE_GMM7550_ILA_PROTOCOL_H

#include <stdint.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "gmm7550-ila"

#define ILA_ID 0x1c0ffee1

#define NUM_CHANNELS  19
#define LIMIT_SAMPLES 1024
#define SAMPLE_RATE   SR_MHZ(25)

struct dev_context {
        uint64_t limit_samples;
	uint64_t capture_ratio;
        uint32_t t_mask;
        uint32_t t_data;
        uint8_t * raw_sample_buf;
        gboolean detached_kernel_driver;
};

SR_PRIV uint32_t gmm7550_ila_get_id(libusb_device_handle *dev);

SR_PRIV int gmm7550_ila_convert_trigger(const struct sr_dev_inst *sdi);
SR_PRIV int gmm7550_ila_configure_trigger(const struct sr_dev_inst *sdi, struct dev_context *devc);
SR_PRIV int gmm7550_ila_configure_pre_store(const struct sr_dev_inst *sdi, struct dev_context *devc);
SR_PRIV int gmm7550_ila_acquisition_start(const struct sr_dev_inst *sdi);
SR_PRIV int gmm7550_ila_acquisition_stop(const struct sr_dev_inst *sdi);
SR_PRIV int gmm7550_ila_receive_data(int fd, int revents, void *cb_data);

#endif
