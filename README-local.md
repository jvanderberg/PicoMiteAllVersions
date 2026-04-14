# Local build notes

## VM prototype build and test loop
- Host oracle/VM harness:
  - `make -C host`
  - `./host/run_tests.sh`
  - `./host/run_pixel_tests.sh`
  - `./host/run_host_shim_tests.sh`
  - `./host/run_frontend_tests.sh`
  - `./host/run_optimizer_tests.sh`
  - `bash host/run_unsupported_tests.sh`
  - `./host/run_missing_syscall_tests.sh` (intentionally red syscall TODO inventory)
- RP2350 firmware build:
  - `make -C build2350 -j8`
  - `arm-none-eabi-size build2350/PicoMite.elf`
- Current host binary: `host/mmbasic_test`.
- Current device program execution target: VM-only `RUN`.
- `FRUN` and interpreter bridge fallback are removed from the user-facing VM path.
- If the host build links against stale command-table objects after header changes, use `make -B -C host`.

## rp2040 RAM overflow fix
- Symptom: linking failed with `.heap` not fitting in `RAM` (`region 'RAM' overflowed by 20 bytes`) when building rp2040 variants.
- Cause: rp2040 builds used `-DPICO_HEAP_SIZE=0x1000`, and the final layout exceeded the 256 KB RAM window by 20 bytes.
- Fix: `CMakeLists.txt` now sets a helper variable `PICOMITE_HEAP_SIZE` to `0x0fe0` for rp2040 builds (keeps `0x1000` for others) and passes it via `-DPICO_HEAP_SIZE=${PICOMITE_HEAP_SIZE}`. This trims 0x20 bytes from the heap and brings the image under the RAM limit.

## Reconfigure after pulling this change
- From the repo root, recreate or re-run CMake for each build dir (example):
  - `rm -rf build && mkdir build && cd build && cmake .. && make`
- Or run the helper: `./rebuildall.sh` (uses `PICO_SDK_PATH=$HOME/pico/pico-sdk`).
