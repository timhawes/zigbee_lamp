# Report: Zigbee dimmable lamp firmware (ESP32-C6-Zero)

*2026-10-16*

## Summary

The firmware is built and flashed to board 1, and board 2 runs as a Zigbee coordinator test jig.

| Board | Serial (by-id) | Role |
|---|---|---|
| 1 | `…AC:EB:E6:2C:5F:90` (ttyACM0) | Lamp |
| 2 | `…AC:EB:E6:2C:71:54` (ttyACM1) | Test jig (coordinator) |

The automated end-to-end suite passes **12/12** in about 75 s. Three things still need you:

- how the LED and the real button look and feel;
- the boot-time factory-reset gesture;
- pairing with your Hue Bridge.

## Update: Hue integration and fixes (later on 2026-10-16)

The lamp now pairs with the Hue Bridge. It appears in the app, and on/off and dimming work from the app. Button changes show
in the app within about 0.5 s. All of this was tested on your bridge. The regression suite on the jig passes **15/16**; the one
failure is `test_scenes`, a library limitation described below.

| Problem you saw | Root cause (found with a sniffer on the jig board) | Fix |
|---|---|---|
| Turning off paused at minimum brightness before going dark | The smoothing ran on linear brightness, and the "minimum 1" clamp also applied while fading to off | Smoothing now runs in the level domain; the clamp only applies while on |
| White blinks and resets while pairing | A failed join attempt makes the stack signal a leave, which the firmware treated as removal and factory-reset | Factory reset only on a leave while actually joined |
| Couldn't pair with Hue | Hue bulbs send the network key encrypted with the ZLL pre-configured link key, and the Hue network uses distributed security | Hue key plus distributed joining, alternating with standard Zigbee 3.0 settings on each attempt |
| Joined, but not shown in the app | esp-zigbee-lib 2.0.4 swaps nibbles in the simple descriptor: device version 1 is sent as 0, which Hue rejects | Store the swapped value so 0x01 goes out on air |
| "Off" from the app came back at full brightness | The library crashes on Hue's *Off with effect* (global-scene store path), and the lamp rebooted | The app handles Off with effect itself |
| The app didn't follow the button | The bridge never configures reporting on third-party lights | Unsolicited On/Off and Level reports to the last controller, coalesced to at most one every 300 ms |
| At minimum brightness the light wouldn't toggle (2026-10-17) | The renderer skipped LED writes when a rounded change key was unchanged; at levels up to about 8 "on" and "off" rounded to the same key | Compare the actual bytes sent to the LED. `status` now prints them (`rgb=`), and `test_button_toggle_at_min` checks them |

