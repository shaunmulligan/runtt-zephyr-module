/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */

/*
 * The confirm deadline: the reboot that makes MCUboot's revert actually happen.
 *
 * MCUboot reverts an unconfirmed image AT BOOT, and its own design document is
 * explicit that scheduling that boot is out of scope -- it describes only the
 * decision it makes at startup from the trailer flags. Nothing else schedules
 * it either. Zephyr's own OTA clients demonstrate the gap rather than close it:
 * hawkbit logs "Current image is not confirmed" and terminates
 * (HAWKBIT_UNCONFIRMED_IMAGE), and updatehub returns -EIO. Both leave the
 * device running the unconfirmed image.
 *
 * Observed end to end on a Pico 2 W, twice: an image built with
 * CONFIG_MCUMGR_TRANSPORT_UART=n boots and runs perfectly well -- USB
 * enumerates, both contract channels register, the application ticks -- it
 * simply cannot be talked to, so the host can never confirm it. The board then
 * ran it for 14 minutes with no sign of stopping, and reverted correctly the
 * instant an operator reset it. The safety property held only because a human
 * intervened.
 *
 * The usual fix does not fit us. The documented pattern is a LOCAL self-test:
 * boot, check yourself, then either boot_write_img_confirmed() or sys_reboot().
 * That works when the verdict is local. runtt's verdict is REMOTE by design --
 * confirmation travels over the very contract being tested, which is what makes
 * contract loss unrecoverable-proof -- so no self-test can produce it. The
 * firmware cannot know it is broken; it only knows nobody has confirmed it.
 * Hence a deadline rather than a self-test.
 */

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#if defined(CONFIG_RUNTT_WATCHDOG)
#include <zephyr/drivers/watchdog.h>
#endif
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(runtt_confirm, CONFIG_RUNTT_LOG_LEVEL);

static void deadline_expired(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(deadline_work, deadline_expired);

static void deadline_expired(struct k_work *work)
{
	ARG_UNUSED(work);

	/*
	 * Re-checked here rather than cancelled from the confirm path, so this
	 * file has no coupling to img_mgmt. A confirmed image just makes the
	 * deadline a no-op, which is the path every healthy deploy takes.
	 */
	if (boot_is_img_confirmed()) {
		LOG_DBG("confirmed before the deadline; nothing to do");
		return;
	}

	LOG_ERR("no confirm within %d s: rebooting so MCUboot reverts",
		CONFIG_RUNTT_CONFIRM_DEADLINE_SEC);

	/*
	 * Flush synchronously. Under deferred logging the message above sits in
	 * a buffer that the reset discards, and the one thing an operator needs
	 * from this event is the reason it happened.
	 */
	LOG_PANIC();

#if defined(CONFIG_RUNTT_WATCHDOG)
	/*
	 * Hand MCUboot a clean slate, where the SoC allows it.
	 *
	 * A watchdog armed here keeps counting THROUGH this reset -- measured on
	 * both RP2350 and nRF52840 -- so without this it would impose its
	 * remaining 6-8 s on MCUboot's swap and on the next image's startup,
	 * neither of which is feeding it. On RP2350 that caused a spurious
	 * revert of one deploy and reset another image 5.5 s after boot.
	 *
	 * nRF52840 returns -EPERM: that silicon has no WDT TASKS_STOP, so a
	 * running watchdog cannot be stopped by software at all. There it is
	 * MCUboot's BOOT_WATCHDOG_FEED that protects the swap, which is enabled
	 * by upstream default on Nordic. Not an error, so it is logged as a
	 * fact rather than a failure.
	 */
	{
		const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));

		if (device_is_ready(wdt)) {
			int err = wdt_disable(wdt);

			if (err == 0) {
				LOG_DBG("watchdog stopped before reboot");
			} else {
				LOG_INF("cannot stop the watchdog (%d); "
					"relying on MCUboot to feed it", err);
			}
		}
	}
#endif

	sys_reboot(SYS_REBOOT_COLD);
}

static int confirm_deadline_init(void)
{
	int swap;

	/*
	 * The steady state, and overwhelmingly the common one: a confirmed
	 * image arms nothing. This is why the deadline costs a board running
	 * settled firmware exactly nothing -- it exists only in the window
	 * between "new firmware booted" and "host confirmed it", which is the
	 * window that is already dangerous.
	 */
	if (boot_is_img_confirmed()) {
		return 0;
	}

	/*
	 * Only arm when a reboot would ACTUALLY revert. mcuboot_swap_type()
	 * reports the action MCUboot will take on the next reboot, so
	 * BOOT_SWAP_TYPE_REVERT is precisely the precondition for this deadline
	 * to help.
	 *
	 * Without this guard the deadline is a bootloop generator. An image
	 * flashed straight into the primary slot without --confirm (someone
	 * building their own, rather than deploying through runtt) also boots
	 * unconfirmed -- but with nothing staged in the secondary slot there is
	 * nothing to revert TO, so rebooting would just run the same image again
	 * and hit the same deadline, forever. Declining to arm leaves that board
	 * running and manageable, which is strictly better.
	 *
	 * A negative return means the trailer could not be read at all. Treat it
	 * the same way: do not arm on an unknown.
	 */
	swap = mcuboot_swap_type();
	if (swap != BOOT_SWAP_TYPE_REVERT) {
		LOG_WRN("unconfirmed image, but next boot would not revert "
			"(swap type %d): deadline not armed",
			swap);
		return 0;
	}

	LOG_INF("unconfirmed test image: %d s to be confirmed, or this board "
		"reboots and MCUboot reverts",
		CONFIG_RUNTT_CONFIRM_DEADLINE_SEC);

	k_work_schedule(&deadline_work, K_SECONDS(CONFIG_RUNTT_CONFIRM_DEADLINE_SEC));

	return 0;
}

SYS_INIT(confirm_deadline_init, APPLICATION, CONFIG_RUNTT_CONFIRM_DEADLINE_INIT_PRIORITY);
