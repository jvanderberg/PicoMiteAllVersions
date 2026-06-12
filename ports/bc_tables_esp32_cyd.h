/*
 * Compiler-table sizes for classic ESP32 CYD without PSRAM.
 *
 * MODE 3 VGA leaves only a small ESP-IDF internal-heap window for FRUN's
 * transient compiler tables. Keep the upfront table footprint below that
 * window while preserving enough room for the bundled small demos.
 */
#ifndef BC_TABLES_ESP32_CYD_H
#define BC_TABLES_ESP32_CYD_H

#define BC_MAX_CODE (10 * 1024)
#define BC_MAX_CONSTANTS 16
#define BC_MAX_SLOTS 64
#define BC_MAX_SUBFUNS 16
#define BC_MAX_FIXUPS 128
#define BC_MAX_LINEMAP 512
#define BC_MAX_LOCALS 32
#define BC_MAX_PARAMS 16
#define BC_MAX_LOCAL_META 112
#define BC_MAX_NEST 8
#define BC_MAX_DATA_ITEMS 64

/* Keep the transient '!FAST converter below the classic ESP32 compile
 * scratch ceiling. The bundled Mandel loop is intentionally small; larger
 * loops will fail with the existing "'!FAST loop too large" diagnostic. */
#define BC_FAST_MAX_ROP_BYTES 1024
#define BC_FAST_MAX_BC_BYTES 1024
#define BC_FAST_OOM_SOFT 1

/* The default RP2040 VM local arena is 256 entries. On CYD that burns
 * runtime heap before user arrays can allocate; 64 is enough for the
 * small no-PSRAM profile and the bundled demos. */
#define BC_VM_MAX_LOCALS 64

#endif /* BC_TABLES_ESP32_CYD_H */
