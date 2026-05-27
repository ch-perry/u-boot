// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2024 Ventana Micro Systems Inc.
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#define LOG_CATEGORY UCLASS_RPMI

#include <asm/types.h>
#include <asm/io.h>
#include <asm/byteorder.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <rpmi.h>
#include "rpmi-sandbox.h"

struct rpmi_clock {
	enum rpmi_clock_type type;
	const char *name;
	u32 rate_count;
	const u64 *rate_array;
	u32 transition_latency;
	enum rpmi_clock_state state;
	u64 rate;
};

const u64 discrete_rates[] = { 20000000, 333333333, 500000000, 1000000000 };

const u64 linear_rates[] = { 50000000, 100000000, 1000000 };

#define DISCRETE_CLOCK(name, current_state, current_rate) \
	{ RPMI_CLK_TYPE_DISCRETE,                         \
	  name,                                           \
	  ARRAY_SIZE(discrete_rates),                     \
	  discrete_rates,                                 \
	  0,                                              \
	  current_state,                                  \
	  current_rate }

#define LINEAR_CLOCK(name, current_state, current_rate)         \
	{ RPMI_CLK_TYPE_LINEAR, name, ARRAY_SIZE(linear_rates), \
	  linear_rates,		0,    current_state,            \
	  current_rate }

struct rpmi_clock clock_simulation[] = {
	DISCRETE_CLOCK("CLK0", RPMI_CLK_STATE_DISABLED, discrete_rates[0]),
	DISCRETE_CLOCK("CLK1", RPMI_CLK_STATE_DISABLED, discrete_rates[1]),
	DISCRETE_CLOCK("CLK2", RPMI_CLK_STATE_ENABLED, discrete_rates[2]),
	LINEAR_CLOCK("CLK3", RPMI_CLK_STATE_ENABLED, linear_rates[0]),
};

static u32 clock_count = ARRAY_SIZE(clock_simulation);

static struct rpmi_clock *rpmi_get_clock(u32 clkid)
{
	if (clkid >= clock_count)
		return NULL;

	return &clock_simulation[clkid];
}

