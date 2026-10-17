# CLAUDE.md

Zigbee dimmable light firmware for the Waveshare ESP32-C6-Zero. The output is the onboard WS2812 (GPIO8) and the
input is the BOOT button (GPIO9). The lamp is a router, HA Dimmable Light 0x0101, on endpoint 11. It's paired with the
user's Philips Hue Bridge and works there: on/off, dimming, rooms, scenes, and the app following the button. Start
with `README.md` (behaviour, Hue findings, limitations) and `test/README.md` (jig, console, sniffer). `REPORT.md` is the
dated history; `PLAN.md` is the original plan.

## Hardware on this PC

| Board | Port | Role |
|---|---|---|
| Lamp | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:5F:90-if00` | Device under test, normally paired with Hue |
| Jig | `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:71:54-if00` | Coordinator test jig (`test/jig_setup.sh`) or sniffer (`test/sniffer/`) |

Opening either serial port resets that board. The lamp rejoins Hue by itself after a reset. Leave the jig idle, with no
network formed, whenever the lamp should be on Hue.

## Environment

- ESP-IDF v5.5.5 at `~/esp/esp-idf`: run `. ~/esp/esp-idf/export.sh` first.
- `espressif/esp-zigbee-lib` is pinned to `==2.0.4`.
- The jig app is built in `~/esp/esp-zigbee-sdk/examples/lamp_jig`, at SDK commit `01685ae`. `jig_setup.sh` clones the SDK, builds the app and flashes the jig.
- `sdkconfig.local` (git-ignored, present on this PC) holds the Hue/ZLL link key, `CONFIG_LAMP_ALT_GLOBAL_LINK_KEY`. It's applied only when `sdkconfig` is generated.
- Passwordless sudo is available, and the PC is dedicated to this project.

## Working rules

- **Flash with `idf.py -p <lamp> app-flash`.** It keeps the Zigbee pairing. `erase-flash` or a factory reset unpairs the lamp from Hue.
- **Ask before running `test/test_lamp.py`.** It factory-resets the lamp and joins it to the jig, so the user has to re-pair in the Hue app. Expect "N passed, 1 expected failures, 0 failed".
- **Afterwards, return both boards to a pairable state.** Send `factoryreset` to the jig, and `factory_reset` to the lamp. Room membership comes back on its own a while after re-pairing, so room controls can lag at first.
- **Never print or store the user's Hue network key.** The decoders recover it in memory only. Sniffer captures that contain a key delivery are sensitive: keep them out of the repo.
- **Trivial power savings aren't wanted.** Only propose power or heat work that gives a significant, measured reduction; otherwise prefer performance and reliability. Frequency scaling was tried, saved about 1 °C, and was reverted. The board draws about 0.05 A at 5 V with the LED off.
- End commit messages with a `Co-Authored-By: Claude …` line. The repo's git identity is Tim Hawes <me@timhawes.com>.

## Things that look wrong but aren't

These are workarounds for esp-zigbee-lib 2.0.4, all in `main/zigbee_light.c`. Re-verify each one against Hue and the jig
if the library is upgraded.

- **Descriptor version byte, after endpoint registration:** the library swaps the version and reserved nibbles, so the code stores `version 0 / reserved 1` to put 0x01 on air. Hue hides the light if the version is wrong.
- **`zcl_raw_frame_handler` handles Off-with-effect (OnOff cmd 0x40) itself.** The library crashes on it when GlobalSceneControl is TRUE. The handler also learns which controller to send reports to.
- **`report_later()`:** the code sends unsolicited On/Off and Level reports to the last controller, because Hue never configures reporting.
- **`select_link_key()` / retry timer:** pairing alternates between the Hue/ZLL key with distributed security and the standard Zigbee 3.0 key with centralized security. A LEAVE signal while not joined is a failed join attempt, not a removal.
- **Scenes:** the library stores no scenes, so recall returns NOT_FOUND. `test_scenes` is an expected failure. Hue still works because it sends direct commands after a group recall.
- **ZCL rule:** "Move to Level *with On/Off*" that ends at the minimum level turns the light off. Tests that need "on at level 1" send On, then a plain Move to Level.

## Debugging Hue traffic

Flash `test/sniffer` to the jig. The Hue network is on channel 20, PAN 0x3ba6; the bridge is 0x4e38, endpoint 64. Then
decode with `test/sniffer/decode_session.py --broadcasts`.

Decryption needs a capture that contains the lamp's key delivery from a (re)pairing, passed as `--key-capture`. No such
capture is kept, so record one during the next re-pair. The lamp's `status` command prints its current short address.
Run `jig_setup.sh` afterwards to restore the jig.

## Open items (production)

- Real output stage: PWM with more than 8 bits, keeping the `light_output_set()` interface.
- Final GPIOs, and `CONFIG_LAMP_TEST_CONSOLE=n`.
- Optional Zigbee OTA. This needs two app partitions; currently there is a single factory slot.
- Optional switching regulator, to cut heat.
- The user hasn't confirmed the boot-time factory-reset gesture with the real button. Check before relying on it.
