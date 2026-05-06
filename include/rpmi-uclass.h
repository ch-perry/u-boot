/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#ifndef _RPMI_UCLASS_H
#define _RPMI_UCLASS_H

#include <rpmi.h>

/**
 * struct rpmi_transport_ops - The functions that an RPMI transport driver must implement.
 */
struct rpmi_transport_ops {
	/**
	 * of_xlate - Translate a client's device-tree (OF) mailbox specifier.
	 *
	 * Mandatory, cannot be null.
	 *
	 * @chan:	The channel to hold the translation result.
	 * @args:	The mailbox specifier values from device tree.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*of_xlate)(struct rpmi_chan *chan,
			struct ofnode_phandle_args *args);

	/**
	 * get_attr - Get an RPMI attribute
	 *
	 * Mandatory, cannot be null.
	 *
	 * @chan:	RPMI channel associated to the request.
	 * @id:		ID of the attribute to get.
	 * @data:	buffer to hold the parameter.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*get_attr)(struct rpmi_chan *chan, enum rpmi_attribute_id id,
			u32 *data);

	/**
	 * request - send a request and possibly receive a response.
	 *
	 * Mandatory, cannot be null.
	 *
	 * @chan:		RPMI channel associated to the request.
	 * @service_id:		RPMI service ID to call.
	 * @request:		buffer to hold the request.
	 * @request_len:	length of the request buffer.
	 * @response:		buffer to hold the response. If NULL, this is a posted request.
	 * @max_response_len:	size of the response buffer. If 0, this is a posted request.
	 * @out_response_len:	size of the actual response. Out parameter.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*request)(struct rpmi_chan *chan, u32 service_id,
		       const void *request, unsigned long request_len,
		       void *response, unsigned long max_response_len,
		       unsigned long *out_response_len);

	/**
	 * open - Perform channel initialization.
	 *
	 * Optional, can be null.
	 *
	 * @chan:	RPMI channel to initialize.
	 * @return 0 if OK, or a negative error code.
	 */
	int (*open)(struct rpmi_chan *chan);

	/**
	 * close - Perform channel deinitialization.
	 *
	 * Optional, can be null.
	 *
	 * @chan:	RPMI channel to deinitialize.
	 */
	void (*close)(struct rpmi_chan *chan);
};

#endif
