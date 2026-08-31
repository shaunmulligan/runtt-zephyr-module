# Firmware guide

How to take a Zephyr application and make it deployable as a container image.

The honesty test for this design: a competent Zephyr developer with existing
firmware should get onto the platform by **adding a module to `west.yml`, adding
one build flag, and wrapping it in a two-stage Dockerfile.** Under an hour, no
source changes. If you find yourself editing
application code to satisfy the platform, something has gone wrong — tell us.

Read `docs/WIRE_CONTRACT.md` for what the runtime requires on the wire. This
document is the practical side: how to arrange your tree so you get it for free.

## What you need before starting

* Zephyr **v4.4.2** (pin it exactly — see "Zephyr version policy" below)
* A board with an **MCUboot partition layout**: `boot_partition`, `slot0`,
  `slot1`. Most upstream boards already have one; check
  `boards/…/<board>.dts` for `slot1_partition`.
* The board **provisioned** — MCUboot flashed once over SWD or UF2. That is a
  one-time physical act, described in `docs/PROVISIONING.md`. Nothing here works
  until it's done.

## The five things you add

### 1. The module, in `west.yml`

```yaml
manifest:
  projects:
    - name: zephyr
      remote: upstream
      revision: v4.4.2          # exact tag, never a branch
      import:
        name-allowlist:
          - zcbor               # not optional — see below
          - mcuboot
          - mbedtls
          - tf-psa-crypto       # not optional either — see below
          - tinycrypt
          - <your board's HAL>
    - name: runtt
      url: https://github.com/<org>/runtt   # wherever this lives
      revision: main
      path: modules/runtt
```

> **Two allowlist traps, both of which fail confusingly.** Omit **`zcbor`** and
> `CONFIG_MCUMGR` silently *does not exist* — its root Kconfig symbol depends on
> zcbor, so you get no error, just a config that quietly isn't there. Omit
> **`tf-psa-crypto`** and mbedtls fails with `tf-psa-crypto is not an existing
> directory`; since mbedtls 4.x it's a separate repo rather than a subdirectory.
> An allowlist that looks complete is the failure mode in both cases.

### 2. A `VERSION` file next to your `CMakeLists.txt`

```
VERSION_MAJOR = 0
VERSION_MINOR = 3
PATCHLEVEL = 0
VERSION_TWEAK = 0
EXTRAVERSION =
```

This is what `APP_VERSION_STRING` expands to and what `describe` reports. It is
how you tell which release is on a board.

> **Trap:** `imgtool --version` sets the **MCUboot image header** version, which
> is a *different field*. Setting it does not change `APP_VERSION_STRING`, and
> the two disagreeing is confusing rather than harmful. Bump the `VERSION` file.

> **A second imgtool trap, and this one bricks boards.** If you sign an image by
> hand, do **not** pass `--pad-header`. An application built for MCUboot sets
> `CONFIG_ROM_START_OFFSET=0x200` and already reserves the header space;
> `--pad-header` prepends another, so the header says `hdr_size=0x200` while the
> image really starts at 0x400. `imgtool verify` still passes. MCUboot jumps to
> `image + 0x200`, hits the padding, and the core locks up unrecoverably.
> Sanity-check that the word at `hdr_size` is a RAM address, not zero.

### 3. `sysbuild.conf`

```kconfig
SB_CONFIG_BOOTLOADER_MCUBOOT=y
SB_CONFIG_MCUBOOT_MODE_SWAP_USING_OFFSET=y   # pin it; don't inherit the default
SB_CONFIG_BOOT_SIGNATURE_TYPE_RSA=y
```

