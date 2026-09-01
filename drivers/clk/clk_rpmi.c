// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_CLK

#include <asm/types.h>
#include <clk-uclass.h>
#include <dm.h>
#include <dm/device_compat.h>
#include <dm/device-internal.h>
#include <linux/clk-provider.h>
#include <rpmi.h>

struct rpmi_get_num_clocks_rx {
	__le32 status;
	__le32 num_clocks;
};

struct rpmi_get_rate_rx {
	__le32 status;
	__le32 lo;
	__le32 hi;
};

struct rpmi_get_config_rx {
	__le32 status;
	__le32 state;
};

struct rpmi_set_rate_tx {
	__le32 clkid;
	__le32 flags;
	__le32 lo;
	__le32 hi;
};

struct rpmi_set_config_tx {
	__le32 clkid;
	__le32 config;
};

struct rpmi_get_attrs_rx {
	__le32 status;
#define RPMI_CLK_TYPE_MASK GENMASK(1, 0)
	__le32 flags;
	__le32 num_rates;
	__le32 transition_latency;
	char name[RPMI_CLK_NAME_LEN];
};

struct rpmi_clk_def {
	u32 num_rates;
	enum rpmi_clock_type type;
	char name[RPMI_CLK_NAME_LEN + 1];
};

struct rpmi_clk_priv {
	struct rpmi_chan chan;
	u32 num_clocks;
	struct rpmi_clk_def *defs;
};

static int _rpmi_clk_enable_disable(struct rpmi_clk_priv *priv, u32 clkid,
				    bool ena)
{
	struct rpmi_set_config_tx tx = {
		.clkid = cpu_to_le32(clkid),
		.config = cpu_to_le32(ena ? RPMI_CLK_STATE_ENABLED :
					    RPMI_CLK_STATE_DISABLED),
	};
	__le32 rx;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_SET_CONFIG, &tx,
				  sizeof(tx), &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx);

	return rpmi_to_linux_error(status);
}

static ulong _rpmi_clk_get_rate(struct rpmi_clk_priv *priv, u32 clkid)
{
	__le32 tx = cpu_to_le32(clkid);
	struct rpmi_get_rate_rx rx;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_GET_RATE, &tx,
				  sizeof(tx), &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx.status);
	if (status)
		return rpmi_to_linux_error(status);

	return (((u64)(le32_to_cpu(rx.hi)) << 32) | (u32)(le32_to_cpu(rx.lo)));
}

static __maybe_unused int _rpmi_clk_get_config(struct rpmi_clk_priv *priv,
					       u32 clkid)
{
	__le32 tx = cpu_to_le32(clkid);
	struct rpmi_get_config_rx rx;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_GET_CONFIG, &tx,
				  sizeof(tx), &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx.status);
	if (status)
		return rpmi_to_linux_error(status);

	return le32_to_cpu(rx.state);
}

static int _rpmi_clk_set_rate(struct rpmi_clk_priv *priv, u32 clkid,
			      unsigned long rate)
{
	struct rpmi_set_rate_tx tx = {
		.clkid = cpu_to_le32(clkid),
		.flags = 0,
		.lo = cpu_to_le32(lower_32_bits(rate)),
		.hi = cpu_to_le32(upper_32_bits(rate)),
	};
	__le32 rx;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_SET_RATE, &tx,
				  sizeof(tx), &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx);

	return rpmi_to_linux_error(status);
}

static int _rpmi_clk_get_num_clocks(struct rpmi_clk_priv *priv)
{
	struct rpmi_get_num_clocks_rx rx;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_GET_NUM_CLOCKS,
				  NULL, 0, &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx.status);
	if (status)
		return rpmi_to_linux_error(status);

	priv->num_clocks = le32_to_cpu(rx.num_clocks);

	return 0;
}

static int rpmi_clk_enable(struct clk *clk)
{
	return _rpmi_clk_enable_disable(dev_get_plat(clk->dev),
					clk->id & CLK_ID_MSK, true);
}

static int rpmi_clk_disable(struct clk *clk)
{
	return _rpmi_clk_enable_disable(dev_get_plat(clk->dev),
					clk->id & CLK_ID_MSK, false);
}

static ulong rpmi_clk_get_rate(struct clk *clk)
{
	return _rpmi_clk_get_rate(dev_get_plat(clk->dev), clk->id & CLK_ID_MSK);
}

static ulong rpmi_clk_set_rate(struct clk *clk, ulong rate)
{
	int ret;

	ret = _rpmi_clk_set_rate(dev_get_plat(clk->dev), clk->id & CLK_ID_MSK,
				 rate);
	if (ret)
		return ret;

	return _rpmi_clk_get_rate(dev_get_plat(clk->dev), clk->id & CLK_ID_MSK);
}

