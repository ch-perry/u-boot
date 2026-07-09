// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 * Copyright (c) 2026, Rahul Pathak <rahul.pathak@oss.qualcomm.com>
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
#include <dm/device-internal.h>

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
	SBI_MPXY_RPMI_ATTR_SERVICEGROUP_ID = 0x00,
	SBI_MPXY_RPMI_ATTR_SERVICEGROUP_VERSION = 0x01,
	SBI_MPXY_RPMI_ATTR_IMPL_ID = 0x02,
	SBI_MPXY_RPMI_ATTR_IMPL_VERSION = 0x03,
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

static struct rpmi_sbi_mpxy *g_mpxy;

static __maybe_unused struct rpmi_sbi_mpxy *mpxy_get_global(void)
{
	struct udevice *dev;
	struct uclass *uc;

	if (!g_mpxy) {
		uclass_get(UCLASS_RPMI, &uc);
		uclass_foreach_dev(dev, uc)
		{
			device_probe(dev);
		}
	}

	return g_mpxy;
}

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

static __maybe_unused int
rpmi_sbi_mpxy_write_attrs(struct rpmi_sbi_mpxy *mpxy, u32 channel_id,
			  u32 base_attrid, u32 attr_count, u32 *attrs_buf)
{
	struct sbiret sret;
	int i;

	for (i = 0; i < attr_count; i++)
		((__le32 *)mpxy->shmem)[i] = cpu_to_le32(attrs_buf[i]);

	sret = sbi_ecall(SBI_EXT_MPXY, SBI_EXT_MPXY_WRITE_ATTRS, channel_id,
			 base_attrid, attr_count, 0, 0, 0);
	if (sret.error)
		return sbi_to_linux_error(sret.error);

	return 0;
}

