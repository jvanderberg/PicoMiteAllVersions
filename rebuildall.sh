set -euo pipefail

# Use the local SDK copy at ~/pico/pico-sdk for all builds.
export PICO_SDK_PATH="$HOME/pico/pico-sdk"

if [ ! -d "$PICO_SDK_PATH" ]; then
  echo "PICO_SDK_PATH does not exist: $PICO_SDK_PATH" >&2
  exit 1
fi

rm -rf build
mkdir build
cd build
cmake -DPICO_SDK_PATH="$PICO_SDK_PATH" ..
make
cd ..
