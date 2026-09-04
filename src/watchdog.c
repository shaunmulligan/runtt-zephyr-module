/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

/*
 * Layer 2: a hardware watchdog, for the failure the confirm deadline cannot see.
 *
 * CONFIG_RUNTT_CONFIRM_DEADLINE closes the case where firmware boots, runs, and
 * simply cannot be reached: a delayed work item notices it was never confirmed
 * and reboots. That depends on the kernel still scheduling work.
 *
 * It therefore cannot close the case where the kernel stops. Zephyr's default
 * fatal handler HALTS -- arch_system_halt(), kernel/fatal.c -- rather than
 * rebooting, so an image that hard faults sits there forever with the CPU
 * stopped. No deadline fires, because nothing runs. Same for a deadlock that
 * wedges the system workqueue.
 *
 * A hardware watchdog is the only thing that survives that, because it is not
 * software. Armed at init and fed from the system workqueue, it reboots the
 * board when the workqueue stops making progress -- and on that boot MCUboot
 * reverts an unconfirmed image, which is the property this exists to protect.
 *
 * WHAT THIS DOES NOT DO, so it is not mistaken for more than it is: it cannot
 * help firmware that does not carry this module, and it cannot help firmware
 * that faults before this file's SYS_INIT runs. Covering those would mean
 * arming a watchdog in MCUboot before it jumps to the application -- and that
 * would bootloop any firmware that does not know to feed it, which is every
 * application not built with runtt. Deliberately not done.
 *
 * The feed is a liveness proxy rather than a correctness one. It proves the
 * system workqueue is scheduling, which is exactly the thing the contract needs
 * -- both CDC-ACM channels and MCUmgr's os-reset handler run there, so a
 * workqueue that has stopped means SMP is already dead and the board is already
 * unmanageable. The watchdog fires when the device is beyond saving by any
 * other means, which is the right moment for a reboot.
 */

#include <runtt/health.h>
#include <zephyr/drivers/watchdog.h>
#if defined(CONFIG_RUNTT_WATCHDOG_DISARM_ON_RESET)
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/grp/os_mgmt/os_mgmt_callbacks.h>
#endif
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(runtt_wdt, CONFIG_RUNTT_LOG_LEVEL);

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

#define FEED_INTERVAL K_MSEC(CONFIG_RUNTT_WATCHDOG_TIMEOUT_MS / 4)

