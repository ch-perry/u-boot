// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_RPMI

#include <asm/io.h>
#include <asm/sbi.h>
#include <asm/types.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <log.h>
#include <malloc.h>
#include <rpmi-uclass.h>
#include <string.h>

/* SBI MPXY channel IDs data in shared memory */
struct sbi_mpxy_channel_ids_data {
	/* Remaining number of channel ids */
	__le32 remaining;
	/* Returned channel ids in current function call */
	__le32 returned;
	/* Returned channel id array */
	__le32 channel_array[];
};

enum sbi_mpxy_attribute_id {
	/* Standard channel attributes managed by MPXY framework */
	SBI_MPXY_ATTR_MSG_PROT_ID = 0x00000000,
	SBI_MPXY_ATTR_MSG_PROT_VER = 0x00000001,
	SBI_MPXY_ATTR_MSG_MAX_LEN = 0x00000002,
	SBI_MPXY_ATTR_MSG_SEND_TIMEOUT = 0x00000003,
	SBI_MPXY_ATTR_MSG_COMPLETION_TIMEOUT = 0x00000004,
	SBI_MPXY_ATTR_CHANNEL_CAPABILITY = 0x00000005,
	SBI_MPXY_ATTR_SSE_EVENT_ID = 0x00000006,
	SBI_MPXY_ATTR_MSI_CONTROL = 0x00000007,
	SBI_MPXY_ATTR_MSI_ADDR_LO = 0x00000008,
	SBI_MPXY_ATTR_MSI_ADDR_HI = 0x00000009,
	SBI_MPXY_ATTR_MSI_DATA = 0x0000000A,
	SBI_MPXY_ATTR_EVENTS_STATE_CONTROL = 0x0000000B,
	SBI_MPXY_ATTR_STD_ATTR_MAX_IDX,
	/*
	 * Message protocol specific attributes, managed by
	 * the message protocol specification.
	 */
	SBI_MPXY_ATTR_MSGPROTO_ATTR_START = 0x80000000,
	SBI_MPXY_ATTR_MSGPROTO_ATTR_END = 0xffffffff
};

/* SBI_MPXY_ATTR_CHANNEL_CAPABILITY bits */
#define SBI_MPXY_CHAN_CAP_MSI BIT(0)
#define SBI_MPXY_CHAN_CAP_SSE BIT(1)
#define SBI_MPXY_CHAN_CAP_EVENTS_STATE BIT(2)
#define SBI_MPXY_CHAN_CAP_SEND_WITH_RESP BIT(3)
#define SBI_MPXY_CHAN_CAP_SEND_WITHOUT_RESP BIT(4)
#define SBI_MPXY_CHAN_CAP_GET_NOTIFICATIONS BIT(5)

/* RPMI message protocol specific MPXY attributes */
enum sbi_mpxy_rpmi_attribute_id {
	SBI_MPXY_RPMI_ATTR_SERVICEGROUP_ID,
	SBI_MPXY_RPMI_ATTR_SERVICEGROUP_VERSION,
	SBI_MPXY_RPMI_ATTR_IMPL_ID,
	SBI_MPXY_RPMI_ATTR_IMPL_VERSION,
	SBI_MPXY_RPMI_ATTR_MAX_ID
};

/* Possible values of MSG_PROT_ID attribute as-per SBI v3.0 (or higher) */
enum sbi_mpxy_msgproto_id {
	SBI_MPXY_MSGPROTO_RPMI_ID = 0x0,
};

struct sbi_mpxy_chan_desc {
	u32 id;
	u32 max_xfer_len;
	u32 attrs[SBI_MPXY_ATTR_STD_ATTR_MAX_IDX];
	u32 rpmi_attrs[SBI_MPXY_RPMI_ATTR_MAX_ID];
};

struct rpmi_sbi_mpxy {
	struct udevice *dev;
	void *shmem;
	unsigned long shmem_size;
	u32 channel_count;
	struct sbi_mpxy_chan_desc *channel_descs;
};

