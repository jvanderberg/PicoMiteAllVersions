# ESP32-S3 quad-PSRAM main component.
#
# Shares the full component definition (sources, includes, demos, flags) with
# ports/esp32_s3 via sources.cmake; the bare port-local filenames there resolve
# to esp32_s3/main regardless of which port includes this file. The ONLY
# difference from the octal port is the PSRAM heap slab size: an R2 module is
# 2 MB total, vs the octal default's 8 MB.
#
# The slab override is passed as a variable (not a direct target_* call) so it
# is applied inside sources.cmake, after idf_component_register — target_*
# commands are not available during IDF's requirements-scan pass, which reaches
# any code placed after include() here. 1.25 MB heap leaves room for the
# 0x60000 gap + RAM-slot region within 2 MB; tunable up after measuring free
# SPIRAM on real R2 hardware.
set(HAL_PORT_PSRAM_SLAB_BYTES_OVERRIDE 1310720u)

include(${CMAKE_CURRENT_LIST_DIR}/../../esp32_s3/main/sources.cmake)