static __maybe_unused void rpmi_clk_dump(struct udevice *dev)
{
	struct rpmi_clk_priv *priv = (struct rpmi_clk_priv *)dev_get_plat(dev);
	struct rpmi_clk_def *def;
	u64 clk_rate = 0;
	int i, state;

	printf("              Rate State  Id Name\n");
	printf("--------------------------------------\n");

	for (i = 0; i < priv->num_clocks; i++) {
		def = &priv->defs[i];
		clk_rate = _rpmi_clk_get_rate(priv, i);
		state = _rpmi_clk_get_config(priv, i);
		printf("%18lld %s %4i %s\n", clk_rate,
		       (state == RPMI_CLK_STATE_ENABLED) ? "   ON" : "  OFF", i,
		       def->name);
	}
}

static int rpmi_clk_xlate(struct clk *clk, struct ofnode_phandle_args *args)
{
	struct rpmi_clk_priv *priv = dev_get_plat(clk->dev);

	if (args->args_count != 1)
		return -EINVAL;

	if (args->args[0] >= priv->num_clocks)
		return -EINVAL;

	clk->id = CLK_ID(clk->dev, args->args[0]);
	clk->data = 0;

	return 0;
}

static int rpmi_clk_get_attrs(struct rpmi_clk_priv *priv, u32 clkid,
			      struct rpmi_clk_def *def)
{
	__le32 tx = cpu_to_le32(clkid);
	struct rpmi_get_attrs_rx rx;
	u8 format;
	int ret, status;

	ret = rpmi_send_with_resp(&priv->chan, RPMI_CLK_SRV_GET_ATTRIBUTES, &tx,
				  sizeof(tx), &rx, sizeof(rx), NULL);
	if (ret)
		return ret;

	status = le32_to_cpu(rx.status);
	if (status)
		return rpmi_to_linux_error(status);

	format = le32_to_cpu(rx.flags) & RPMI_CLK_TYPE_MASK;
	if (format >= RPMI_CLK_TYPE_MAX)
		return -EINVAL;

	def->type = format;
	def->num_rates = le32_to_cpu(rx.num_rates);
	memcpy(def->name, rx.name, RPMI_CLK_NAME_LEN);
	def->name[RPMI_CLK_NAME_LEN] = '\0';

	return 0;
}

static int rpmi_clk_probe(struct udevice *dev)
{
	struct rpmi_clk_priv *priv = (struct rpmi_clk_priv *)dev_get_plat(dev);
	struct rpmi_chan *chan = &priv->chan;
	int ret, i;

	ret = rpmi_open(chan);
	if (ret)
		return ret;

	ret = rpmi_check_versions(chan, RPMI_MKVER(1, 0), RPMI_SRVGRP_CLOCK,
				  RPMI_MKVER(1, 0));
	if (ret)
		goto err_out;

	ret = _rpmi_clk_get_num_clocks(priv);
	if (ret)
		goto err_out;

	priv->defs = calloc(priv->num_clocks, sizeof(struct rpmi_clk_def));
	if (!priv->defs) {
		ret = -ENOMEM;
		goto err_out;
	}

	for (i = 0; i < priv->num_clocks; i++) {
		ret = rpmi_clk_get_attrs(priv, i, &priv->defs[i]);
		if (ret)
			goto err_out;
	}

	dev_dbg(dev, "num clock = %i\n", priv->num_clocks);

	return 0;

err_out:
	free(priv->defs);
	rpmi_close(chan);

	return ret;
}

static int rpmi_clk_remove(struct udevice *dev)
{
	struct rpmi_clk_priv *priv = (struct rpmi_clk_priv *)dev_get_plat(dev);
	struct rpmi_chan *chan = &priv->chan;

	free(priv->defs);
	rpmi_close(chan);

	return 0;
}

static int rpmi_clk_to_plat(struct udevice *dev)
{
	struct rpmi_clk_priv *priv = (struct rpmi_clk_priv *)dev_get_plat(dev);

	return rpmi_get_by_index(dev, 0, &priv->chan);
}

static struct clk_ops rpmi_clk_ops = {
	.of_xlate = rpmi_clk_xlate,
	.set_rate = rpmi_clk_set_rate,
	.get_rate = rpmi_clk_get_rate,
	.enable = rpmi_clk_enable,
	.disable = rpmi_clk_disable,
#if IS_ENABLED(CONFIG_CMD_CLK)
	.dump = rpmi_clk_dump,
#endif
};

static const struct udevice_id rpmi_clk_ids[] = {
	{ .compatible = "riscv,rpmi-clock" },
	{}
};

U_BOOT_DRIVER(rpmi_clk) = {
	.name = "rpmi-clk",
	.id = UCLASS_CLK,
	.of_match = rpmi_clk_ids,
	.probe = rpmi_clk_probe,
	.remove = rpmi_clk_remove,
	.ops = &rpmi_clk_ops,
	.of_to_plat = rpmi_clk_to_plat,
	.plat_auto = sizeof(struct rpmi_clk_priv),
	.flags = DM_FLAG_PRE_RELOC,
};
