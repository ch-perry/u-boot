// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_RPMI

#include <asm/io.h>
#include <asm/types.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <linux/delay.h>
#include <linux/log2.h>
#include <log.h>
#include <malloc.h>
#include <rpmi-uclass.h>

#define RPMI_SLOT_SIZE_MIN 64

enum rpmi_queue_idx {
	RPMI_QUEUE_IDX_A2P_REQ = 0,
	RPMI_QUEUE_IDX_P2A_ACK = 1,
	RPMI_QUEUE_IDX_P2A_REQ = 2,
	RPMI_QUEUE_IDX_A2P_ACK = 3,
	RPMI_QUEUE_IDX_MAX_COUNT = 4,
	RPMI_A2P_DOORBELL = 5,
};

struct shmem_queue {
	u32 num_slot;
	__le32 *headptr;
	__le32 *tailptr;
	void *buffer;
};

struct rpmi_shmem {
	struct udevice *dev;
	struct shmem_queue a2p_req;
	struct shmem_queue p2a_ack;
	int slot_size;
	int rx_timeout_ms;
	u32 token;
	void *doorbell;
	u32 doorbell_value;
	u32 spec_version;
	u32 impl_version;
	u32 impl_id;
};

struct rpmi_shmem_chan_priv {
	struct rpmi_shmem *priv;
	u32 servicegroup_id;
	u32 servicegroup_version;
};

static int rpmi_shmem_queue_idx(const char *name)
{
	static const struct {
		int id;
		const char *name;
	} id_names[] = {
		{ RPMI_QUEUE_IDX_A2P_REQ, "a2p-req" },
		{ RPMI_QUEUE_IDX_P2A_ACK, "p2a-ack" },
		{ RPMI_QUEUE_IDX_P2A_REQ, "p2a-req" },
		{ RPMI_QUEUE_IDX_A2P_ACK, "a2p-ack" },
		{ RPMI_A2P_DOORBELL, "a2p-doorbell" },
	};

	for (int i = 0; i < ARRAY_SIZE(id_names); i++) {
		if (strcmp(name, id_names[i].name) == 0)
			return id_names[i].id;
	}

	return -ENODEV;
}

static u32 rpmi_read_head(struct shmem_queue *qctx)
{
	return le32_to_cpu(readl(qctx->headptr));
}

static u32 rpmi_read_tail(struct shmem_queue *qctx)
{
	return le32_to_cpu(readl(qctx->tailptr));
}

static void rpmi_write_head(struct shmem_queue *qctx, u32 val)
{
	writel(cpu_to_le32(val), qctx->headptr);
}

static void rpmi_write_tail(struct shmem_queue *qctx, u32 val)
{
	writel(cpu_to_le32(val), qctx->tailptr);
}

static bool rpmi_smq_queue_full(struct shmem_queue *qctx)
{
	return ((rpmi_read_tail(qctx) + 1) % qctx->num_slot ==
		rpmi_read_head(qctx));
}

/* 31                                            0
 * +---------------------+-----------------------+
 * | FLAGS | SERVICE_ID  |   SERVICEGROUP_ID     |
 * +---------------------+-----------------------+
 * |        TOKEN        |     DATA LENGTH       |
 * +---------------------+-----------------------+
 * |                 DATA/PAYLOAD                |
 * +---------------------------------------------+
 */
struct rpmi_message_header {
	__le16 servicegroup_id;
	u8 service_id;
	u8 flags;
	__le16 datalen;
	__le16 token;
} __packed;

