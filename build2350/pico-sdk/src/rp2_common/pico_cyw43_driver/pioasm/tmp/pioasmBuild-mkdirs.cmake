# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/Users/joshv/pico/pico-sdk/tools/pioasm"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pioasm"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pioasm-install"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/tmp"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src"
  "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/Users/joshv/picocalc/PicoMiteAllVersions/build2350/pico-sdk/src/rp2_common/pico_cyw43_driver/pioasm/src/pioasmBuild-stamp${cfgdir}") # cfgdir has leading slash
endif()
