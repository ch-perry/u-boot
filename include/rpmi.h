/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#ifndef _RPMI_H
#define _RPMI_H

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <rpmi_proto.h>

/* RPMI version encode/decode macros */
#define RPMI_VER_MAJOR(__ver) upper_16_bits(__ver)
#define RPMI_VER_MINOR(__ver) lower_16_bits(__ver)
#define RPMI_MKVER(__maj, __min) (((u32)(__maj) << 16) | (u16)(__min))

/* RPMI attribute IDs */
enum rpmi_attribute_id {
	RPMI_ATTR_SPEC_VERSION,
	RPMI_ATTR_MAX_MSG_DATA_SIZE,
	RPMI_ATTR_SERVICEGROUP_ID,
	RPMI_ATTR_SERVICEGROUP_VERSION,
	RPMI_ATTR_IMPL_ID,
	RPMI_ATTR_IMPL_VERSION,
	RPMI_ATTR_MAX_ID
};

static inline int rpmi_to_linux_error(int rpmi_error)
{
	switch (rpmi_error) {
	case RPMI_SUCCESS:
		return 0;
	case RPMI_ERR_INVALID_PARAM:
	case RPMI_ERR_BAD_RANGE:
	case RPMI_ERR_INVALID_STATE:
		return -EINVAL;
	case RPMI_ERR_DENIED:
		return -EPERM;
	case RPMI_ERR_INVALID_ADDR:
	case RPMI_ERR_HW_FAULT:
		return -EFAULT;
	case RPMI_ERR_ALREADY:
		return -EALREADY;
	case RPMI_ERR_BUSY:
		return -EBUSY;
	case RPMI_ERR_TIMEOUT:
		return -ETIMEDOUT;
	case RPMI_ERR_IO:
		return -ECOMM;
	case RPMI_ERR_FAILED:
	case RPMI_ERR_NOTSUPP:
	case RPMI_ERR_NO_DATA:
	case RPMI_ERR_EXTENSION:
	default:
		return -EOPNOTSUPP;
	}
}

struct udevice;

/**
 * struct rpmi_chan - A handle to a single RPMI channel.
 *
 * One RPMI channel is associated to one service group.
 */
struct rpmi_chan {
	struct udevice *dev;
	/* Written by rpmi_get_by_index or rpmi_open */
	unsigned long id;
	void *con_priv;
};

/**
 * rpmi_get_by_index - Get an RPMI channel by integer index.
 *
 * Initialize an RPMI channel using hardware information such as device tree
 * data. No communication with the PuC occurs when calling this function.
 *
 * @dev:        The client device.
 * @index:      The index of the RPMI channel to request.
 * @chan:       A pointer to a channel object to initialize.
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_get_by_index(struct udevice *dev, int index, struct rpmi_chan *chan);

/**
 * rpmi_open - Initialize an RPMI channel.
 *
 * Initialize an RPMI channel. Communication with the PuC is performed. Must be
 * called after rpmi_get_by_index.
 *
 * @chan:	The channel to initialize.
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_open(struct rpmi_chan *chan);

/**
 * rpmi_close - Deinitialize an RPMI channel.
 *
 * @chan:	The channel to deinitialize.
 */
void rpmi_close(struct rpmi_chan *chan);

/**
 * rpmi_get_attr - Get an RPMI channel attribute.
 *
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_get_attr(struct rpmi_chan *chan, enum rpmi_attribute_id id, u32 *data);

/**
 * rpmi_send_with_resp - Send a request and receive a response.
 *
 * @chan:		RPMI channel associated to the request.
 * @service_id:		RPMI service ID to call.
 * @request:		buffer to hold the request. Can be NULL if request_len is zero.
 * @request_len:	length of the request buffer.
 * @response:		buffer to hold the response. Cannot be NULL.
 * @max_response_len:	length of the response buffer.
 * @out_response_len:	length of the actual response. Out parameter.
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_send_with_resp(struct rpmi_chan *chan, u32 service_id,
			const void *request, unsigned long request_len,
			void *response, unsigned long max_response_len,
			unsigned long *out_response_len);

/**
 * rpmi_send_posted - Send a posted request.
 *
 * @chan:		RPMI channel associated to the request.
 * @service_id:		RPMI service ID to call.
 * @request:		buffer to hold the request. Can be NULL if request_len is zero.
 * @request_len:	length of the request buffer.
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_send_posted(struct rpmi_chan *chan, u32 service_id,
		     const void *request, unsigned long request_len);

/**
 * rpmi_check_versions - helper function to check version numbers of the
 * implementation.
 *
 * @chan:                       RPMI channel associated to the request.
 * @min_proto_version:          Minimum RPMI protocol version required. Caller
 *                              should use the RPMI_MKVER() macro.
 * @servicegroup_id_expect:     Service group ID expected.
 * @min_servicegroup_version:   Minimum Service group implementation version
 *                              required. Caller should use the RPMI_MKVER()
 *                              macro.
 * @return: 0 if OK, or a negative error code.
 */
int rpmi_check_versions(struct rpmi_chan *chan, u32 min_proto_version,
			u32 servicegroup_id_expect,
			u32 min_servicegroup_version);

#endif
