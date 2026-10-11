# Bundled NimBLE-Arduino host

Deotaland includes the NimBLE host and ESP32 ports from
[NimBLE-Arduino 2.5.1](https://github.com/h2zero/NimBLE-Arduino/tree/2.5.1).
Users install only the Deotaland Arduino library ZIP and a supported official
Arduino-ESP32 Core. No separate NimBLE-Arduino installation, SDK replacement,
or sketch-level compiler flags are required.

## Provenance

- Upstream tag: `2.5.1`
- Upstream commit: `5ae2c43f62c5a588a25c1de68f7d17b6f3b75adb`
- Download: `https://codeload.github.com/h2zero/NimBLE-Arduino/zip/refs/tags/2.5.1`
- Archive SHA-256: `612e7b814bbf3a69f2e43fd1a91430b23e4e8f3b9e820a5bb82330e517b5dc14`
- Licenses and notices: [LICENSE](LICENSE), [NOTICE](NOTICE),
  [Apache NimBLE LICENSE](nimble/LICENSE), and [Apache NimBLE NOTICE](nimble/NOTICE).
  File-specific copyright and license notices remain in their original files.

The retained upstream files are `src/nimble/**`, `src/syscfg/**`,
`src/nimconfig.h`, and `src/nimconfig_rename.h`, plus the upstream license
and notice. This includes the raw host, GAP/GATT services, bond storage,
FreeRTOS/ESP32 HCI ports, and their support headers. The high-level `NimBLE*`
C++ wrappers, HID wrappers, examples, and upstream documentation are not
included. Deotaland owns the host lifecycle and GATT definitions through the
raw C API; it does not create a second `NimBLEDevice` singleton.

The three ancillary Markdown documents beneath `src/nimble` are omitted;
their source remains available upstream:
[README](https://github.com/h2zero/NimBLE-Arduino/blob/5ae2c43f62c5a588a25c1de68f7d17b6f3b75adb/src/nimble/README.md),
[coding standards](https://github.com/h2zero/NimBLE-Arduino/blob/5ae2c43f62c5a588a25c1de68f7d17b6f3b75adb/src/nimble/CODING_STANDARDS.md), and
[release notes](https://github.com/h2zero/NimBLE-Arduino/blob/5ae2c43f62c5a588a25c1de68f7d17b6f3b75adb/src/nimble/RELEASE_NOTES.md).
These documents refer to files outside the vendored subset. All source,
copyright headers, license files, and notices remain included.

## Local changes

1. Includes rooted at upstream `src` now use the private
   `vendor/nimble-arduino/` prefix. This prevents SDK header selection from
   silently mixing two different NimBLE host layouts. ESP HCI internal and
   driver headers are also selected explicitly from this private tree, so
   newer-controller translation units do not import the Core's mbuf/NPL
   definitions through an unqualified header.
2. Every bundled C source is guarded by `DeotalandNimblePlatform.h` and includes
   the same private `nimconfig.h` before upstream code. On native builds,
   STM32, chips without BLE, `AGENT_LINK_DISABLE_BLE`, and the explicit
   `DEOTALAND_USE_IDF_NIMBLE` fallback, these files are empty translation units.
3. `nimconfig.h` includes `DeotalandNimbleConfig.h` after SDK configuration and
   upstream compatibility aliases. It consistently selects one connection,
   peripheral/broadcaster roles, two CoC channels, a 512-byte CoC MPS, and a
   6144-byte host task stack. Central/observer, extended advertising, periodic
   advertising, and EATT are disabled for this backend.
4. The internal MSYS pools contain 24 blocks of 256 bytes and 32 blocks of
   576 bytes. The larger block leaves room for a 512-byte SDU and mbuf/framing
   metadata. The pool data alone reserves 24 KiB; host structures, controller
   buffers, tasks, and application queues add further RAM use.
5. `DeotalandNimble.h` exposes the selected raw C headers to the transport.
   The transport initializes the official Core's controller and the bundled
   host once. Upstream Arduino's port intentionally delegates controller
   initialization to its caller.
6. The legacy ESP32/S3/C3 VHCI lifecycle prepares its pools, semaphore, and
   transport before registering controller callbacks. Failed initialization
   rolls back only initialized resources; teardown detaches callbacks before
   releasing resources. This fixes the upstream semaphore allocation failure
   path, which previously left callbacks registered against freed buffers.
   These lifecycle changes keep the public NimBLE API unchanged. Teardown follows the
   upstream stopped-host contract and does not wait for a TX credit after
   detaching the callback that would replenish it.

The official Core's Bluetooth controller remains precompiled. These settings
configure the bundled host sources only; no installed Core files are edited.
Deotaland is the BLE host owner. Applications must not also initialize Core
`BLEDevice` or a separately installed `NimBLEDevice` in the same firmware.

Custom host builds may define `DEOTALAND_USE_IDF_NIMBLE=1` consistently for all
library translation units to select their own ESP-IDF NimBLE host instead of
the bundled host. Such builds must provide the matching SDK and enable CoC in
that host; the define cannot add CoC to a precompiled SDK. The ordinary Arduino
library path uses the bundled host by default.