static enum rpmi_error_codes
rpmi_clock_set_rate(u32 clkid, enum rpmi_clock_rate_match match, u64 rate)
{
	struct rpmi_clock *clk = rpmi_get_clock(clkid);

	if (!clk)
		return RPMI_ERR_INVALID_PARAM;

	clk->rate = rate;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes rpmi_clock_set_state(u32 clkid,
						  enum rpmi_clock_state state)
{
	struct rpmi_clock *clk = rpmi_get_clock(clkid);

	if (!clk)
		return RPMI_ERR_INVALID_PARAM;

	clk->state = state;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes rpmi_clock_get_state(u32 clkid,
						  enum rpmi_clock_state *state)
{
	struct rpmi_clock *clk = rpmi_get_clock(clkid);

	if (!clk || !state)
		return RPMI_ERR_INVALID_PARAM;

	*state = clk->state;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes rpmi_clock_get_rate(u32 clkid, u64 *rate)
{
	struct rpmi_clock *clk = rpmi_get_clock(clkid);

	if (!clk || !rate)
		return RPMI_ERR_INVALID_PARAM;

	*rate = clk->rate;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_get_num_clocks(struct rpmi_transport *trans, u16 request_datalen,
			     const u8 *request_data, u16 *response_datalen,
			     u8 *response_data)
{
	u32 *resp = (void *)response_data;

	resp[0] = cpu_to_le32((u32)RPMI_SUCCESS);
	resp[1] = cpu_to_le32(clock_count);

	*response_datalen = 2 * sizeof(*resp);

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_get_attributes(struct rpmi_transport *trans, u16 request_datalen,
			     const u8 *request_data, u16 *response_datalen,
			     u8 *response_data)
{
	struct rpmi_clock *clk;
	u16 resp_dlen;
	u32 flags = 0;

	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	clk = rpmi_get_clock(clkid);
	if (!clk) {
		resp_dlen = sizeof(*resp);
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	/* encode CLOCK_FORMAT */
	if (clk->type == RPMI_CLK_TYPE_LINEAR)
		flags |= 1;

	resp[3] = cpu_to_le32(clk->transition_latency);
	resp[2] = cpu_to_le32(clk->rate_count);
	resp[1] = cpu_to_le32(flags);
	resp[0] = cpu_to_le32((u32)RPMI_SUCCESS);

	if (clk->name)
		strlcpy((char *)&resp[4], clk->name, RPMI_CLK_NAME_LEN);

	resp_dlen = 8 * sizeof(*resp);

done:
	*response_datalen = resp_dlen;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_get_supp_rates(struct rpmi_transport *trans, u16 request_datalen,
			     const u8 *request_data, u16 *response_datalen,
			     u8 *response_data)
{
	u32 i = 0, j = 0;
	u32 rate_count;
	u32 resp_dlen = 0, clk_rate_idx = 0;
	u32 max_rates, remaining = 0, returned = 0;
	const u64 *rate_array;
	struct rpmi_clock *clk;

	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	clk = rpmi_get_clock(clkid);
	if (!clk) {
		resp_dlen = sizeof(*resp);
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	rate_count = clk->rate_count;
	rate_array = clk->rate_array;
	if (!rate_count || !rate_array) {
		resp_dlen = sizeof(*resp);
		resp[0] = cpu_to_le32((u32)RPMI_ERR_NOTSUPP);
		goto done;
	}

	clk_rate_idx = cpu_to_le32(((const u32 *)request_data)[1]);

	if (clk->type == RPMI_CLK_TYPE_LINEAR) {
		/* max, min and step */
		for (i = 0; i < 3; i++) {
			resp[4 + 2 * i] = cpu_to_le32((u32)(rate_array[i]));
			resp[5 + 2 * i] =
				cpu_to_le32((u32)(rate_array[i] >> 32));
		}
		remaining = 0;
		returned = 3;
	} else if (clk->type == RPMI_CLK_TYPE_DISCRETE) {
		if (clk_rate_idx > rate_count) {
			resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
			resp_dlen = sizeof(*resp);
			goto done;
		}

		/* max rates a rpmi message can accommodate */
		max_rates = (trans->msg_data_size - (4 * sizeof(*resp))) /
			    sizeof(u64);
		remaining = rate_count - clk_rate_idx;
		if (remaining > max_rates)
			returned = max_rates;
		else
			returned = remaining;

		for (i = clk_rate_idx, j = 0;
		     i <= (clk_rate_idx + returned - 1); i++, j++) {
			resp[4 + 2 * j] = cpu_to_le32((u32)(rate_array[i]));
			resp[5 + 2 * j] =
				cpu_to_le32((u32)(rate_array[i] >> 32));
		}

		remaining = rate_count - (clk_rate_idx + returned);
	} else {
		return RPMI_ERR_FAILED;
	}

	resp[3] = cpu_to_le32((u32)returned);
	resp[2] = cpu_to_le32((u32)remaining);
	/* No flags currently supported */
	resp[1] = cpu_to_le32(0);
	resp[0] = cpu_to_le32((u32)RPMI_SUCCESS);

	resp_dlen = (4 * sizeof(*resp)) + (returned * sizeof(u64));

done:
	*response_datalen = resp_dlen;

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_set_config(struct rpmi_transport *trans, u16 request_datalen,
			 const u8 *request_data, u16 *response_datalen,
			 u8 *response_data)
{
	enum rpmi_error_codes status;
	u32 cfg, new_state;
	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	if (clkid >= clock_count) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	cfg = cpu_to_le32(((const u32 *)request_data)[1]);

	/* get command from 0th index bit in config field */
	new_state = (cfg & 0b1) ? RPMI_CLK_STATE_ENABLED :
				  RPMI_CLK_STATE_DISABLED;

	/* change clock config synchronously */
	status = rpmi_clock_set_state(clkid, new_state);
	resp[0] = cpu_to_le32((u32)status);

done:
	*response_datalen = sizeof(*resp);

	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_get_config(struct rpmi_transport *trans, u16 request_datalen,
			 const u8 *request_data, u16 *response_datalen,
			 u8 *response_data)
{
	u16 resp_dlen;
	enum rpmi_error_codes status;
	enum rpmi_clock_state state;
	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	if (clkid >= clock_count) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		resp_dlen = sizeof(*resp);
		goto done;
	}

	status = rpmi_clock_get_state(clkid, &state);
	if (status) {
		resp[0] = cpu_to_le32((u32)status);
		resp_dlen = sizeof(*resp);
		goto done;
	}

	/** RPMI config field only return enabled or disabled state */
	state = (state == RPMI_CLK_STATE_ENABLED) ? 1 : 0;

	resp[1] = cpu_to_le32((u32)state);
	resp[0] = cpu_to_le32((u32)RPMI_SUCCESS);

	resp_dlen = 2 * sizeof(*resp);

done:
	*response_datalen = resp_dlen;
	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_set_rate(struct rpmi_transport *trans, u16 request_datalen,
		       const u8 *request_data, u16 *response_datalen,
		       u8 *response_data)
{
	enum rpmi_error_codes status;
	enum rpmi_clock_rate_match rate_match;
	u32 flags;
	u32 rate_low, rate_hi;
	u64 rate_u64;
	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	if (clkid >= clock_count) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	flags = cpu_to_le32(((const u32 *)request_data)[1]);

	/* get rate match mode from flags */
	rate_match = flags & 0b11;
	if (rate_match >= RPMI_CLK_RATE_MATCH_MAX) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	rate_low = cpu_to_le32(((const u32 *)request_data)[2]);
	rate_hi = cpu_to_le32(((const u32 *)request_data)[3]);

	rate_u64 = ((u64)rate_hi << 32) | rate_low;
	if (rate_u64 == RPMI_CLOCK_RATE_INVALID || rate_u64 == 0) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		goto done;
	}

	status = rpmi_clock_set_rate(clkid, rate_match, rate_u64);
	resp[0] = cpu_to_le32((u32)status);

done:
	*response_datalen = sizeof(*resp);
	return RPMI_SUCCESS;
}

static enum rpmi_error_codes
rpmi_clock_sg_get_rate(struct rpmi_transport *trans, u16 request_datalen,
		       const u8 *request_data, u16 *response_datalen,
		       u8 *response_data)
{
	u16 resp_dlen;
	enum rpmi_error_codes status;
	u64 rate_u64;
	u32 *resp = (void *)response_data;

	u32 clkid = cpu_to_le32(((const u32 *)request_data)[0]);

	if (clkid >= clock_count) {
		resp[0] = cpu_to_le32((u32)RPMI_ERR_INVALID_PARAM);
		resp_dlen = sizeof(*resp);
		goto done;
	}

	status = rpmi_clock_get_rate(clkid, &rate_u64);
	if (status) {
		resp[0] = cpu_to_le32((u32)status);
		resp_dlen = sizeof(*resp);
		goto done;
	}

	resp[2] = cpu_to_le32((u32)(rate_u64 >> 32));
	resp[1] = cpu_to_le32((u32)(rate_u64));
	resp[0] = cpu_to_le32((u32)RPMI_SUCCESS);

	resp_dlen = 3 * sizeof(*resp);

done:
	*response_datalen = resp_dlen;
	return RPMI_SUCCESS;
}

static const struct rpmi_service rpmi_clock_services[RPMI_CLK_SRV_ID_MAX_COUNT] = {
	[RPMI_CLK_SRV_GET_NUM_CLOCKS] = {
		.service_id = RPMI_CLK_SRV_GET_NUM_CLOCKS,
		.min_a2p_request_datalen = 0,
		.process_a2p_request = rpmi_clock_sg_get_num_clocks,
	},
	[RPMI_CLK_SRV_GET_ATTRIBUTES] = {
		.service_id = RPMI_CLK_SRV_GET_ATTRIBUTES,
		.min_a2p_request_datalen = 4,
		.process_a2p_request = rpmi_clock_sg_get_attributes,
	},
	[RPMI_CLK_SRV_GET_SUPPORTED_RATES] = {
		.service_id = RPMI_CLK_SRV_GET_SUPPORTED_RATES,
		.min_a2p_request_datalen = 8,
		.process_a2p_request = rpmi_clock_sg_get_supp_rates,
	},
	[RPMI_CLK_SRV_SET_CONFIG] = {
		.service_id = RPMI_CLK_SRV_SET_CONFIG,
		.min_a2p_request_datalen = 8,
		.process_a2p_request = rpmi_clock_sg_set_config,
	},
	[RPMI_CLK_SRV_GET_CONFIG] = {
		.service_id = RPMI_CLK_SRV_GET_CONFIG,
		.min_a2p_request_datalen = 4,
		.process_a2p_request = rpmi_clock_sg_get_config,
	},
	[RPMI_CLK_SRV_SET_RATE] = {
		.service_id = RPMI_CLK_SRV_SET_RATE,
		.min_a2p_request_datalen = 16,
		.process_a2p_request = rpmi_clock_sg_set_rate,
	},
	[RPMI_CLK_SRV_GET_RATE] = {
		.service_id = RPMI_CLK_SRV_GET_RATE,
		.min_a2p_request_datalen = 4,
		.process_a2p_request = rpmi_clock_sg_get_rate,
	},
};

const struct rpmi_service_group rpmi_sandbox_clock_sg = {
	.name = "clk",
	.max_service_id = RPMI_CLK_SRV_GET_RATE,
	.servicegroup_version = RPMI_MKVER(1, 0),
	.services = rpmi_clock_services,
};
