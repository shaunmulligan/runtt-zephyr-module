# Development notes

**For agents and maintainers, not for users.** Findings, failures worth not
repeating, and the reasoning behind decisions that are not obvious from the code.
Nothing here is needed to *use* this module — for that, see the README and
[`docs/FIRMWARE_GUIDE.md`](docs/FIRMWARE_GUIDE.md).

Put development findings here rather than in the guide. The guide is what a
firmware author reads to get onto the platform; it should say what is true now
and how to use it, not how it came to be that way.

Much of this module's hard-won detail lives in Kconfig help text and source
comments instead, deliberately: it is read at the moment it is needed, and
`zephyr/Kconfig` is where someone looking at a symbol will look.

---

## `ZEPHYR_EXTRA_MODULES` used to be mandatory, and the split made it wrong

The guide told authors to pass `-DZEPHYR_EXTRA_MODULES=<path to runtt>`. That
was correct while this module was a subdirectory of the manifest repository:
west auto-discovers a `module.yml` only at a project's root, so a module nested
inside another project is invisible to it and has to be pointed at by hand.

Standing alone, it is declared in `runtt-boards`' `west.yml` as a project at
`modules/runtt`, so west registers it and the flag became not just unnecessary
but misleading — it suggests the manifest declaration is insufficient.

## Why `RUNTT_USB` exists rather than Zephyr's own initialisation

`CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` registers only the *first* CDC-ACM
instance — its own source comment says so. A board declaring both a management
and a log channel therefore enumerated with one interface and the log channel
never appeared. Observed on an RP2040 before this module existed, and the
symptom is confusing: the board works, `lsusb` looks right, and only the second
channel is missing.

## The system workqueue stack is not a tuning knob

`configdefault SYSTEM_WORKQUEUE_STACK_SIZE 2560` in `zephyr/Kconfig` exists
because both CDC-ACM instances and MCUmgr's `os reset` handler all submit work
to the system workqueue. Overflowing it kills the queue's thread, which takes
SMP, log output and the pending reboot with it — while USB interrupts keep
running on their own stack, so the board still answers control transfers and
looks healthy to `lsusb` while being entirely unmanageable. Observed on an
RP2040: an `os reset` with no upload bricked a board running runtt-idle until it
was physically replugged. The full reasoning is in the Kconfig comment.

---

## The watchdog inherits across a soft reset, on both SoC families

Measured 2026-09-04 on an Adafruit Feather nRF52840, and 2026-09-03 on a
Raspberry Pi Pico 2 W. This is the finding that shapes CONFIG_RUNTT_WATCHDOG,
and it was not what the design assumed.

**A watchdog armed by the application keeps counting through a soft reset.** It
therefore imposes its deadline on MCUboot's swap and on the next image's
startup, neither of which knows about it.

nRF52840, over SWD with no firmware involved -- arm the WDT, clear
POWER.RESETREAS, issue SYSRESETREQ (the same reset `os reset` performs), read
back:

| | RESETREAS | RUNSTATUS | CRV |
|---|---|---|---|
| armed, RESETREAS cleared | 0 | 1 | 0x1e0000 |
| after SYSRESETREQ | 0x4 SREQ | **1 -- survived** | preserved |
| after firing unfed | 0x6 DOG\|SREQ | **0 -- did not survive** | back to default |

Clearing RESETREAS first is what makes this trustworthy. The register is
cumulative, so the first attempt read 0x4 and proved nothing -- that bit was
already latched from an earlier reset.

**The blast radius is bounded, and that matters more than the survival itself.**
A DOG reset fully resets the WDT block, so an unfed watchdog fires exactly once
and stops. No bootloop, no power cycle. Verified: the board recovered on its own.

RP2350 behaves the same way. Read over SWD in an image that arms no watchdog at
all, immediately after one that armed 8 s: `ctrl=0x4754c200 ENABLE=1
TIME=5554688` and counting down -- 5.55 s of an 8 s period inherited. It then
fired, and the next boot reported the expiry.

### The consequence, and which board is actually exposed

| | survives soft reset | MCUboot feeds during swap | can disarm |
|---|---|---|---|
| nRF52840 | yes | **yes**, upstream default | **no** (no TASKS_STOP) |
| RP2350 | yes | **no** | yes |