**Pin the swap mode.** It recently changed to swap-using-offset, and a
bootloader built for one mode cannot boot an image built for the other
([zephyr#98050](https://github.com/zephyrproject-rtos/zephyr/issues/98050)).
Sysbuild builds both together so they always agree locally — the pin is what
stops a Zephyr bump silently changing the on-device contract.

> ### ⚠️ Signing keys — read this before you ship anything
>
> Sysbuild's default is RSA-2048 signed with MCUboot's `root-rsa-2048.pem`. That
> file is the **private key, committed in the public MCUboot repository.**
>
> The consequence is not subtle and it does not look like a failure. `imgtool
> verify` reports *"Image was correctly validated"*. The board boots. Everything
> works. But flashing MCUboot is what **enrols the trust root** — the embedded
> key defines whose firmware the board will ever accept — and with a publicly
> known private key, no trust is enrolled at all. Anyone who can reach the SMP
> transport can push firmware the bootloader will happily verify and boot.
>
> Fine for a bench PoC, deliberately. Before any fleet:
> * generate a per-fleet key pair, keep the private half out of the repo;
> * point `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` at it via the build environment;
> * remember the **public half is baked into MCUboot at provisioning time**, so
>   rotating it means re-flashing over SWD. Key management is a provisioning
>   decision, not a build-time one — decide it before you provision, not after.

### 4. The build flag

```bash
west build -b <board> --sysbuild app/ -- \
      -DZEPHYR_EXTRA_MODULES=<path to runtt> \
      -Dapp_SNIPPET=runtt
```

The snippet appends the contract's Kconfig and the board's devicetree overlay.
Everything in `docs/WIRE_CONTRACT.md` follows from it.

> **`-DZEPHYR_EXTRA_MODULES` is not optional.** West auto-discovers a
> `module.yml` only at a project's root, and `runtt` is nested inside its
> manifest repo rather than being one. Without the flag the module and its
> snippet are simply not there, and the failure is a confusing "unknown snippet"
> rather than anything naming the module. Inside the builder image the path is
> `/ws/runtt`, also exported as `$RUNTT_MODULE`.

> **Use `-Dapp_SNIPPET=`, not `-S`/`--snippet`, whenever you build under
> sysbuild.** A top-level snippet applies to **every** image sysbuild produces,
> including MCUboot — which would pull MCUmgr, this module and a dual CDC-ACM
> composite *into the bootloader*. On a target like RP2040, whose boot slot is
> 63.5 KB, that is also a size problem. `-Dapp_SNIPPET=` scopes it to the
> application image, which is the only place it belongs.
>
> Plain `-S runtt` is correct only for a **non-sysbuild** build — a
> bootloader-less bring-up image, as in `scripts/build-pico.sh bringup`.

### 5. The Dockerfile

The build environment — Zephyr, MCUboot and the `runtt` module — comes from
a **builder image**, built once:

```bash
docker build -f firmware/builder/Dockerfile -t runtt-builder:v4.4.2 firmware/
```

Your application directory then needs nothing but its own source and this:

```dockerfile
ARG BUILDER=runtt-builder:v4.4.2
FROM ${BUILDER} AS builder
ARG BOARD=rpi_pico/rp2040/mcuboot

COPY . /ws/app
RUN west build -b "${BOARD}" --sysbuild /ws/app -d /ws/build -- \
      -DZEPHYR_EXTRA_MODULES=/ws/runtt \
      -Dapp_SNIPPET=runtt

FROM scratch
COPY --from=builder /ws/build/app/zephyr/zephyr.signed.bin /app.signed.bin
ENTRYPOINT ["app.signed.bin"]
```

```bash
cd my-app && docker build -t my-app:v1 .
```

`firmware/examples/app1` and `app2` are complete worked examples.

> **Why a builder image rather than fetching Zephyr in your own Dockerfile.**
> The west manifest and the module are siblings of the application, so a
> self-contained Dockerfile would have to `COPY` them from outside its own
> directory — which forces the build context up to the whole repo and makes
> `cd my-app && docker build .` fail with `"/west.yml": not found`. Putting the
> environment in the builder image is what keeps an application directory
> genuinely self-contained.

> **`COPY . /ws/app` names the sysbuild image.** That destination is why
> `app_SNIPPET` and `/ws/build/app/` both say `app`, whatever your host
> directory is called. Rename the destination and you must rename both.

The second stage is the whole delivery format: `FROM scratch`, one signed
image, an entrypoint naming it. **The runtime resolves `process.args[0]` inside
the rootfs** — that is the entire contract between your image and the runtime.

**There is no `imgtool` step.** Sysbuild signs the image itself, and does it
correctly — which is how this path avoids the `--pad-header` trap in §2
entirely.

Deltas are computed on full image content rather than per layer, so a
single-layer blob is structurally the best case for update size.

## Running it

```bash
docker run --runtime=runtt \
  --annotation dev.runtt.target=usb:3-4 \
  my-firmware:latest
```

Your firmware's log output is that container's stdout.

## What your application code looks like

Unchanged, in the normal case. The template application is deliberately boring:

```c
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

int main(void)
{
	LOG_INF("app %s on %s", APP_VERSION_STRING, CONFIG_BOARD_TARGET);
	while (1) {
		LOG_INF("alive");
		k_sleep(K_SECONDS(2));
	}
}
```

Anything logged on the console channel reaches `docker logs`. That is the
headline feature and it costs you nothing.

### The one optional C API

```c
#include <runtt/health.h>

while (1) {
	do_work();
	runtt_health_feed();     /* CONFIG_RUNTT_HEALTH=y */
}
```

Worth understanding *why* it exists. **SMP echo proves the kernel is alive, not
that your application is.** An app deadlocked in its own logic still answers
echo perfectly, so a host confirming on echo alone can confirm a broken image
— and a confirmed image does not get reverted. Feeding the watchdog extends the
host's confirm gate from "kernel alive" to "application alive".

Optional. Firmware that never calls it is fully manageable, it just gets the
weaker guarantee. If your application has a main loop, use it.

## The Kconfig surface

| Symbol | Default | What it's for |
|---|---|---|
| `RUNTT` | set by the snippet | the whole module |
| `RUNTT_CHANNELS` | `2` | `1` for single-serial targets (ESP32-C3 class) and probe-UART bring-up |
| `RUNTT_USB` | `y` if the DT declares CDC-ACM | registers **every** CDC-ACM instance |
| `RUNTT_USB_VID` / `_PID` | Zephyr's | ship your own; the contract doesn't key off them |
| `RUNTT_IMG_MGMT` | `y` if `slot1_partition` exists | the update half |
| `RUNTT_SMP_DESCRIBE` | `y` | the identity command |
| `RUNTT_CONTRACT_VERSION` | `1.2.0` | what `describe` reports |
| `RUNTT_HEALTH` | `n` | the liveness watchdog above |
| `RUNTT_IDLE` | `n` | **the provisioning placeholder only.** Never set this in customer firmware |

### Why `RUNTT_USB` exists at all

Zephyr's own `CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT` **only registers the
first CDC-ACM instance** — its own source comment says so. A board declaring
both a management and a log channel therefore enumerates with just one, and the
log channel never appears. Observed on an RP2040 before this module existed.
Set `CDC_ACM_SERIAL_INITIALIZE_AT_BOOT=n` and let `RUNTT_USB` do it.

## The two channels

```
&zephyr_udc0 {
	cdc_acm_mgmt: cdc_acm_mgmt {
		compatible = "zephyr,cdc-acm-uart";
		label = "runtt-mgmt";     /* → USB interface string descriptor */
	};
	cdc_acm_log: cdc_acm_log {
		compatible = "zephyr,cdc-acm-uart";
		label = "runtt-log";
	};
};

/ {
	chosen {
		zephyr,uart-mcumgr = &cdc_acm_mgmt;
		zephyr,console     = &cdc_acm_log;
		zephyr,shell-uart  = &cdc_acm_log;
	};
};
```

Each node's devicetree **`label` becomes the USB interface string descriptor**,
and that is what the runtime and the udev rules match on — never VID/PID, never
interface number. You may ship your own VID, and `ID_PATH` is interface-suffixed
so the two channels of one composite device land on different `ID_PATH`s. The
string descriptor is the part of the identity your firmware actually controls.

The snippet provides this overlay for supported boards. For a new board, copy
the pattern into `snippets/runtt/boards/<board>.overlay`.

Each CDC-ACM instance costs 3 endpoints (bulk in, bulk out, interrupt in).
RP2040 has 16 bidirectional and nRF52840 has 7 IN + 7 OUT, so two is
comfortable on both.

Use the **new USB stack** (`CONFIG_USB_DEVICE_STACK_NEXT=y`) — the default since
4.3, the only one that survives 4.5, and the one that reads the devicetree
`label` as the interface string descriptor.

## Skipping `describe`

`describe` is a custom SMP command in the per-user group (64) answering with
contract version, board target, app version, channel count, and whether image
management and the health watchdog are present.

**If you don't implement it, deployment still works.** The runtime probes with a
short timeout (1.5 s), gets nothing, and continues — you lose the identity
readout and version-skew becomes a confusing failure later instead of a clear
one now. With the snippet you get it automatically, so this only matters if you
are writing a firmware side from scratch against the wire contract.

If you *do* implement it by hand, `#include <app_version.h>` behind a
`__has_include` guard — without it `APP_VERSION_STRING` silently reports
`"unknown"` rather than failing to compile.

## Boards without a second slot

`RUNTT_IMG_MGMT` defaults to whether the devicetree has `slot1_partition`.
A plain `rpi_pico` (single code partition, no slots) gets transport and identity
only — it boots standalone with no bootloader, which is exactly what you want
while proving USB enumeration, interface descriptors and udev rules.

It cannot receive an update, and `describe` says so (`img: false`). **Don't ship
it.** For a deliverable image use a slotted board target such as
`rpi_pico/rp2040/mcuboot`, built under sysbuild with MCUboot.

## Zephyr version policy

Pin **v4.4.2** exactly. Minor releases have moved behaviour under this project
twice already: the flash simulator's erase-at-start default changed between 4.3
and 4.4, and MCUboot's swap-mode default became swap-using-offset. Treat a bump
as a deliberate, tested step, with the CI gate as the test.

**About 4.5:** it removes the legacy USB device stack. Your exposure is near
zero by construction — the snippet already uses `USB_DEVICE_STACK_NEXT`. The
only thing still on the legacy stack is **MCUboot itself**, and Zephyr keeps it
there deliberately (`default y if !MCUBOOT`). We don't use MCUboot's USB at all:
it needs it only for `CONFIG_BOOT_SERIAL_CDC_ACM` serial recovery, which we
don't ship. **Keep MCUboot's USB disabled and the 4.5 removal cannot touch
you.** Migration is then: bump the pin, rebuild, run the gate.

## Testing without hardware

```bash
west build -b native_sim/native/64 --snippet runtt app/
```

No sysbuild and no MCUboot here — it cannot chain-load on this target (see
below), so building it would be pointless. This is the one case where the plain
`--snippet` form is the right one.

The two channels become host ptys instead of USB endpoints — same overlay shape,
different transport, which is the point. `scripts/native-sim-e2e.sh` drives the
whole loop headless.

Two honest limits:

* **Flash is erased on every process start** on 4.4 (`flash_simulator.c` sets
  `flash_erase_at_start` when the DT node has no `memory-region`, with no CLI
  escape). Since `os reset` re-execs, flash does not survive a reset. Assert on
  the upload path before the reset, and on reconnection after it.
* **MCUboot cannot chain-load on native_sim** — its POSIX path computes
  `flash_base + offset` and calls it as a function pointer, and "flash" is an
  `mmap`'d data file with no `PROT_EXEC`. Swap, revert and confirm are covered
  by MCUboot's own Rust simulator in `sim/`, which compiles the real `bootutil`
  sources over a NOR-flash model with injectable power failures.

## Checklist

- [ ] `west.yml` pins Zephyr exactly and allowlists `zcbor` **and**
      `tf-psa-crypto`
- [ ] `VERSION` file present and bumped for this release
- [ ] `sysbuild.conf` pins the swap mode
- [ ] **A real signing key, not MCUboot's published one** — before you provision
- [ ] Board target has `slot1_partition`
- [ ] Build passes with `-Dapp_SNIPPET=runtt` (**not** top-level `-S`)
- [ ] Dockerfile's `ENTRYPOINT` names the signed image in the scratch rootfs
- [ ] `describe` reports the version you expect
- [ ] Logs appear in `docker logs`
- [ ] Considered `runtt_health_feed()`

---

*Co-authored with Claude*