static int rpmi_sbi_mpxy_read_attrs(struct rpmi_sbi_mpxy *mpxy, u32 channel_id,
				    u32 base_attrid, u32 attr_count,
				    u32 *attrs_buf)
{
	struct sbiret sret;
	int i;

	sret = sbi_ecall(SBI_EXT_MPXY, SBI_EXT_MPXY_READ_ATTRS, channel_id,
			 base_attrid, attr_count, 0, 0, 0);
	if (sret.error)
		return sbi_to_linux_error(sret.error);

	for (i = 0; i < attr_count; i++)
		attrs_buf[i] = le32_to_cpu(((__le32 *)mpxy->shmem)[i]);

	return 0;
}

static int rpmi_sbi_mpxy_send_message(struct rpmi_sbi_mpxy *mpxy,
				      struct sbi_mpxy_chan_desc *desc,
				      u32 mpxy_fid, u32 msg_id, const void *tx,
				      unsigned long tx_len, void *rx,
				      unsigned long max_rx_len,
				      unsigned long *rx_len)
{
	void *shmem = mpxy->shmem;
	unsigned long rx_bytes;
	struct sbiret sret;

	if (tx)
		memcpy(shmem, tx, tx_len);

	sret = sbi_ecall(SBI_EXT_MPXY, mpxy_fid, desc->id, msg_id, tx_len, 0, 0,
			 0);

	if (rx && !sret.error) {
		rx_bytes = sret.value;
		if (rx_bytes > max_rx_len)
			return -ENOSPC;
		if (rx_bytes > desc->max_xfer_len)
			return -EIO;

		memcpy(rx, shmem, rx_bytes);
		if (rx_len)
			*rx_len = rx_bytes;
	}

	return sbi_to_linux_error(sret.error);
}

static int rpmi_sbi_mpxy_get_attr(struct rpmi_chan *chan,
				  enum rpmi_attribute_id id, u32 *data)
{
	struct rpmi_sbi_mpxy *mpxy = dev_get_priv(chan->dev);
	struct sbi_mpxy_chan_desc *desc = &mpxy->channel_descs[chan->id];