static int rpmi_smq_rx(struct rpmi_shmem *priv, struct shmem_queue *qctx,
		       u32 service_group_id, u32 service_id, u16 token,
		       void *response, unsigned long max_response_len,
		       unsigned long *out_response_len)
{
	struct rpmi_message_header *msg;
	u32 dlen, headidx, tailidx;
	int rxretry = 0;
	void *src;

	headidx = rpmi_read_head(qctx);

	/* Wait for the Rx message with matching token, discard any other
	 * messages received in the mean time
	 */
	while (true) {
		tailidx = rpmi_read_tail(qctx);

		if (headidx != tailidx) {
			msg = (void *)qctx->buffer +
			      (headidx * priv->slot_size);
			if ((le16_to_cpu(msg->token) == token) &&
			    (le16_to_cpu(msg->servicegroup_id) ==
			     service_group_id) &&
			    (msg->service_id == service_id))
				break;

			headidx = (headidx + 1) % qctx->num_slot;
		}

		mdelay(1);
		rxretry += 1;
		if (rxretry > priv->rx_timeout_ms)
			return -ETIMEDOUT;
	}

	/* Extract data from the message */
	if (response) {
		dlen = le16_to_cpu(msg->datalen);
		if (dlen > priv->slot_size - sizeof(struct rpmi_message_header))
			return -EIO;
		if (dlen > max_response_len)
			dlen = max_response_len;
		if (out_response_len)
			*out_response_len = dlen;
		src = (void *)msg + sizeof(struct rpmi_message_header);
		memcpy_fromio(response, src, dlen);
	}

	/* Update the head index */
	headidx = (headidx + 1) % qctx->num_slot;
	rpmi_write_head(qctx, headidx);

	return 0;
}

static int rpmi_smq_tx(struct rpmi_shmem *priv, struct shmem_queue *qctx,
		       u32 service_group_id, u32 service_id,
		       enum rpmi_message_type type, u16 token,
		       const void *request, unsigned long request_len)
{
	struct rpmi_message_header header = { 0 };
	u32 tailidx;
	void *dst;

	/* Tx sanity checks */
	if (request_len >
	    (priv->slot_size - sizeof(struct rpmi_message_header)))
		return -EINVAL;

	if (rpmi_smq_queue_full(qctx))
		return -ENOMEM;

	tailidx = rpmi_read_tail(qctx);

	/* Prepare the header to be written into the slot */
	header.servicegroup_id = cpu_to_le16(service_group_id);
	header.service_id = service_id;
	header.flags = type;
	header.datalen = cpu_to_le16(request_len);
	header.token = cpu_to_le16(token);

	/* Write header into the slot */
	dst = (char *)qctx->buffer + (tailidx * priv->slot_size);
	memcpy_toio(dst, &header, sizeof(header));
	dst += sizeof(header);

	/* Write data into the slot */
	if (request)
		memcpy_toio(dst, request, request_len);

	/* Update the tail/write index */
	tailidx = (tailidx + 1) % qctx->num_slot;
	rpmi_write_tail(qctx, tailidx);

	/* Ring the RPMI doorbell if present */
	if (priv->doorbell)
		writel(priv->doorbell_value, priv->doorbell);

	return 0;
}

static int rpmi_shmem_request_priv(struct rpmi_shmem *priv, u32 servicegroup_id,
				   u32 service_id, const void *request,
				   unsigned long request_len, void *response,
				   unsigned long max_response_len,
				   unsigned long *out_response_len)
{
	enum rpmi_message_type type = response ? RPMI_MSG_NORMAL_REQUEST :
						 RPMI_MSG_POSTED_REQUEST;
	int err;

	if (!request && request_len)
		return -EINVAL;

	err = rpmi_smq_tx(priv, &priv->a2p_req, servicegroup_id, service_id,
			  type, priv->token, request, request_len);
	if (err) {
		dev_err(priv->dev, "tx failed %i\n", err);
		goto out;
	}

	if (response) {
		err = rpmi_smq_rx(priv, &priv->p2a_ack, servicegroup_id,
				  service_id, priv->token, response,
				  max_response_len, out_response_len);
		if (err) {
			dev_err(priv->dev, "rx failed %i\n", err);
			goto out;
		}
	}

out:
	priv->token++;
	return err;
}

