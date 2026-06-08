# ESP32-S3 quad-PSRAM port — plan

## Goal

Ship **two** ESP32-S3 images that between them cover every S3 board:

- **octal** — R8 modules (8 MB OPI PSRAM). The current `ports/esp32_s3`.
- **quad** — R2 modules (2 MB QPI PSRAM) **and** no-PSRAM boards. A new
  `ports/esp32_s3_quad`.

WiFi must work in all three cases (octal / quad / none). The release publishes
only the flashable bins. Each port is built the standard, documented way — no
per-variant build scripts.

## Why two images (not one)

The PSRAM **line mode** (octal OPI vs quad QPI) is configured by the bootloader
and the compiled `esp_psram` driver — it is **not** runtime-switchable, and
ESP-IDF builds the driver for exactly one mode. Sending OPI commands to a QPI
chip reads back nothing (`PSRAM ID read error: 0x00000000 ... wrong PSRAM line
mode`). So octal and quad are a genuine compile-time fork; MicroPython and
Arduino-ESP32 ship per-variant firmware for the same reason. The common
Espressif modules are R8 (8 MB, octal) and R2 (2 MB, quad); the reliable way to
know which image a board needs is empirical — flash one and check the boot log /
`MM.INFO(PSRAM SIZE)`. If PSRAM fails to initialize, the line mode is wrong, so
use the other image.

Field reports that motivated this (three boards, three different root causes):

