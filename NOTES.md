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

*Co-authored with Claude*