	switch (id) {
	case RPMI_ATTR_SPEC_VERSION:
		*data = desc->attrs[SBI_MPXY_ATTR_MSG_PROT_VER];
		break;
	case RPMI_ATTR_MAX_MSG_DATA_SIZE:
		*data = desc->max_xfer_len;
		break;
	case RPMI_ATTR_SERVICEGROUP_ID:
		*data = desc->rpmi_attrs[SBI_MPXY_RPMI_ATTR_SERVICEGROUP_ID];
		break;
	case RPMI_ATTR_SERVICEGROUP_VERSION:
		*data = desc->rpmi_attrs[SBI_MPXY_RPMI_ATTR_SERVICEGROUP_VERSION];
		break;
	case RPMI_ATTR_IMPL_ID:
		*data = desc->rpmi_attrs[SBI_MPXY_RPMI_ATTR_IMPL_ID];
		break;
	case RPMI_ATTR_IMPL_VERSION:
		*data = desc->rpmi_attrs[SBI_MPXY_RPMI_ATTR_IMPL_VERSION];
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rpmi_sbi_mpxy_request(struct rpmi_chan *chan, u32 service_id,
				 const void *request, unsigned long request_len,
				 void *response, unsigned long max_response_len,
				 unsigned long *out_response_len)
{
	struct rpmi_sbi_mpxy *mpxy = dev_get_priv(chan->dev);
	struct sbi_mpxy_chan_desc *desc = &mpxy->channel_descs[chan->id];
	u32 caps = desc->attrs[SBI_MPXY_ATTR_CHANNEL_CAPABILITY];
	u32 mpxy_fid;

	if (!request && request_len)
		return -EINVAL;

	if (request_len > desc->max_xfer_len)
		return -EINVAL;

	if (!response && max_response_len)
		return -EINVAL;

	if (response) {
		if (!(caps & SBI_MPXY_CHAN_CAP_SEND_WITH_RESP))
			return -EIO;

		mpxy_fid = SBI_EXT_MPXY_SEND_MSG_WITH_RESP;
	} else {
		if (!(caps & SBI_MPXY_CHAN_CAP_SEND_WITHOUT_RESP))
			return -EIO;

		mpxy_fid = SBI_EXT_MPXY_SEND_MSG_WITHOUT_RESP;
	}

	return rpmi_sbi_mpxy_send_message(mpxy, desc, mpxy_fid, service_id,
					  request, request_len, response,
					  max_response_len, out_response_len);
}

static int rpmi_sbi_mpxy_of_xlate(struct rpmi_chan *chan,
				  struct ofnode_phandle_args *args)
{
	struct rpmi_sbi_mpxy *mpxy = dev_get_priv(chan->dev);
	u32 channel_id, proto;
	int i;

	if (args->args_count != 2)
		return -EINVAL;

	channel_id = args->args[0];
	proto = args->args[1];

	for (i = 0; i < mpxy->channel_count; i++) {
		if (mpxy->channel_descs[i].id == channel_id &&
		    mpxy->channel_descs[i].attrs[SBI_MPXY_ATTR_MSG_PROT_ID] ==
			    proto) {
			chan->id = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int rpmi_sbi_mpxy_get_shmem_size(unsigned long *shmem_size)
{
	struct sbiret sret;

	sret = sbi_ecall(SBI_EXT_MPXY, SBI_EXT_MPXY_GET_SHMEM_SIZE, 0, 0, 0, 0,
			 0, 0);
	if (sret.error)
		return sbi_to_linux_error(sret.error);

	/* The shared memory size MUST be at least 4096 bytes */
	if (sret.value < PAGE_SIZE)
		return -ENODEV;

	/* The shared memory size MUST be multiple of 4096 bytes */
	if (sret.value & (PAGE_SIZE - 1))
		return -ENODEV;

	if (shmem_size)
		*shmem_size = sret.value;

	return 0;
}

static int rpmi_sbi_mpxy_get_channel_ids(struct rpmi_sbi_mpxy *mpxy)
{
	struct sbi_mpxy_channel_ids_data *sdata = mpxy->shmem;
	u32 remaining, returned, channel_id, start_index = 0;
	struct sbiret sret;
	int i, ret;

	do {
		sret = sbi_ecall(SBI_EXT_MPXY, SBI_EXT_MPXY_GET_CHANNEL_IDS,
				 start_index, 0, 0, 0, 0, 0);
		if (sret.error)
			return sbi_to_linux_error(sret.error);

		remaining = le32_to_cpu(sdata->remaining);
		returned = le32_to_cpu(sdata->returned);

		// Allocate memory on the first call.
		if (!mpxy->channel_descs) {
			mpxy->channel_count = returned + remaining;
			if (mpxy->channel_count == 0)
				return -ENODEV;

			mpxy->channel_descs =
				calloc(mpxy->channel_count,
				       sizeof(struct sbi_mpxy_chan_desc));
			if (!mpxy->channel_descs)
				return -ENOMEM;

			dev_dbg(mpxy->dev, "channel count = %i\n",
				mpxy->channel_count);
		}

		if (remaining && !returned)
			return -EPROTO;

		if (start_index + returned > mpxy->channel_count)
			return -EPROTO;

		for (i = 0; i < returned; i++) {
			mpxy->channel_descs[start_index + i].id =
				le32_to_cpu(sdata->channel_array[i]);
		}
		start_index += returned;
	} while (remaining);

	for (i = 0; i < mpxy->channel_count; i++) {
		channel_id = mpxy->channel_descs[i].id;

		ret = rpmi_sbi_mpxy_read_attrs(mpxy, channel_id,
					       SBI_MPXY_ATTR_MSG_PROT_ID,
					       SBI_MPXY_ATTR_STD_ATTR_MAX_IDX,
					       mpxy->channel_descs[i].attrs);
		if (ret)
			return ret;

		if (mpxy->channel_descs[i].attrs[SBI_MPXY_ATTR_MSG_PROT_ID] ==
		    SBI_MPXY_MSGPROTO_RPMI_ID) {
			ret = rpmi_sbi_mpxy_read_attrs(
				mpxy, channel_id,
				SBI_MPXY_ATTR_MSGPROTO_ATTR_START,
				SBI_MPXY_RPMI_ATTR_MAX_ID,
				mpxy->channel_descs[i].rpmi_attrs);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int rpmi_sbi_mpxy_probe(struct udevice *dev)
{
	struct rpmi_sbi_mpxy *mpxy = dev_get_priv(dev);
	struct sbi_mpxy_chan_desc *desc;
	unsigned long flags;
	struct sbiret sret;
	long sbi_version;
	int i, ret;

	mpxy->dev = dev;

	/* Probe for SBI MPXY extension */
	sbi_version = sbi_get_spec_version();
	if (sbi_version < 0 || sbi_version < sbi_mk_version(1, 0) ||
	    sbi_probe_extension(SBI_EXT_MPXY) <= 0) {
		dev_info(dev, "SBI MPXY extension not available\n");
		return -ENODEV;
	}

	/* Find-out shared memory size */
	ret = rpmi_sbi_mpxy_get_shmem_size(&mpxy->shmem_size);
	if (ret) {
		dev_err(dev, "failed to get MPXY shared memory size\n");
		return -ENODEV;
	}

	mpxy->shmem = memalign(PAGE_SIZE, mpxy->shmem_size);
	if (!mpxy->shmem)
		return -ENOMEM;

	/* Setup shmem in OVERWRITE mode. (flags[1:0] = 00b) */
	flags = 0;
	sret = sbi_ecall(SBI_EXT_MPXY, SBI_EXT_MPXY_SET_SHMEM,
			 virt_to_phys(mpxy->shmem), 0, flags, 0, 0, 0);
	if (sret.error) {
		ret = sbi_to_linux_error(sret.error);
		goto err;
	}

	ret = rpmi_sbi_mpxy_get_channel_ids(mpxy);
	if (ret)
		goto err;

	for (i = 0; i < mpxy->channel_count; i++) {
		desc = &mpxy->channel_descs[i];
		desc->max_xfer_len =
			min((u32)mpxy->shmem_size,
			    desc->attrs[SBI_MPXY_ATTR_MSG_MAX_LEN]);
	}

	return 0;

err:
	free(mpxy->channel_descs);
	free(mpxy->shmem);

	return ret;
}

static int rpmi_sbi_mpxy_remove(struct udevice *dev)
{
	struct rpmi_sbi_mpxy *mpxy = dev_get_priv(dev);

	free(mpxy->channel_descs);
	free(mpxy->shmem);

	return 0;
}

static const struct udevice_id rpmi_sbi_mpxy_ids[] = {
	{ .compatible = "riscv,sbi-mpxy-mbox" },
	{}
};

static const struct rpmi_transport_ops rpmi_sbi_mpxy_ops = {
	.of_xlate = rpmi_sbi_mpxy_of_xlate,
	.get_attr = rpmi_sbi_mpxy_get_attr,
	.request = rpmi_sbi_mpxy_request,
};

U_BOOT_DRIVER(rpmi_sbi_mpxy) = {
	.name = "rpmi-sbi-mpxy",
	.id = UCLASS_RPMI,
	.of_match = rpmi_sbi_mpxy_ids,
	.probe = rpmi_sbi_mpxy_probe,
	.remove = rpmi_sbi_mpxy_remove,
	.priv_auto = sizeof(struct rpmi_sbi_mpxy),
	.ops = &rpmi_sbi_mpxy_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
