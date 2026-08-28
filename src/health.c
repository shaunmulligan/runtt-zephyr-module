/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 */
#include <balena_mcu/health.h>
#include <zephyr/kernel.h>

/*
 * A software watchdog, not a hardware one. The hardware watchdog case belongs
 * to the bootloader-to-application boundary and is per-SoC; this is the much
 * smaller thing the host's confirm gate can consult.
 */
#define HEALTH_WINDOW K_SECONDS(30)

static atomic_t last_feed_ticks;

void balena_mcu_health_feed(void)
{
	atomic_set(&last_feed_ticks, (atomic_val_t)k_uptime_get_32());
}

bool balena_mcu_health_ok(void)
{
	uint32_t last = (uint32_t)atomic_get(&last_feed_ticks);

	/* Never fed: treat as healthy rather than failing closed. Firmware that
	 * does not opt in must not be reported as broken.
	 */
	if (last == 0U) {
		return true;
	}

	return (k_uptime_get_32() - last) < (uint32_t)k_ticks_to_ms_floor32(HEALTH_WINDOW.ticks);
}
