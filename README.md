# runtt-zephyr-module

**The device half of the runtt wire contract**, as a Zephyr module.

[runtt](https://github.com/shaunmulligan/runtt) is an OCI runtime that deploys firmware to
a microcontroller instead of running a container. This repository is everything
that has to be true *on the MCU* for that to work: the runtime can find the board,
talk to it over MCUmgr SMP, update it, and read its logs.

Add it to your `west.yml`, append one flag to your build, and your existing
Zephyr application becomes deployable as a container image. No source changes.

```bash
west build -b <board> --sysbuild my-app -- -Dapp_SNIPPET=runtt
```

The full setup — the `west.yml` stanza, the two manifest allowlist traps that
fail confusingly, and the signing rules — is in
[docs/FIRMWARE_GUIDE.md](docs/FIRMWARE_GUIDE.md).

## What it provides

Each of these exists because Zephyr does not ship it, or ships it in a form the
contract cannot use.

| Source | What and why |
|---|---|
| `src/usbd.c` | The dual CDC-ACM composite carrying the contract's **interface string descriptors** (`runtt-mgmt`, `runtt-log`). Zephyr's canned initialiser cannot set per-interface strings, and the host's udev rules key on exactly those |
| `src/smp_can.c` | An MCUmgr transport over ISO-TP. Zephyr ships transports for serial, shell, BLE, UDP and LoRaWAN — none for CAN |
| `src/can_log.c` | Console output as raw CAN frames, because a CAN target has no second channel |
| `src/describe.c` | A custom SMP group at 64 answering *which board, which contract, how many channels, am I provisioned* |
| `src/identity.c` | Reads a per-board identity record from flash, so **one firmware image serves a fleet** |
| `src/health.c` | Optional application liveness, extending the host's confirm gate from "kernel alive" to "application alive" |

`snippets/runtt/` is the developer surface: base config plus per-board
`.conf`/`.overlay`. `zephyr/Kconfig` has thirty `RUNTT_*` options underneath
if you need to tune, but the defaults *are* the contract.

## This repository is not buildable on its own

There is no `west.yml` here and no application — it is a Zephyr *module*, not a
manifest repository. To build anything you need a Zephyr workspace and an app;
[`runtt-boards`](https://github.com/shaunmulligan/runtt-boards) supplies both.

## The contract

`CONTRACT_VERSION` states which version of the wire contract this module
implements. The contract itself is documented in
[`runtt`/docs/WIRE_CONTRACT.md](https://github.com/shaunmulligan/runtt/blob/main/docs/WIRE_CONTRACT.md),
which is the authority — a host refuses a device whose **major** disagrees.

```bash
./tests/contract_version.sh
```

That asserts the Kconfig default matches `CONTRACT_VERSION`, and that a board
overlay declares both interface descriptors. Its counterpart on the host side
checks the document, the runtime and the mock; between them nothing goes
unasserted across the two repositories.

## The runtt repositories

| Repo | What it holds | Start here if |
|---|---|---|
| [`runtt`](https://github.com/shaunmulligan/runtt) | the OCI runtime — the **host** side | you want to know what runtt is, or to work on the runtime |
| [`runtt-zephyr-module`](https://github.com/shaunmulligan/runtt-zephyr-module) | the Zephyr module — the **device** side | you have firmware and want it manageable |
| [`runtt-boards`](https://github.com/shaunmulligan/runtt-boards) | provisioning, board bring-up, the west manifest | you have a board that has never run runtt |
| [`runtt-examples`](https://github.com/shaunmulligan/runtt-examples) | two worked applications, and the walkthrough | you want to watch it work end to end |

**New here?** Read [`runtt`](https://github.com/shaunmulligan/runtt)’s README for what this
is and why, then follow the walkthrough in
[`runtt-examples`](https://github.com/shaunmulligan/runtt-examples).

## Licence

Dual licensed under [Apache-2.0](LICENSE-APACHE) or [MIT](LICENSE-MIT), at your
option.