static int rpmi_sbi_mpxy_send_message(void *shmem, u32 mpxy_fid, u32 channel_id,
				      u32 msg_id, const void *tx,
				      unsigned long tx_len, void *rx,
				      unsigned long max_rx_len,
				      unsigned long *rx_len)
{
	unsigned long rx_bytes;
	struct sbiret sret;

	if (tx)
		memcpy(shmem, tx, tx_len);

	sret = sbi_ecall(SBI_EXT_MPXY, mpxy_fid, channel_id, msg_id, tx_len, 0,
			 0, 0);

	if (rx && !sret.error) {
		rx_bytes = sret.value;
		if (rx_bytes > max_rx_len)
			return -ENOSPC;

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

	return rpmi_sbi_mpxy_send_message(mpxy->shmem, mpxy_fid, desc->id,
					  service_id, request, request_len,
					  response, max_response_len,
					  out_response_len);
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
	int i, ret;

	mpxy->dev = dev;

	/* Probe for SBI MPXY extension */
	if (sbi_get_spec_version() < sbi_mk_version(1, 0) ||
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

	g_mpxy = mpxy;

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

#if CONFIG_IS_ENABLED(CMD_MPXY) && !CONFIG_IS_ENABLED(XPL_BUILD)

#include <command.h>
#include <hexdump.h>

static const char *mpxy_proto_to_str(u32 proto_id)
{
	switch (proto_id) {
	case SBI_MPXY_MSGPROTO_RPMI_ID:
		return "RPMI";
	default:
		return "Unknown";
	}
}

/**
 * mpxy list
 *
 * List the mpxy channels
 */
static int do_mpxy_list(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct rpmi_sbi_mpxy *mpxy = mpxy_get_global();
	struct sbi_mpxy_chan_desc *chan;
	u32 proto;
	int i;

	if (!mpxy)
		return CMD_RET_FAILURE;

	for (i = 0; i < mpxy->channel_count; i++) {
		chan = &mpxy->channel_descs[i];
		proto = chan->attrs[SBI_MPXY_ATTR_MSG_PROT_ID];
		printf("Channel 0x%x: proto_id=0x%x (%s), ver=0x%x\n", chan->id,
		       proto, mpxy_proto_to_str(proto),
		       chan->attrs[SBI_MPXY_ATTR_MSG_PROT_VER]);
	}

	printf("sbi_mpxy: found %d channels\n", mpxy->channel_count);
	return CMD_RET_SUCCESS;
}

static void mpxy_print_capability(u32 val)
{
	static const struct {
		u32 bit;
		const char *name;
	} caps[] = {
		{ SBI_MPXY_CHAN_CAP_MSI, "MSI" },
		{ SBI_MPXY_CHAN_CAP_SSE, "SSE_EVENT" },
		{ SBI_MPXY_CHAN_CAP_EVENTS_STATE, "EVENTS_STATE" },
		{ SBI_MPXY_CHAN_CAP_SEND_WITH_RESP, "SEND_WITH_RESP" },
		{ SBI_MPXY_CHAN_CAP_SEND_WITHOUT_RESP, "SEND_WITHOUT_RESP" },
		{ SBI_MPXY_CHAN_CAP_GET_NOTIFICATIONS, "GET_NOTIFICATIONS" },
	};

	bool any = false;
	int i;

	printf(" (");

	if (!val) {
		printf("none)\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(caps); i++) {
		if (val & caps[i].bit) {
			if (any)
				printf("|");
			printf("%s", caps[i].name);
			any = true;
		}
	}
	printf(")\n");
}

/* Decode well-known field values for selected standard attributes */
static void mpxy_print_std_attr(u32 attr_id, u32 val)
{
	/* Standard MPXY attribute names, indexed by attr_id */
	static const char *const mpxy_std_attr_names[] = {
		[SBI_MPXY_ATTR_MSG_PROT_ID] = "MSG_PROT_ID",
		[SBI_MPXY_ATTR_MSG_PROT_VER] = "MSG_PROT_VER",
		[SBI_MPXY_ATTR_MSG_MAX_LEN] = "MSG_MAX_LEN",
		[SBI_MPXY_ATTR_MSG_SEND_TIMEOUT] = "MSG_SEND_TIMEOUT",
		[SBI_MPXY_ATTR_MSG_COMPLETION_TIMEOUT] =
			"MSG_COMPLETION_TIMEOUT",
		[SBI_MPXY_ATTR_CHANNEL_CAPABILITY] = "CHANNEL_CAPABILITY",
		[SBI_MPXY_ATTR_SSE_EVENT_ID] = "SSE_EVENT_ID",
		[SBI_MPXY_ATTR_MSI_CONTROL] = "MSI_CONTROL",
		[SBI_MPXY_ATTR_MSI_ADDR_LO] = "MSI_ADDR_LO",
		[SBI_MPXY_ATTR_MSI_ADDR_HI] = "MSI_ADDR_HI",
		[SBI_MPXY_ATTR_MSI_DATA] = "MSI_DATA",
		[SBI_MPXY_ATTR_EVENTS_STATE_CONTROL] = "EVENTS_STATE_CONTROL",
	};

	printf("  [0x%02x] %-28s: 0x%08x", attr_id,
	       mpxy_std_attr_names[attr_id], val);

	switch (attr_id) {
	case SBI_MPXY_ATTR_MSG_PROT_ID:
		printf(" (%s)\n", mpxy_proto_to_str(val));
		break;
	case SBI_MPXY_ATTR_MSG_PROT_VER:
		printf(" (v%u.%u)\n", (val >> 16) & 0xffff, val & 0xffff);
		break;
	case SBI_MPXY_ATTR_MSG_MAX_LEN:
		printf(" (%u bytes)\n", val);
		break;
	case SBI_MPXY_ATTR_MSG_SEND_TIMEOUT:
	case SBI_MPXY_ATTR_MSG_COMPLETION_TIMEOUT:
		printf(" (%u ms)\n", val);
		break;
	case SBI_MPXY_ATTR_CHANNEL_CAPABILITY:
		mpxy_print_capability(val);
		break;
	default:
		printf("\n");
		break;
	}
}

static const char *mpxy_rpmi_proto_attr_name(u32 attr_id)
{
	switch (attr_id) {
	case SBI_MPXY_RPMI_ATTR_SERVICEGROUP_ID:
		return "SERVICEGROUP_ID";
	case SBI_MPXY_RPMI_ATTR_SERVICEGROUP_VERSION:
		return "SERVICEGROUP_VERSION";
	case SBI_MPXY_RPMI_ATTR_IMPL_ID:
		return "IMPLEMENTATION_ID";
	case SBI_MPXY_RPMI_ATTR_IMPL_VERSION:
		return "IMPLEMENTATION_VER";
	default:
		return "UNKNOWN";
	};
}

static void mpxy_print_proto_attr(u32 attr_id, u32 val, bool is_rpmi)
{
	u32 rpmi_attr_id = attr_id - SBI_MPXY_ATTR_MSGPROTO_ATTR_START;
	const char *name = is_rpmi ? mpxy_rpmi_proto_attr_name(rpmi_attr_id) :
				     "UNKNOWN";

	printf("  [0x%08x] %-24s: 0x%08x", attr_id, name, val);

	if (is_rpmi &&
	    (rpmi_attr_id == SBI_MPXY_RPMI_ATTR_SERVICEGROUP_VERSION ||
	     rpmi_attr_id == SBI_MPXY_RPMI_ATTR_IMPL_VERSION)) {
		printf(" (v%u.%u)\n", (val >> 16) & 0xffff, val & 0xffff);
	} else {
		printf("\n");
	}
}

/**
 * mpxy readattr <channel_id> <attr_id>
 *
 * Read the attribute on a channel
 */
static int do_mpxy_readattr(struct cmd_tbl *cmdtp, int flag, int argc,
			    char *const argv[])
{
	struct rpmi_sbi_mpxy *mpxy = mpxy_get_global();
	u32 chan_id, attr_id, attr_val;
	int ret;

	if (argc < 3)
		return CMD_RET_USAGE;

	if (!mpxy)
		return CMD_RET_FAILURE;

	chan_id = simple_strtoul(argv[1], NULL, 0);
	attr_id = simple_strtoul(argv[2], NULL, 0);

	ret = rpmi_sbi_mpxy_read_attrs(mpxy, chan_id, attr_id, 1, &attr_val);
	if (ret) {
		printf("Read attr 0x%x on channel 0x%x failed (%d)\n", attr_id,
		       chan_id, ret);
		return CMD_RET_FAILURE;
	}

	printf("Channel 0x%x: attr[0x%x] = 0x%x\n", chan_id, attr_id, attr_val);
	return CMD_RET_SUCCESS;
}

/**
 * mpxy readattrall <channel_id>
 */
static int do_mpxy_readattr_all(struct cmd_tbl *cmdtp, int flag, int argc,
				char *const argv[])
{
	struct rpmi_sbi_mpxy *mpxy = mpxy_get_global();
	u32 chan_id, attr_val[SBI_MPXY_ATTR_STD_ATTR_MAX_IDX];
	bool is_rpmi;
	int i, ret;

	if (argc < 2)
		return CMD_RET_USAGE;

	if (!mpxy)
		return CMD_RET_FAILURE;

	chan_id = simple_strtoul(argv[1], NULL, 0);

	ret = rpmi_sbi_mpxy_read_attrs(mpxy, chan_id, SBI_MPXY_ATTR_MSG_PROT_ID,
				       SBI_MPXY_ATTR_STD_ATTR_MAX_IDX,
				       attr_val);
	if (ret) {
		printf("Read attr on channel 0x%x failed (%d)\n", chan_id, ret);
		return CMD_RET_FAILURE;
	}

	/* Standard attributes */
	printf("Channel 0x%x standard attributes:\n", chan_id);
	for (i = 0; i < SBI_MPXY_ATTR_STD_ATTR_MAX_IDX; i++)
		mpxy_print_std_attr(i, attr_val[i]);

	/* Protocol-specific attributes */
	printf("Channel 0x%x protocol attributes:\n", chan_id);

	for (i = SBI_MPXY_ATTR_MSGPROTO_ATTR_START;
	     i < SBI_MPXY_ATTR_MSGPROTO_ATTR_END; i++) {
		ret = rpmi_sbi_mpxy_read_attrs(mpxy, chan_id, i, 1,
					       &attr_val[0]);
		if (ret)
			break;

		is_rpmi = mpxy->channel_descs[chan_id]
				  .attrs[SBI_MPXY_ATTR_MSG_PROT_ID] ==
			  SBI_MPXY_MSGPROTO_RPMI_ID;
		mpxy_print_proto_attr(i, attr_val[0], is_rpmi);
	}

	return CMD_RET_SUCCESS;
}

/**
 * mpxy writeattr <channel_id> <attr_id> <value>
 */
static int do_mpxy_writeattr(struct cmd_tbl *cmdtp, int flag, int argc,
			     char *const argv[])
{
	struct rpmi_sbi_mpxy *mpxy = mpxy_get_global();
	u32 chan_id, attr_id, attr_val;
	int ret;

	if (argc < 4)
		return CMD_RET_USAGE;

	if (!mpxy)
		return CMD_RET_FAILURE;

	chan_id = simple_strtoul(argv[1], NULL, 0);
	attr_id = simple_strtoul(argv[2], NULL, 0);
	attr_val = simple_strtoul(argv[3], NULL, 0);

	ret = rpmi_sbi_mpxy_write_attrs(mpxy, chan_id, attr_id, 1, &attr_val);
	if (ret) {
		printf("Write attr 0x%x on channel 0x%x failed (%d)\n", attr_id,
		       chan_id, ret);
		return CMD_RET_FAILURE;
	}

	printf("Channel 0x%x: attr[0x%x] written 0x%x\n", chan_id, attr_id,
	       attr_val);
	return CMD_RET_SUCCESS;
}

/**
 * mpxy send <channel_id> <msg_id> [payload_word ...]
 *
 * For RPMI, msg_id is the service_id. Payload words follow.
 */
static int do_mpxy_send(struct cmd_tbl *cmdtp, int flag, int argc,
			char *const argv[])
{
	struct rpmi_sbi_mpxy *mpxy = mpxy_get_global();
	u32 tx[64];
	u32 rx[64];
	unsigned long tx_words, rx_bytes;
	u32 chan_id, service_id;
	int i, ret;

	if (argc < 3)
		return CMD_RET_USAGE;

	if (!mpxy)
		return CMD_RET_FAILURE;

	chan_id = simple_strtoul(argv[1], NULL, 0);
	service_id = simple_strtoul(argv[2], NULL, 0);
	tx_words = argc - 3;

	if (tx_words > ARRAY_SIZE(tx)) {
		printf("Too many words (max %zu)\n", ARRAY_SIZE(tx));
		return CMD_RET_FAILURE;
	}

	for (i = 0; i < tx_words; i++)
		tx[i] = simple_strtoul(argv[3 + i], NULL, 0);

	ret = rpmi_sbi_mpxy_send_message(mpxy->shmem,
					 SBI_EXT_MPXY_SEND_MSG_WITH_RESP,
					 chan_id, service_id, tx, tx_words * 4,
					 rx, sizeof(rx), &rx_bytes);
	if (ret) {
		printf("Send on channel 0x%x failed (%d)\n", chan_id, ret);
		return CMD_RET_FAILURE;
	}

	printf("Response (%ld word(s)):\n", rx_bytes / 4);
	print_hex_dump(" ", DUMP_PREFIX_OFFSET, 4, 4, rx, rx_bytes, true);

	return CMD_RET_SUCCESS;
}

/**
 * Subcommand table and top-level dispatch
 */
static struct cmd_tbl cmd_mpxy_sub[] = {
	U_BOOT_CMD_MKENT(list, 1, 0, do_mpxy_list, "", ""),
	U_BOOT_CMD_MKENT(readattr, 3, 0, do_mpxy_readattr, "", ""),
	U_BOOT_CMD_MKENT(readattrall, 2, 0, do_mpxy_readattr_all, "", ""),
	U_BOOT_CMD_MKENT(writeattr, 4, 0, do_mpxy_writeattr, "", ""),
	U_BOOT_CMD_MKENT(send, CONFIG_SYS_MAXARGS, 0, do_mpxy_send, "", ""),
};

static int do_mpxy(struct cmd_tbl *cmdtp, int flag, int argc,
		   char *const argv[])
{
	struct cmd_tbl *c;

	if (argc < 2)
		return CMD_RET_USAGE;

	c = find_cmd_tbl(argv[1], cmd_mpxy_sub, ARRAY_SIZE(cmd_mpxy_sub));
	if (!c)
		return CMD_RET_USAGE;

	return c->cmd(cmdtp, flag, argc - 1, argv + 1);
}

U_BOOT_CMD(
	mpxy, CONFIG_SYS_MAXARGS, 0, do_mpxy, "SBI MPXY channel debug commands",
	"list                              - list discovered channels\n"
	"mpxy readattr    <chan> <attr>         - read one attribute\n"
	"mpxy readattrall <chan>                - read all attributes\n"
	"mpxy writeattr   <chan> <attr> <val>   - write an attribute\n"
	"mpxy send        <chan> <msg_id> [payload_word ...]  - send message\n");

#endif // CONFIG_CMD_MPXY