MCUboot's `BOOT_WATCHDOG_FEED` is `default y if SOC_FAMILY_NORDIC_NRF`
*unconditionally* -- not merely `default y if WATCHDOG` -- and implies
`BOOT_WATCHDOG_FEED_NRFX_WDT`. Upstream already assumes a watchdog runs through
the bootloader on Nordic. Verified in a fresh Feather build:
`BOOT_WATCHDOG_FEED=y`, `BOOT_WATCHDOG_FEED_NRFX_WDT=y`, `NRFX_WDT=y`.

So the nRF52840's inability to disarm turns out **not** to matter, and the Pico
is the exposed one: same inheritance, no feeding. On RP2350 an inherited 8 s
watchdog interrupted MCUboot mid-swap and produced a confirmed primary image
with no revert target -- a test image made permanent, which is the opposite of
what the deadline exists to guarantee.

Measured on the Feather, with the feeding in place: an 8 s watchdog counted
through a swap of an **82 KB** image and never fired --
`RESETREAS=0x4 SREQ` only, `RUNSTATUS=1` at the new image's boot, so it really
was live across both the reset and the swap.

### MCUboot cannot feed the watchdog on RP2, and that took a wrong turn to find

The obvious fix for the Pico looked like enabling `CONFIG_WATCHDOG=y` in its
MCUboot image, so MCUboot's `BOOT_WATCHDOG_FEED` (`default y if WATCHDOG`) would
fire the way Nordic's does. It builds, the Kconfig comes out right --
`BOOT_WATCHDOG_FEED=y` -- and `wdt0` is already `status = "okay"` in MCUboot's
devicetree via the board file, so the device resolves.

It still does nothing, and the driver says why:

```c
static int wdt_rpi_pico_feed(const struct device *dev, int channel_id)
{
	if (data->enabled == false) {
		/* Watchdog is not running so does not need to be fed */
		return -EINVAL;
	}
	watchdog_hw->load = data->load;
}
```

MCUboot links its own instance of the driver, with its own `data`. It never
called `wdt_setup`, so `enabled` is false and `wdt_feed()` returns `-EINVAL`.
Even if it did not, `data->load` is only set by `wdt_install_timeout`, so a feed
that went through would reload the counter with **zero**. There is no API in
Zephyr for "feed a watchdog somebody else started".

The remaining lever, `BOOT_WATCHDOG_SETUP_AT_BOOT`, would have MCUboot arm a
watchdog of its own before chainloading -- and then bootloop every application
that does not know to feed it, which is every application not built with this
module. Not acceptable as a default.

So the Pico is fixed from the other end: the application stops the watchdog when
the host asks for a reset (`CONFIG_RUNTT_WATCHDOG_DISARM_ON_RESET`, using
MCUmgr's `MGMT_EVT_OP_OS_MGMT_RESET` hook). That covers the deploy path, which
is the one that matters, and `wdt_disable()` works on RP2. nRF52840 cannot do it
and does not need to.

The wrong turn is recorded because the Kconfig looked correct at every step --
the option existed, the default fired, the devicetree node was enabled -- and
only the driver source showed it was inert. Reading the generated `.config` was
not enough here; the implementation had to be read too.

### The residual footgun

An image that does **not** arm a watchdog, deployed immediately after one that
does, inherits a live countdown and is reset once, 6-8 s after boot, through no
fault of its own. Bounded to a single reset by the DOG-reset behaviour above,
but it is a spurious reset of good firmware.

Mitigated where the SoC allows it: `src/confirm.c` disables the watchdog before
its own `sys_reboot()`. RP2350 supports `wdt_disable()`; nRF52840 returns
`-EPERM`, so there it is MCUboot's feeding or nothing.

### How the earlier wrong conclusion happened

The first RP2350 experiment appeared to show the watchdog was irrelevant: a
panicking image recovered in ~11 s with `RUNTT_WATCHDOG=n`. It was measuring the
wrong thing. The panic was 500 ms after boot, which lands *inside* the swap and
boot-path window, so what recovered the board was the inherited watchdog
interrupting MCUboot -- not fault recovery. Moving the panic to 20-30 s, clear of
that window, produced the real result: with no watchdog the halt is permanent,
with one the board reboots and MCUboot reverts.

The lesson worth keeping: **a fault-injection test has to fire after the boot
path has settled, or it measures the bootloader rather than the application.**

---

*Co-authored with Claude*
