/*
 * Copyright (c) 2020 Stephanos Ioannidis <root@stephanos.io>
 * Copyright (c) 2018 Xilinx, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <soc.h>
#include <drivers/timer/system_timer.h>
#include "xlnx_psttc_timer_priv.h"

#define TIMER_INDEX		CONFIG_XLNX_PSTTC_TIMER_INDEX
#define TIMER_DT(v)		UTIL_CAT(UTIL_CAT(DT_INST_, TIMER_INDEX), _##v)

#define TIMER_IRQ		TIMER_DT(XLNX_TTCPS_IRQ_0)
#define TIMER_BASE_ADDR		TIMER_DT(XLNX_TTCPS_BASE_ADDRESS)
#define TIMER_CLOCK_FREQUECY	TIMER_DT(XLNX_TTCPS_CLOCK_FREQUENCY)

#define TICKS_PER_SEC		CONFIG_SYS_CLOCK_TICKS_PER_SEC
#define CYCLES_PER_SEC		TIMER_CLOCK_FREQUECY
#define CYCLES_PER_TICK		(CYCLES_PER_SEC / TICKS_PER_SEC)

/*
 * CYCLES_NEXT_MIN must be large enough to ensure that the timer does not miss
 * interrupts.  This value was conservatively set using the trial and error
 * method, and there is room for improvement.
 */
#define CYCLES_NEXT_MIN		(10000)
#define CYCLES_NEXT_MAX		(XTTC_MAX_INTERVAL_COUNT)

BUILD_ASSERT(TIMER_DT(XLNX_TTCPS_CLOCK_FREQUENCY) ==
			CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
	     "Configured system timer frequency does not match the TTC "
	     "clock frequency in the device tree");

BUILD_ASSERT(CYCLES_PER_SEC >= TICKS_PER_SEC,
	     "Timer clock frequency must be greater than the system tick "
	     "frequency");

BUILD_ASSERT((CYCLES_PER_SEC % TICKS_PER_SEC) == 0,
	     "Timer clock frequency is not divisible by the system tick "
	     "frequency");

#ifdef CONFIG_TICKLESS_KERNEL
static u32_t last_cycles;
#endif

static u32_t read_count(void)
{
	/* Read current counter value */
	return sys_read32(TIMER_BASE_ADDR + XTTCPS_COUNT_VALUE_OFFSET);
}

static void update_match(u32_t cycles, u32_t match)
{
	u32_t delta = match - cycles;

	/* Ensure that the match value meets the minimum timing requirements */
	if (delta < CYCLES_NEXT_MIN) {
		match += CYCLES_NEXT_MIN - delta;
	}

	/* Write counter match value for interrupt generation */
	sys_write32(match, TIMER_BASE_ADDR + XTTCPS_MATCH_0_OFFSET);
}

static void ttc_isr(void *arg)
{
	u32_t cycles;
	u32_t ticks;

	ARG_UNUSED(arg);

	/* Acknowledge interrupt */
	sys_read32(TIMER_BASE_ADDR + XTTCPS_ISR_OFFSET);

	/* Read counter value */
	cycles = read_count();

#ifdef CONFIG_TICKLESS_KERNEL
	/* Calculate the number of ticks since last announcement  */
	ticks = (cycles - last_cycles) / CYCLES_PER_TICK;

	/* Update last cycles count */
	last_cycles = cycles;
#else
	/* Update counter match value for the next interrupt */
	update_match(cycles, cycles + CYCLES_PER_TICK);

	/* Advance tick count by 1 */
	ticks = 1;
#endif

	/* Announce to the kernel*/
	z_clock_announce(ticks);
}

int z_clock_driver_init(struct device *device)
{
	u32_t reg_val;

	/* Stop timer */
	sys_write32(XTTCPS_CNT_CNTRL_DIS_MASK,
		    TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

#ifdef CONFIG_TICKLESS_KERNEL
	/* Initialise internal states */
	last_cycles = 0;
#endif

	/* Initialise timer registers */
	sys_write32(XTTCPS_CNT_CNTRL_RESET_VALUE,
		    TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_CLK_CNTRL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_INTERVAL_VAL_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_0_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_1_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_MATCH_2_OFFSET);
	sys_write32(0, TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);
	sys_write32(XTTCPS_IXR_ALL_MASK, TIMER_BASE_ADDR + XTTCPS_ISR_OFFSET);

	/* Reset counter value */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val |= XTTCPS_CNT_CNTRL_RST_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	/* Set match mode */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val |= XTTCPS_CNT_CNTRL_MATCH_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	/* Set initial timeout */
	reg_val = IS_ENABLED(CONFIG_TICKLESS_KERNEL) ?
			CYCLES_NEXT_MAX : CYCLES_PER_TICK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_MATCH_0_OFFSET);

	/* Connect timer interrupt */
	IRQ_CONNECT(TIMER_IRQ, 0, ttc_isr, 0, 0);
	irq_enable(TIMER_IRQ);

	/* Enable timer interrupt */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);
	reg_val |= XTTCPS_IXR_MATCH_0_MASK;
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_IER_OFFSET);

	/* Start timer */
	reg_val = sys_read32(TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);
	reg_val &= (~XTTCPS_CNT_CNTRL_DIS_MASK);
	sys_write32(reg_val, TIMER_BASE_ADDR + XTTCPS_CNT_CNTRL_OFFSET);

	return 0;
}

void z_clock_set_timeout(s32_t ticks, bool idle)
{
#ifdef CONFIG_TICKLESS_KERNEL
	u32_t cycles;
	u32_t next_cycles;

	/* Read counter value */
	cycles = read_count();

	/* Calculate timeout counter value */
	if (ticks == K_TICKS_FOREVER) {
		next_cycles = cycles + CYCLES_NEXT_MAX;
	} else {
		next_cycles = cycles + ((u32_t)ticks * CYCLES_PER_TICK);
	}

	/* Set match value for the next interrupt */
	update_match(cycles, next_cycles);
#endif
}

u32_t z_clock_elapsed(void)
{
#ifdef CONFIG_TICKLESS_KERNEL
	u32_t cycles;

	/* Read counter value */
	cycles = read_count();

	/* Return the number of ticks since last announcement */
	return (cycles - last_cycles) / CYCLES_PER_TICK;
#else
	/* Always return 0 for tickful operation */
	return 0;
#endif
}

u32_t z_timer_cycle_get_32(void)
{
	/* Return the current counter value */
	return read_count();
}
