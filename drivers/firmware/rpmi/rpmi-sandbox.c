// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_RPMI

#include <dm.h>
#include <log.h>
#include <rpmi-uclass.h>
#include <dm/device_compat.h>

#include "rpmi-sandbox.h"

#define RPMI_SANDBOX_MIN_SLOT_SIZE 64
#define RPMI_SANDBOX_SHMEM_HEADER_SIZE 8
#define RPMI_SANDBOX_MAX_MSG_SIZE \
	(RPMI_SANDBOX_MIN_SLOT_SIZE - RPMI_SANDBOX_SHMEM_HEADER_SIZE)

#define DRIVER_NAME "rpmi-sandbox"
#define DRIVER_NAME_LEN (sizeof(DRIVER_NAME))

struct rpmi_sandbox {
	struct udevice *dev;
};

const struct rpmi_service_group *rpmi_sandbox_sg(u32 servicegroup_id)
{
	switch (servicegroup_id) {
	case RPMI_SRVGRP_CLOCK:
		return &rpmi_sandbox_clock_sg;
	default:
		return NULL;
	}
}

static int rpmi_sandbox_get_attr(struct rpmi_chan *chan,
				 enum rpmi_attribute_id id, u32 *data)
{
	const struct rpmi_service_group *sg = rpmi_sandbox_sg(chan->id);

	if (!sg)
		return -EOPNOTSUPP;

	switch (id) {
	case RPMI_ATTR_SPEC_VERSION:
		*data = RPMI_MKVER(1, 0);
		break;
	case RPMI_ATTR_MAX_MSG_DATA_SIZE:
		*data = RPMI_SANDBOX_MAX_MSG_SIZE;
		break;
	case RPMI_ATTR_SERVICEGROUP_ID:
		*data = chan->id;
		break;
	case RPMI_ATTR_SERVICEGROUP_VERSION:
		*data = sg->servicegroup_version;
		break;
	case RPMI_ATTR_IMPL_ID:
		*data = 0;
		break;
	case RPMI_ATTR_IMPL_VERSION:
		*data = RPMI_MKVER(1, 0);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rpmi_sandbox_request(struct rpmi_chan *chan, u32 service_id,
				const void *request, unsigned long request_len,
				void *response, unsigned long max_response_len,
				unsigned long *out_response_len)
{
	const struct rpmi_service_group *sg = rpmi_sandbox_sg(chan->id);
	const struct rpmi_service *service;
	u8 slot[RPMI_SANDBOX_MAX_MSG_SIZE];
	struct rpmi_transport trans = {
		.msg_data_size = sizeof(slot),
	};
	u16 response_data_len = 0;
	enum rpmi_error_codes err;

	if (!sg)
		return -EOPNOTSUPP;

	if (service_id > sg->max_service_id)
		return -EOPNOTSUPP;

	service = &sg->services[service_id];

	if (!service)
		return -EOPNOTSUPP;

	if (request_len < service->min_a2p_request_datalen)
		return -EINVAL;

	if (!service->process_a2p_request)
		return -ENOSYS;

	err = service->process_a2p_request(&trans, request_len, request,
					   &response_data_len, slot);

	if (err)
		return rpmi_to_linux_error(err);

	if (out_response_len)
		*out_response_len = response_data_len;

	if (response)
		memcpy(response, slot,
		       min(max_response_len, (unsigned long)response_data_len));

	return 0;
}

static int rpmi_sandbox_of_xlate(struct rpmi_chan *chan,
				 struct ofnode_phandle_args *args)
{
	if (args->args_count != 1)
		return -EINVAL;

	chan->id = args->args[0];

	return 0;
}

static int rpmi_sandbox_probe(struct udevice *dev)
{
	struct rpmi_sandbox *priv = dev_get_priv(dev);

	priv->dev = dev;

	return 0;
}

static const struct udevice_id rpmi_sandbox_ids[] = {
	{ .compatible = "sandbox,rpmi-transport" },
	{}
};

static const struct rpmi_transport_ops rpmi_sandbox_ops = {
	.of_xlate = rpmi_sandbox_of_xlate,
	.get_attr = rpmi_sandbox_get_attr,
	.request = rpmi_sandbox_request,
};

U_BOOT_DRIVER(rpmi_sandbox) = {
	.name = "rpmi-sandbox",
	.id = UCLASS_RPMI,
	.of_match = rpmi_sandbox_ids,
	.probe = rpmi_sandbox_probe,
	.priv_auto = sizeof(struct rpmi_sandbox),
	.ops = &rpmi_sandbox_ops,
};
