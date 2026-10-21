# Test jig

Board B runs Espressif's `all_device_types_app` Zigbee console as a coordinator. `test_lamp.py` drives both
boards over USB-Serial/JTAG: it forms a fresh network on the jig, factory-resets the lamp so it joins, then
runs the tests. Button presses are injected through the lamp's test console (`btn press`, `btn hold`).

```sh
. ~/esp/esp-idf/export.sh
./jig_setup.sh             # once: builds the console app (SDK pinned to v2.0.4) and flashes board B
python3 test_lamp.py       # ~3 min; add -k <name> to run a subset; full serial log in test_lamp.log
```

Ports default to the two boards' `/dev/serial/by-id/...` paths. Override them with `--lamp` / `--jig`, and
`JIG_PORT=` for `jig_setup.sh`.

Note: opening a USB-Serial/JTAG port resets the chip, so each run starts with both boards rebooting.

Expected result: everything passes, and `test_scenes` is reported as `XFAIL` (expected failure): esp-zigbee-lib 2.0.4
stores no scenes. Hue scenes still work because the bridge sends direct commands. An `XPASS` means a library update
fixed scenes.

### Lamp console

| Command | Output |
|---|---|
| `status` | `STATUS on= level= joined= short= pan= channel= out= txpower=`; `out` is the value last written to the output: 0xRRGGBB for the WS2812, the duty count for PWM |
| `btn press [ms]`, `btn hold <ms>` | Injects a button press through the real debounce/hold logic |
| `temp` | `TEMP <°C>`, the chip's internal sensor, in 1 °C steps (about 48 °C when idle and off) |
| `scan` | Lists nearby Zigbee networks |
| `factory_reset` | Leaves the network and starts pairing |
| `touch [watch <s> \| raw <s> \| scan [s]]` | Touch builds only: `TOUCH raw= baseline= delta= edges= calibrated= touched= interference=` (delays in ns, delta and interference in per mille). `scan` (PWM builds) sweeps the PWM hands-off for the given seconds (default 60, 0 stops) and prints `SCAN worst= ref= <10 bands>` per 2.3 s cycle; see README "Interference indicator". `watch` streams at 10 Hz. `raw` streams every 10 ms frame in the background, as `TR <ms> <frame delta> <20 ms delta> <edges> <touched> <PWM duty>`, while other commands keep working |
| `pwm phase <us>` / `pwm test <per mille>\|off` | Production PWM only: re-lock the PWM counter at a phase after the touch drive edge, or hold a fixed duty (touches are ignored meanwhile). For mapping the PWM's coupling into the touch pad |

## Sniffer (debugging joins and traffic)

`sniffer/` contains a passive IEEE 802.15.4 sniffer firmware for the jig board, and two decoders:

```sh
cd sniffer
idf.py set-target esp32c6 && idf.py -p $JIG_PORT flash            # prints "F <ch> <rssi> <lqi> <hex>" per frame (channel 20 by default)
python3 analyze_join.py CAPTURE --joiner AC:EB:E6:FF:FE:2C:5F:90 --key <hex>      # association + key delivery, tests link keys
python3 decode_session.py CAPTURE --joiner ... --link-key <hex> [--key-capture C --short 0xNNNN]   # decrypt ZDO/ZCL traffic
```

`decode_session.py` recovers the network key from a captured key delivery, in memory only, and never prints it. A
capture file is the sniffer's serial output, one frame per line, optionally prefixed with a timestamp.
Re-run `jig_setup.sh` afterwards to restore the coordinator firmware.