New tests: Off with effect (unicast and group), group commands, scenes, and unsolicited reports. New tools in `test/sniffer/`:
an 802.15.4 sniffer firmware, `analyze_join.py` (join and key-delivery analysis; tests candidate link keys) and
`decode_session.py` (decrypts a device's traffic; the network key is never printed).

**Power and heat (2026-10-17):** the chip sits at about 48 °C when the lamp is off. CPU frequency scaling (160 to 40 MHz when idle)
lowered that by only about 1 °C, so it was reverted in favour of performance and reliability. The heat is the radio, which a
router keeps listening all the time, plus the board's linear regulator. TX power is 20 dBm. The console has a `temp` command.

**Status at 2026-10-17:** paired with the Hue Bridge. On/off, dimming, room controls and scenes all work, and the app follows
the button. The regression suite passes, with `test_scenes` reported as an expected failure. Hue scenes work despite the
library: the bridge recalls the scene through a group, then sends on/off and level directly to the lamp. The Hue key moved
to the git-ignored `sdkconfig.local`. The project is in git. `CLAUDE.md` holds the hand-over notes and the production
to-do list.

**Still open**
- **Library workarounds:** the descriptor and Off-with-effect fixes depend on esp-zigbee-lib 2.0.4 internals. Re-check
  them if you upgrade the library.
- **Production:** PWM output stage, final GPIOs, test console off, optional OTA and a switching regulator.

The sections below are the original report. Where they conflict, this update supersedes them (the test count, the Hue
key limitation, and the factory-reset paths).

## Behaviour

The lamp is a Zigbee 3.0 Dimmable Light (HA device 0x0101) acting as a router.

| Input / event | Result |
|---|---|
| Short press (< 0.4 s) | Toggle on/off |
| Hold | Dim (full sweep about 3 s). The direction alternates on each hold, always going up from the minimum and down from the maximum. Holding while off turns the light on at the minimum and dims up. |
| Press within 1 s of power-up, hold 2 s | Factory reset: the LED blinks red, the lamp leaves the network and starts pairing again |
| Not on a network | The LED breathes blue; the lamp retries joining every 10 s |
| Removed by the coordinator (e.g. deleted in the Hue app) | The lamp factory-resets itself and starts pairing again |
| Identify (Hue's "blink") | The LED blinks white |

On/off, level, StartUpOnOff and StartUpCurrentLevel survive reboots. By default the light comes back on at its previous level.

These Hue-specific details come from community reports:

- application device version 1;
- the GlobalSceneControl, OnTime, OffWaitTime and StartUp* attributes, which Hue needs for "off with effect" and power-on behaviour;
- mains power source;
- re-pairing after removal.

## Verification

`test/test_lamp.py` forms a fresh network on the jig, factory-resets the lamp so it joins, then runs the tests. Button presses are injected through a test console on the lamp, so they go through the real debounce and hold logic.

| Test | Result |
|---|---|
| Join and descriptor (profile HA, device 0x0101, version 1) | PASS |
| Basic attributes (manufacturer, model, power source = mains) | PASS |
| Remote On / Off / Toggle | PASS |
| Remote Move to Level, immediate and with a 1 s transition | PASS |
| Identify | PASS |
| Binding and reporting setup | PASS |
| Button toggle, with an On/Off report reaching the coordinator | PASS |
| Button dimming: direction alternates, stops at max, Level reports sent | PASS |
| Hold while off: turns on at min and dims up | PASS |
| Persistence across reboot | PASS |
| StartUpOnOff = off is honoured and persisted | PASS |
| Removal by coordinator: resets, re-pairs, back to defaults | PASS |

A production build with the test console disabled also compiles cleanly.

### Bug found and fixed during testing

With a fade on Off (OnOffTransitionTime > 0), the stack leaves CurrentLevel at minimum while the light is off and only restores it at the next command. A power cut in that state would have saved level 1, so the light would have come back at minimum.

The fix sets OnOffTransitionTime to 0. The LED's own 150 ms smoothing still gives a soft on/off fade.

## Known limitations

- **Coarse dim end:** the onboard WS2812 only has 8-bit brightness, so the bottom of the dimming range has visible steps. A production output should use higher-resolution PWM.
- **Brighter start from off:** if the light is off and receives "Move to Level with On/Off" with a transition, the stack turns it on at the stored level and then moves. The output smoothing softens this, but the start may be briefly brighter.
- **Hue joining:** the light may not be accepted by the Hue Bridge using the standard Zigbee 3.0 key. If the lamp keeps breathing blue and logs `Steering unsuccessful`, set `CONFIG_LAMP_ALT_GLOBAL_LINK_KEY` to the key your bridge requires; you'd need to supply it. Optionally also enable `CONFIG_LAMP_JOIN_DISTRIBUTED`.
- **Boot gesture not automated:** the test console isn't up within the 1 s window, so the factory-reset gesture can't be tested automatically.

## Next steps for you

1. Power off or factory-reset the jig board (board 2). Otherwise the lamp may rejoin the jig's network instead of the bridge. The lamp is currently paired to the jig.
2. On board 1, check the button by hand: toggle, the feel of hold-to-dim, and the reset gesture (press within 1 s of power-up, hold 2 s).
3. In the Hue app, go to *Settings → Lights → Add light → Search*. Check that the lamp appears, dims and toggles, and that changes made with the button show up in the app.
4. For production, set the GPIOs in menuconfig, turn off `CONFIG_LAMP_TEST_CONSOLE`, and replace `light_output.c` with a PWM backend that keeps the `light_output_set()` interface.

## Project files

All paths are under `/home/claude/Projects/lamp`.

| File | Purpose |
|---|---|
| `main/zigbee_light.c` | Device model, commissioning, ZCL callbacks, NVS persistence |
| `main/light_output.c` | WS2812 driver, gamma, smoothing, status effects |
| `main/button.c` | Debounced button events, boot gesture, test injection |
| `main/local_control.c` | Toggle and dimming ramp logic |
| `main/test_console.c` | `btn`, `status`, `factory_reset` commands |
| `main/Kconfig.projbuild` | GPIOs, timings, brightness cap, Basic strings, link key, test console |
| `test/jig_setup.sh` | Builds and flashes the coordinator jig |
| `test/test_lamp.py`, `test/board.py` | End-to-end tests |
| `README.md`, `test/README.md`, `PLAN.md` | Documentation and the original plan |

Versions are pinned: ESP-IDF v5.5.5 and `espressif/esp-zigbee-lib` 2.0.4. The jig uses esp-zigbee-sdk commit `01685ae`, the v2.0.4 release.

## Changes made to this PC

- Installed build packages with apt: git, cmake, ninja-build, python3-venv and others.
- Added the udev rule `/etc/udev/rules.d/60-espressif.rules`, which gives the `plugdev` group access to Espressif USB devices.
- Added user `claude` to `dialout`.
- Installed ESP-IDF v5.5.5 in `~/esp/esp-idf`, with its toolchain in `~/.espressif`.
- Cloned esp-zigbee-sdk to `~/esp/esp-zigbee-sdk`. The jig is built in the untracked directory `examples/lamp_jig` inside it.

The project is not a git repository yet; I can initialise it and make a first commit if you like.