static void feed(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(feed_work, feed);

static void feed(struct k_work *work)
{
	ARG_UNUSED(work);

	/*
	 * When the application has opted into runtt_health_feed(), a stalled
	 * application stops the watchdog being fed even though the kernel is
	 * fine. That turns "the kernel is scheduling" into "the application is
	 * doing work", which is the stronger guarantee and the whole point of
	 * that API.
	 *
	 * Firmware that never calls runtt_health_feed() reports healthy, so
	 * this reduces to the kernel-liveness case rather than reverting
	 * anything by surprise.
	 */
	if (IS_ENABLED(CONFIG_RUNTT_HEALTH) && !runtt_health_ok()) {
		LOG_WRN("application has not fed the health watchdog; "
			"letting the hardware watchdog expire");
		return;
	}

	(void)wdt_feed(wdt, wdt_channel);
	k_work_schedule(&feed_work, FEED_INTERVAL);
}

#if defined(CONFIG_RUNTT_WATCHDOG_DISARM_ON_RESET)
/*
 * Stop the watchdog when the host asks for a reset, so MCUboot gets a clean
 * slate to swap in.
 *
 * This is the deploy path, and it is the one that matters: runtt marks an image
 * test and then sends `os reset`. The watchdog armed here keeps counting
 * through that reset -- measured on RP2350 and nRF52840 -- so MCUboot inherits
 * whatever is left of the period and has to swap an entire image inside it. On
 * RP2350 that interrupted a swap and left a test image confirmed with no revert
 * target.
 *
 * MCUboot cannot solve it on RP2. Its BOOT_WATCHDOG_FEED path calls
 * wdt_feed(), and Zephyr's RP2 driver refuses to feed a watchdog its own
 * instance did not arm -- `if (data->enabled == false) return -EINVAL`, and
 * data->load would be 0 in any case, so a feed that did go through would reload
 * the counter with zero. There is no API for "feed a watchdog somebody else
 * started". Having MCUboot arm its own instead (BOOT_WATCHDOG_SETUP_AT_BOOT)
 * would bootloop every application that does not know to feed it.
 *
 * So the application disarms it on the way out. nRF52840 cannot -- no WDT
 * TASKS_STOP, wdt_disable() returns -EPERM -- and does not need to, because
 * MCUboot's nrfx feed path works there and is on by upstream default. Verified
 * on a Feather: an 8 s watchdog counted through a swap of an 82 KB image and
 * never fired.
 */
static enum mgmt_cb_return reset_requested(uint32_t event, enum mgmt_cb_return prev_status,
					    int32_t *rc, uint16_t *group, bool *abort_more,
					    void *data, size_t data_size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(rc);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (device_is_ready(wdt)) {
		int err = wdt_disable(wdt);

		if (err == 0) {
			LOG_INF("watchdog stopped for the reset, so MCUboot can swap");
		} else {
			/* -EPERM on nRF52840, which has no way to stop it. */
			LOG_INF("cannot stop the watchdog (%d); MCUboot must feed it",
				err);
		}
	}

	/* Never block the reset: a deploy failing because we could not disarm
	 * would be a worse outcome than an inherited countdown.
	 */
	return prev_status;
}

static struct mgmt_callback reset_callback = {
	.callback = reset_requested,
	.event_id = MGMT_EVT_OP_OS_MGMT_RESET,
};
#endif /* CONFIG_RUNTT_WATCHDOG_DISARM_ON_RESET */

static int watchdog_init(void)
{
	int err;

	const struct wdt_timeout_cfg cfg = {
		.window.min = 0U,
		.window.max = CONFIG_RUNTT_WATCHDOG_TIMEOUT_MS,
		.callback = NULL,
		/*
		 * A full SoC reset, not a core reset. The point is to re-enter
		 * MCUboot so it can revert; resetting the core alone would not
		 * necessarily do that.
		 */
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (!device_is_ready(wdt)) {
		LOG_ERR("watchdog device not ready; running without one");
		return 0;
	}

	err = wdt_install_timeout(wdt, &cfg);
	if (err < 0) {
		/*
		 * -EINVAL here is very likely the timeout being out of range.
		 * RP2040 is the tight one: errata RP2040-E1 halves the maximum,
		 * so its ceiling is about 8.3 s where RP2350 allows about 16.8.
		 */
		LOG_ERR("wdt_install_timeout(%d ms) failed: %d; "
			"running without a watchdog",
			CONFIG_RUNTT_WATCHDOG_TIMEOUT_MS, err);
		return 0;
	}
	wdt_channel = err;

	/*
	 * WDT_OPT_PAUSE_HALTED_BY_DBG is a correctness requirement, not a
	 * convenience, and the default is the wrong way round.
	 *
	 * Without this flag Zephyr sets the "keep running while halted by the
	 * debugger" behaviour (wdt_nrfx.c, wdt_rpi_pico.c). Combine that with
	 * an SWD session that halts the core -- to inspect it, or to flash it
	 * -- and the watchdog resets the board mid-operation. On the Feather
	 * that is the recovery path itself: SWD is the only way in, and its
	 * flashing destroys a bootloader with no ROM loader behind it.
	 *
	 * Worse on nRF52840, where it cannot be undone: that silicon has no
	 * WDT TASKS_STOP at all (verified -- NRF_WDT_Type carries only
	 * TASKS_START, and NRF_WDT_HAS_STOP is 0), so wdt_disable() returns
	 * -EPERM and a running watchdog stays running until a reset.
	 */
	err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
	if (err < 0) {
		LOG_ERR("wdt_setup failed: %d; running without a watchdog", err);
		return 0;
	}

	LOG_INF("hardware watchdog armed: %d ms, fed every %d ms",
		CONFIG_RUNTT_WATCHDOG_TIMEOUT_MS,
		CONFIG_RUNTT_WATCHDOG_TIMEOUT_MS / 4);

	k_work_schedule(&feed_work, FEED_INTERVAL);

#if defined(CONFIG_RUNTT_WATCHDOG_DISARM_ON_RESET)
	mgmt_callback_register(&reset_callback);
#endif

	return 0;
}

SYS_INIT(watchdog_init, APPLICATION, CONFIG_RUNTT_WATCHDOG_INIT_PRIORITY);
