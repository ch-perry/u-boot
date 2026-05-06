// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_RPMI

#include <dm.h>
#include <dm/device_compat.h>
#include <log.h>
#include <rpmi-uclass.h>

static inline struct rpmi_transport_ops *rpmi_dev_ops(struct udevice *dev)
{
	return (struct rpmi_transport_ops *)dev->driver->ops;
}

int rpmi_get_by_index(struct udevice *dev, int index, struct rpmi_chan *chan)
{
	struct ofnode_phandle_args args;
	struct rpmi_transport_ops *ops;
	struct udevice *rpmi_dev;
	int ret;

	ret = dev_read_phandle_with_args(dev, "mboxes", "#mbox-cells", 0, index,
					 &args);
	if (ret)
		return ret;

	ret = uclass_get_device_by_ofnode(UCLASS_RPMI, args.node, &rpmi_dev);
	if (ret)
		return ret;

	ops = rpmi_dev_ops(rpmi_dev);

	if (!ops->of_xlate)
		return -ENOSYS;

	chan->dev = rpmi_dev;

	ret = ops->of_xlate(chan, &args);
	if (ret)
		return ret;

	return 0;
}

int rpmi_open(struct rpmi_chan *chan)
{
	struct rpmi_transport_ops *ops = rpmi_dev_ops(chan->dev);

	if (ops->open)
		return ops->open(chan);

	return 0;
}

void rpmi_close(struct rpmi_chan *chan)
{
	struct rpmi_transport_ops *ops = rpmi_dev_ops(chan->dev);

	if (ops->close)
		ops->close(chan);
}

int rpmi_get_attr(struct rpmi_chan *chan, enum rpmi_attribute_id id, u32 *data)
{
	struct rpmi_transport_ops *ops = rpmi_dev_ops(chan->dev);

	if (!ops->get_attr)
		return -ENOSYS;

	return ops->get_attr(chan, id, data);
}

int rpmi_send_with_resp(struct rpmi_chan *chan, u32 service_id,
			const void *request, unsigned long request_len,
			void *response, unsigned long max_response_len,
			unsigned long *out_response_len)
{
	struct rpmi_transport_ops *ops = rpmi_dev_ops(chan->dev);

	if (!ops->request)
		return -ENOSYS;

	return ops->request(chan, service_id, request, request_len, response,
			    max_response_len, out_response_len);
}

int rpmi_send_posted(struct rpmi_chan *chan, u32 service_id,
		     const void *request, unsigned long request_len)
{
	return rpmi_send_with_resp(chan, service_id, request, request_len, NULL,
				   0, NULL);
}

int rpmi_check_versions(struct rpmi_chan *chan, u32 min_proto_version,
			u32 servicegroup_id_expect,
			u32 min_servicegroup_version)
{
	u32 proto_version, servicegroup_id, servicegroup_version;
	int ret;

	ret = rpmi_get_attr(chan, RPMI_ATTR_SPEC_VERSION, &proto_version);
	if (ret)
		return ret;

	if (proto_version < min_proto_version) {
		dev_err(chan->dev,
			"protocol version too old, found 0x%x, expected min 0x%x\n",
			proto_version, min_proto_version);
		return -EINVAL;
	}

	ret = rpmi_get_attr(chan, RPMI_ATTR_SERVICEGROUP_ID, &servicegroup_id);
	if (ret)
		return ret;

	if (servicegroup_id != servicegroup_id_expect) {
		dev_err(chan->dev,
			"service group match fail, found 0x%x, expected 0x%x\n",
			servicegroup_id, servicegroup_id_expect);
		return -EINVAL;
	}

	ret = rpmi_get_attr(chan, RPMI_ATTR_SERVICEGROUP_VERSION,
			    &servicegroup_version);
	if (ret)
		return ret;

	if (servicegroup_version < min_servicegroup_version) {
		dev_err(chan->dev,
			"service group version too old, found 0x%x, expected min 0x%x\n",
			servicegroup_version, min_servicegroup_version);
		return -EINVAL;
	}

	return 0;
}

UCLASS_DRIVER(rpmi) = {
	.id = UCLASS_RPMI,
	.name = "rpmi",
};