| Board | Symptom | Root cause | Resolution |
|---|---|---|---|
| N16R8 (Boya flash) | boot loop (`TG0WDT` / `ets_loader.c`) | flashed **QIO** | flash **DIO** (doc fix, no code) |
| WaveShare S3-PICO (2 MB) | boots, no PSRAM, no WiFi | **quad** chip vs **octal** build | this plan — quad image |
| CP2102 bridge boards | flashes, silent console | native-USB console vs UART-only board | dual-console (PR #33) |

## Memory model

The `MEMORY` "RAM" figure is the variable/general heap. On this port it is the
**internal MMHeap plus the PSRAM slab**:

- `HAL_PORT_HEAP_MEMORY_SIZE` (`port_config.h`) = **48 KB** — the internal
  MMHeap, always present, independent of PSRAM.
- `HAL_PORT_PSRAM_SLAB_BYTES` — the PSRAM heap extension. Configured per port:
  6 MB on octal, 1.25 MB on quad.
- `GetSystemMemory` / `GetMemory` allocate from the internal MMHeap first and
  fall back to PSRAM (`Memory.c:975`), erroring only when both are exhausted —
  so PSRAM is an *extension*, not a hard requirement, and a board with no
  usable PSRAM still has the 48 KB internal heap.

The only measured data point so far: an octal FREENOVE N16R8 reports `6220K` of
"RAM" in `MEMORY` (the 6 MB slab plus the internal heap and variable table). The
quad and no-PSRAM cases are **not yet measured** — the per-port slab sizes above
are configured targets, and the delivered "RAM" on real R2 / no-PSRAM hardware
is to be confirmed when such a board is tested. The program store and saved-vars
region are separate buffers, unaffected by PSRAM.

## The octal/quad delta (it's tiny)

After reading the tree, only two things differ:

1. **`sdkconfig`**: `CONFIG_SPIRAM_MODE_OCT` → `CONFIG_SPIRAM_MODE_QUAD`, PSRAM
   speed, and the WiFi-buffer settings.
2. **`HAL_PORT_PSRAM_SLAB_BYTES`** (`port_config.h:142`): `6 MB` → ~`1.3 MB`
   (2 MB total − the `0x60000` gap − the ~240 KB slot region).

No source, driver, or GPIO differences (PSRAM is on dedicated MSPI pins). The
quad port must therefore be a **thin directory that shares 100% of the code**.

## 1. Quad port directory — `ports/esp32_s3_quad/`

- `CMakeLists.txt` — `project(mmbasic_anywhere_esp32_s3_quad)`.
- `main/CMakeLists.txt` — `include()`s a new extracted
  `ports/esp32_s3/main/sources.cmake` (the SRCS list + `idf_component_register`,
  lifted verbatim from today's `main/CMakeLists.txt` so there is **one**
  source-of-truth file list both ports share).
- `sdkconfig.defaults` — shared base + quad PSRAM fragment (§2).
- **No forked `port_config.h`.** Make the slab size overridable:
  ```c
  #ifndef HAL_PORT_PSRAM_SLAB_BYTES
  #define HAL_PORT_PSRAM_SLAB_BYTES (6u * 1024 * 1024)   /* octal default */
  #endif
  ```
  and the quad project's own `CMakeLists.txt` passes
  `-DHAL_PORT_PSRAM_SLAB_BYTES=...`. Zero header fork.
- **partitions.csv shared** — both are 4 MB-image layouts; the quad project
  points `CONFIG_PARTITION_TABLE_FILENAME` at the existing
  `../esp32_s3/partitions.csv`.

Net: the quad directory is ~3 files and cannot diverge from the octal sources.

## 2. Shared config refactor (so the two never drift)

Split today's monolithic `sdkconfig.defaults` into:

- `sdkconfig.common` — console, watchdog-off, partition table, cores, CPU freq,
  and **WiFi (internal-RAM, §3)** — everything PSRAM-independent.
- `sdkconfig.psram-octal` / `sdkconfig.psram-quad` — only `SPIRAM_MODE_*`,
  speed, and WiFi-SPIRAM toggles.

Each port sets `SDKCONFIG_DEFAULTS = common + its PSRAM fragment`.

## 3. WiFi in both modes — decouple from PSRAM (lands in `sdkconfig.common`)

```
# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set
# CONFIG_ESP_WIFI_STATIC_TX_BUFFER is not set
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y
CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM=0
```

WiFi buffers come from internal RAM → works on octal, quad, and no-PSRAM. The
octal build loses nothing meaningful (MMBasic wants PSRAM for *heap*, not WiFi
buffering) and gains robustness: WiFi now survives any PSRAM-init failure
instead of cascading to `WiFi init failed`.

## 4. Building a port — standard, documented, no variant scripts

Each `ports/esp32_s3*/` directory is a self-contained ESP-IDF project (its own
`sdkconfig.defaults` and `CMakeLists.txt`) and builds the **one standard way**:

```sh
. ~/esp/esp-idf/export.sh
cd ports/esp32_s3_quad        # or ports/esp32_s3
idf.py set-target esp32s3
idf.py build
```

`buildesp32.sh` is generalized to take the **port directory** as its argument —
`./buildesp32.sh ports/esp32_s3_quad` — so the single wrapper (HAL purity gate +
`idf.py build`) works for *any* ESP32 port, keyed on the directory, not on a
"variant" flag. The quad port's only build-time specialization (the slab `-D`)
lives in its own `CMakeLists.txt`, so the plain `idf.py build` produces the right
image with no external flags. This convention is documented in the port README.

## 5. Release workflow (`esp32-s3.yml`) — publish only bins

- Build **both** port directories in the job.
- **Drop `flash.txt`** (instructions move to the README) and stop uploading the
  loose split bins.
- Publish exactly two files to the `latest` release:
  `MMBasic-Anywhere-esp32-s3-octal-merged.bin` and `…-quad-merged.bin`
  (`merge_bin` keeps `--flash_mode dio`).

## 6. README

- **Two downloads**, with a one-line chooser: *8 MB PSRAM → octal; 2 MB or
  unsure → quad (it also runs PSRAM-less boards at the ~48 KB floor).*
- **Flashing** (moved from `flash.txt`): `erase_flash` then
  `write_flash 0x0 …-merged.bin`, with the **DIO** note front-and-centre (the
  N16R8 boot-loop fix). Cross-platform port-node note (`/dev/cu.usbmodem*` /
  `COMx`) already covered.
- Note `MM.INFO(PSRAM SIZE)` returns 0 when PSRAM is disabled — the simple
  "did I grab the wrong image?" check (uses the existing function; no new
  diagnostics added).

## 7. Quad performance

Quad PSRAM uses 4 data lines vs octal's 8, so at the same clock it has roughly
half the peak bus bandwidth — a property of the bus width, not a benchmark. The
practical impact on MMBasic has **not** been measured; by reasoning it should
matter most for bulk PSRAM transfers (framebuffers, large array/copy ops, file
buffering) and least for interpreter-bound work. Where a framebuffer fits in
internal RAM, keeping it there sidesteps the PSRAM path. Benchmark on real R2
hardware before stating anything firmer.

## 8. Testing / rollout

1. Build both; confirm octal is behaviour-identical to today on the FREENOVE
   (no regression).
2. Flash the **quad** merged bin to the WaveShare tester → expect
   `Found 2MB PSRAM`, PSRAM-backed `RAM` in `MEMORY`, and working WiFi.
   (This is where the quad heap figure gets measured for the first time.)
3. Confirm the octal image on the WaveShare still boots to a GENERIC prompt
   (fail-soft) with `MM.INFO(PSRAM SIZE) = 0`.

## Open decisions

1. **Default download** — octal or quad as the headline? (Lean octal; flip if
   reports skew quad.)
2. **Fold dual-console (PR #33) into both variants now**, or land it separately
   and rebase after?
