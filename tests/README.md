# Regression tests

These tests compile the production sources. No board is flashed.
Run them from the source checkout: the Arduino installation ZIP includes
validation documents and build records, but excludes test programs and scripts.

0.6.1 adds the Wi-Fi/network/cloud status matrix, stale IP event ordering and
public status snapshot/reset regressions. Network ordering tests run on the host;
ESP32 event scheduling and FreeRTOS locking still require hardware verification.

On Windows, install Visual Studio C++ Build Tools and run:

```powershell
./tests/run-host-tests.ps1
./tests/run-service-tests.ps1
./tests/run-audit-tests.ps1
./tests/run-audit-tests.ps1 -OptimizationProbes
./tests/run-playback-tests.ps1
./tests/run-ble-policy-tests.ps1
./tests/run-network-optimization-tests.ps1
./tests/run-ota-worker-tests.ps1
./tests/run-arduino-matrix.ps1
```

The host suite uses a transport substitute; the audit suite additionally uses a
deterministic clock and counts heap allocations. It covers time-budget exhaustion,
partial audio continuation, clock wrap, stale stream tokens, metadata, queue
wrap/full/rejection, manifest limits and reconnect policy isolation. The service
suite covers PCM/ADPCM fragmentation, flow retries/cancellation and OTA wire data.

`run-host-tests.sh` and `run-audit-tests.sh` support GCC/Clang hosts (C++11).
Build products stay in Git-ignored directories: `tests/.build`,
`tests/.build-network-optimization`, and `.build/arduino-matrix` (or the chosen
Arduino BuildRoot). The Arduino matrix defaults
to ESP32-S3 with `min_spiffs`; CloudActivation compiles its configured branch with
a placeholder product ID, without connecting or uploading firmware.

See [VALIDATION-0.6.1.md](VALIDATION-0.6.1.md) for current results and hardware limitations.

The original diagnostic probes have been replaced by nine regression groups
for scheduling, cache, playback and manifest fixes:

```powershell
./tests/run-audit-tests.ps1 -OptimizationProbes
```

These regressions check metadata limits, ACK/media independence, scheduling
fairness, exact long-value deduplication, periodic publication, latest-value
coalescing, partial playback and session ordering, bounded manifest generation,
and reserved RX control capacity. They assert the desired behaviour and are not
hardware performance benchmarks. Current resource limits and API behaviour are
documented in the [API reference](../docs/REFERENCE.zh-CN.md).

Playback tests exercise partial sink acceptance and PCM/ADPCM continuation. BLE
policy tests exercise the production packet grouping and queue admission helpers,
not a FreeRTOS scheduler or a radio. Network tests use the production credential,
session and worker-lifetime helpers. OTA tests compile the production worker
against fake ESP-IDF/FreeRTOS operations, including preallocation failures,
cancellation during flash writes, chunk aggregation and persistent task reuse;
they do not program physical flash. Independent test output directories avoid
collisions; host/service/audit share `.build` and must run sequentially.

MSVC sanitizer verification can be reproduced by setting process environment
`CL=/fsanitize=address /Zi` before the audit and optimization commands, then
restoring that variable. Use the same environment setting for playback and OTA
scripts; sanitizer instrumentation is opt-in for those scripts as well.

The production-core regression groups in `host_tests.cpp` check that typed
helpers produce exact little-endian wire bytes; same-width type mismatches and
NaN/Inf preserve the previous cached sample; empty STR values clear it; a cached
sample exceeding the MTU reports INVALID_SIZE and recovers after MTU growth;
offline updates coalesce across reconnect; invalid BOOL/float/vector commands
return InvalidPayload without invoking hardware. The matrix discovers every
example directory, including CachedSensor and SerialPrompt.

The Wi-Fi follow-up adds BasicWiFi. Its configured branch is compiled with dummy
SSID/password constants in a temporary copy; no firmware is uploaded and no
network connection is attempted. This does not change the production library.