static int rpmi_shmem_request(struct rpmi_chan *chan, u32 service_id,
			      const void *request, unsigned long request_len,
			      void *response, unsigned long max_response_len,
			      unsigned long *out_response_len)
{
	struct rpmi_shmem *priv = dev_get_priv(chan->dev);
	struct rpmi_shmem_chan_priv *chan_priv = chan->con_priv;

	return rpmi_shmem_request_priv(priv, chan_priv->servicegroup_id,
				       service_id, request, request_len,
				       response, max_response_len,
				       out_response_len);
}

static int rpmi_shmem_get_attr(struct rpmi_chan *chan,
			       enum rpmi_attribute_id id, u32 *data)
{
	struct rpmi_shmem *priv = dev_get_priv(chan->dev);
	struct rpmi_shmem_chan_priv *chan_priv = chan->con_priv;

	switch (id) {
	case RPMI_ATTR_SPEC_VERSION:
		*data = priv->spec_version;
		break;
	case RPMI_ATTR_MAX_MSG_DATA_SIZE:
		*data = priv->slot_size - sizeof(struct rpmi_message_header);
		break;
	case RPMI_ATTR_SERVICEGROUP_ID:
		*data = chan_priv->servicegroup_id;
		break;
	case RPMI_ATTR_SERVICEGROUP_VERSION:
		*data = chan_priv->servicegroup_version;
		break;
	case RPMI_ATTR_IMPL_ID:
		*data = priv->impl_id;
		break;
	case RPMI_ATTR_IMPL_VERSION:
		*data = priv->impl_version;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int rpmi_shmem_base_service_req(struct rpmi_shmem *priv, u32 service_id,
				       const u32 *arg_in, u32 *arg_out)
{
	int len = 0;
	u32 rx[2];
	u32 tx[1];
	int ret;

	if (arg_in) {
		tx[0] = cpu_to_le32(*arg_in);
		len = sizeof(*arg_in);
	}

	ret = rpmi_shmem_request_priv(priv, RPMI_SRVGRP_BASE, service_id, tx,
				      len, rx, sizeof(rx), NULL);
	if (ret) {
		dev_err(priv->dev, "BASE service %i failed (%i)\n", service_id,
			ret);
		return ret;
	}

	if (rx[0]) {
		ret = rpmi_to_linux_error(le32_to_cpu(rx[0]));
		dev_err(priv->dev, "BASE service %i returned code %i\n",
			service_id, ret);
		return ret;
	}

	if (arg_out)
		*arg_out = le32_to_cpu(rx[1]);

	return 0;
}

static int rpmi_shmem_of_xlate(struct rpmi_chan *chan,
			       struct ofnode_phandle_args *args)
{
	if (args->args_count != 1)
		return -EINVAL;

	// args[0] is the servicegroup id.
	chan->id = args->args[0];

	return 0;
}

static int rpmi_shmem_open(struct rpmi_chan *chan)
{
	struct rpmi_shmem *priv = dev_get_priv(chan->dev);
	u32 servicegroup_id, servicegroup_version;
	struct rpmi_shmem_chan_priv *chan_priv;
	int ret;

	servicegroup_id = chan->id;

	ret = rpmi_shmem_base_service_req(priv,
					  RPMI_BASE_SRV_PROBE_SERVICE_GROUP,
					  &servicegroup_id,
					  &servicegroup_version);
	if (ret)
		return ret;

	chan_priv = malloc(sizeof(*chan_priv));
	if (!chan_priv)
		return -ENOMEM;

	chan->con_priv = chan_priv;

	chan_priv->priv = priv;
	chan_priv->servicegroup_id = servicegroup_id;
	chan_priv->servicegroup_version = servicegroup_version;

	return 0;
}

static void rpmi_shmem_close(struct rpmi_chan *chan)
{
	free(chan->con_priv);
}

static int rpmi_shmem_probe(struct udevice *dev)
{
	struct rpmi_shmem *priv = dev_get_priv(dev);
	int i, ret, qidx, qcount, qslotcnt;
	struct shmem_queue *queue;
	u32 arg_in, arg_out;
	const char *qname;
	fdt_size_t qsize;
	void *start;

	priv->dev = dev;
	priv->rx_timeout_ms = 20;

	priv->slot_size = dev_read_u32_default(dev, "riscv,slot-size", 0);
	if (priv->slot_size < RPMI_SLOT_SIZE_MIN ||
	    !is_power_of_2(priv->slot_size))
		return -EINVAL;

	priv->doorbell_value =
		dev_read_u32_default(dev, "riscv,a2p-doorbell-value", 1);

	qcount = dev_read_string_count(dev, "reg-names");
	if (qcount < 0 || qcount > 5)
		return -EINVAL;

	for (i = 0; i < qcount; i++) {
		ret = dev_read_string_index(dev, "reg-names", i, &qname);
		if (ret)
			continue;

		qidx = rpmi_shmem_queue_idx(qname);
		if (qidx < 0)
			continue;

		start = dev_read_addr_size_index_ptr(dev, i, &qsize);
		if (!start)
			continue;

		if (qidx == RPMI_A2P_DOORBELL) {
			priv->doorbell = start;
		} else {
			qslotcnt = qsize / priv->slot_size;
			if (qslotcnt < 4)
				continue;

			if (qidx == RPMI_QUEUE_IDX_A2P_REQ)
				queue = &priv->a2p_req;
			else if (qidx == RPMI_QUEUE_IDX_P2A_ACK)
				queue = &priv->p2a_ack;
			else
				continue;

			queue->headptr = start;
			queue->tailptr = start + priv->slot_size;
			queue->buffer = start + (2 * priv->slot_size);
			queue->num_slot = qslotcnt - 2;
		}
	}

	if (!priv->a2p_req.num_slot || !priv->p2a_ack.num_slot)
		return -EINVAL;

	arg_in = RPMI_SRVGRP_BASE;
	ret = rpmi_shmem_base_service_req(
		priv, RPMI_BASE_SRV_PROBE_SERVICE_GROUP, &arg_in, &arg_out);
	if (ret)
		return ret;

	if (arg_out < RPMI_MKVER(1, 0))
		return -EINVAL;

	ret = rpmi_shmem_base_service_req(
		priv, RPMI_BASE_SRV_GET_IMPLEMENTATION_VERSION, NULL, &arg_out);
	if (ret)
		return ret;
	priv->impl_version = arg_out;

	ret = rpmi_shmem_base_service_req(
		priv, RPMI_BASE_SRV_GET_IMPLEMENTATION_IDN, NULL, &arg_out);
	if (ret)
		return ret;
	priv->impl_id = arg_out;

	ret = rpmi_shmem_base_service_req(priv, RPMI_BASE_SRV_GET_SPEC_VERSION,
					  NULL, &arg_out);
	if (ret)
		return ret;
	priv->spec_version = arg_out;

	return 0;
}

static const struct udevice_id rpmi_shmem_ids[] = {
	{ .compatible = "riscv,rpmi-shmem-mbox" },
	{}
};

static const struct rpmi_transport_ops rpmi_shmem_ops = {
	.of_xlate = rpmi_shmem_of_xlate,
	.get_attr = rpmi_shmem_get_attr,
	.request = rpmi_shmem_request,
	.open = rpmi_shmem_open,
	.close = rpmi_shmem_close,
};

U_BOOT_DRIVER(rpmi_shmem) = {
	.name = "rpmi-shmem",
	.id = UCLASS_RPMI,
	.of_match = rpmi_shmem_ids,
	.probe = rpmi_shmem_probe,
	.priv_auto = sizeof(struct rpmi_shmem),
	.ops = &rpmi_shmem_ops,
	.flags = DM_FLAG_PRE_RELOC,
};
