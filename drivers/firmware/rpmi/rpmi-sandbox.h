/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#ifndef _RPMI_SANDBOX_H
#define _RPMI_SANDBOX_H

#include <stdbool.h>
#include <asm/types.h>
#include <rpmi.h>

struct rpmi_transport {
	int msg_data_size;
};

struct rpmi_service {
	/** ID of the service */
	u8 service_id;

	/** Minimum data length for handling request */
	u16 min_a2p_request_datalen;

	/** Callback to process a2p request */
	enum rpmi_error_codes (*process_a2p_request)(
		struct rpmi_transport *trans, u16 request_data_len,
		const u8 *request_data, u16 *response_data_len,
		u8 *response_data);
};

struct rpmi_service_group {
	/** Name of the service group */
	const char *name;

	/** Maximum service ID of the service group */
	int max_service_id;

	/** Service group version */
	u32 servicegroup_version;

	/** Array of services indexed by service ID */
	const struct rpmi_service *services;
};

extern const struct rpmi_service_group rpmi_sandbox_clock_sg;

#endif
