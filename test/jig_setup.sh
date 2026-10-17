#!/usr/bin/env bash
# Build Espressif's Zigbee console example and flash it to the test-jig board.
# The jig acts as a Zigbee coordinator controlled over its serial console by test_lamp.py.
set -euo pipefail

SDK_DIR="${ZIGBEE_SDK_DIR:-$HOME/esp/esp-zigbee-sdk}"
SDK_COMMIT="01685aeadd927a5194725e22cf4c51a8843ddcaa"   # esp-zigbee-sdk v2.0.4 release merge
JIG_DIR="$SDK_DIR/examples/lamp_jig"   # inside the SDK tree: the example uses relative paths
JIG_PORT="${JIG_PORT:-/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:71:54-if00}"

: "${IDF_PATH:?Source ESP-IDF export.sh first}"

if [ ! -d "$SDK_DIR/.git" ]; then
    git clone -q https://github.com/espressif/esp-zigbee-sdk.git "$SDK_DIR"
fi
git -C "$SDK_DIR" fetch -q --depth 1 origin "$SDK_COMMIT"
git -C "$SDK_DIR" checkout -q "$SDK_COMMIT"

# Copy the example to an untracked directory next to it,
# then point it at the SDK's console component and pin the library version.
rm -rf "$JIG_DIR"
cp -r "$SDK_DIR/examples/all_device_types_app" "$JIG_DIR"
cat > "$JIG_DIR/main/idf_component.yml" <<YML
dependencies:
  idf:
    version: ">=5.2"
  esp-zigbee-console:
    path: "../../../components/esp-zigbee-console"
  espressif/esp-zigbee-lib: "==2.0.4"
YML
cat >> "$JIG_DIR/sdkconfig.defaults" <<CFG
CONFIG_IDF_TARGET="esp32c6"
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CFG

cd "$JIG_DIR"
rm -rf build sdkconfig
idf.py set-target esp32c6
idf.py build
idf.py -p "$JIG_PORT" erase-flash flash
