// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2026 Microchip Technology Inc. All rights reserved.
 */

#include <clk.h>
#include <dm.h>
#include <dm/device-internal.h>
#include <dm/test.h>
#include <rpmi.h>
#include <stdio.h>
#include <test/ut.h>

static int dm_test_rpmi_clock(struct unit_test_state *uts)
{
	struct udevice *rpmi_dev, *clk_dev;
	struct clk clk0, clk1, clk2, clk3;

	ut_assertok(uclass_get_device_by_name(UCLASS_RPMI, "rpmi", &rpmi_dev));

	ut_assertok(
		uclass_get_device_by_name(UCLASS_CLK, "rpmi-clock", &clk_dev));

	// Grab clock 0 and retrieve the rate
	ut_assertok(clk_get_by_index(clk_dev, 0, &clk0));
	ut_asserteq_64(20000000, clk_get_rate(&clk0));

	// Change to 1GHz
	ut_asserteq(1000000000, clk_set_rate(&clk0, 1000000000));

	// Revert back to 200MHz
	ut_asserteq(20000000, clk_set_rate(&clk0, 20000000));

	// Grab clock 1
	ut_assertok(clk_get_by_index(clk_dev, 1, &clk1));
	ut_asserteq_64(333333333, clk_get_rate(&clk1));

	// Grab clock 2
	ut_assertok(clk_get_by_index(clk_dev, 2, &clk2));
	ut_asserteq_64(500000000, clk_get_rate(&clk2));

	// Grab clock 3
	ut_assertok(clk_get_by_index(clk_dev, 3, &clk3));
	ut_asserteq_64(50000000, clk_get_rate(&clk3));

	return 0;
}
DM_TEST(dm_test_rpmi_clock, UTF_SCAN_FDT);
