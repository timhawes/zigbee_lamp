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

## Update: production I/O for the SAD lamp (2026-10-17)

The firmware now has a production build profile (`-DLAMP_PROFILE=production`) for the SAD lamp: a touch pad behind the
plastic as input, and 3.3 V PWM into the existing LED driver as output. The dev profile (button + WS2812) is unchanged,
so the jig and Hue setup keep working. Wiring, profiles and touch tuning are in the README.

- **Touch:** charge-time sensing with two GPIOs and a 1–2.2 MΩ resistor. MCPWM drives the send pin at 1 kHz, and a
  synced capture channel times each edge at the pad (12.5 ns resolution). Baseline is calibrated in the first 300 ms,
  tracks drift, and recalibrates after a 60 s stuck touch. Checked in loopback on the board (40 edges per 20 ms,
  steady 62 ns); not yet tried with a real pad.
- **PWM:** LEDC, 2 kHz, 15-bit; frequency, polarity and min/max duty are in menuconfig. Duty readback checked
  against the gamma curve.
- **Factory reset:** a press started within 5 s of power-up and held for 15 s, in both profiles. Such a press now acts
  normally until 15 s (tap toggles, hold dims). Checked on the lamp: a tap at 1.6 s toggled, and a 16 s hold
  started at 7 s didn't reset. The reset itself wasn't triggered, to keep the Hue pairing.
- **Indication:** no pairing indication in production; the lamp works normally while unpaired. Identify blinks at 1 Hz,
  and the reset blink is 2 Hz (under 3 flashes per second).
- `status` prints `out=` instead of `rgb=`; `test_lamp.py` was updated, but the suite hasn't been run on this change.

## Update: touch on the real lamp (2026-10-17)

The production build is on the lamp board with the pad and the driver wired up. Two problems turned up, and both were
the LED driver coupling into the pad, not the sensor itself:

1. **Flashing on a firm touch.** At 15-bit resolution the PWM ran at 2003.2 Hz, which beat against the 1 kHz touch
   drive at 3.2 Hz. The reading swung to zero every 312 ms, and each swing counted as a new tap. Fixed by picking the
   resolution that gives an exact divider (14-bit), plus a 50 ms release filter and a 500 ms hold threshold.
2. **Phantom touch after switching on.** With the frequencies locked, the PWM edges land at a fixed phase relative to the
   measurement. Untouched, the reading then shifted by up to 80 %, depending on the phase. A sweep of duty × phase
   (`touch raw` with a temporary console hook) mapped which edge positions are safe. The firmware now finds the phase at
   boot and places each pulse accordingly. Hands-off on/off and dimming tests show no extra toggles, and the untouched
   offset is within ±1 %.
3. **Flash when dimming down through about 9 % duty** (32 % in Hue). With the phase alignment, pulses above a boot-dependent
   duty crossed the LEDC counter wrap. The LEDC loads a new duty at the wrap, so a shorter duty arriving mid-pulse
   held the output high for almost a whole period. The firmware now resets the LEDC counter 30 µs after a drive edge,
   so no pulse crosses the wrap (checked for every duty, with ±3 µs of origin error). The level-1 floor is now 1 % duty,
   because a single 30 ns count didn't light the driver.
4. **Switching off paused at minimum brightness.** The fade's tail sat at the 1 % floor for about half a second. While
   fading to off, the floor now shrinks in proportion to the fade. From level 101, the light goes dark in about 610 ms
   with no plateau.
5. **Ultrasonic PWM tried, reverted (2026-10-18).** The lamp is faintly audible at 2 kHz, so 20 kHz and 21 kHz were
   tried (11-bit, 4 % floor; the PT4115 datasheet gives a 4 % minimum duty at 20 kHz). Edge alignment can't fit in a
   50 µs period, and untouched window deltas rose to 220 ‰ at 20 kHz and ±300 ‰ at 21 kHz, against an ON threshold of
   50 ‰. The lamp toggled and dimmed itself. Back at 2 kHz the same sweep stays within −22…+25 ‰, with no extra
   toggles. Any fix for the noise has to be in hardware (see README "Known limitations").
6. **Interference indicator (2026-10-18).** For trying out the pad wiring, the onboard WS2812 now shows the touch
   interference from green to red (the ON threshold). `touch scan` sweeps the PWM hands-off and reports the worst
   offset per 10 % band of duty. First scans at 2 kHz: worst 35–48 ‰, mostly at 80–100 % duty. The LED's data line
   didn't measurably add noise (an A/B sweep with it disabled was no better).
   After the user rerouted the pad lead twice: the second layout was clean up to 60 % but reached 67–105 ‰ at
   90–100 % (the driver's continuous switching; there are no PWM edges at 100 %). The third layout is flat at a
   worst of 10–13 ‰ across all duties (dimming sweep: −9…+17 ‰, 4/4 toggles). The baseline fell from 16.0 to
   12.0 µs as the lead got shorter.
   With this layout 20 kHz PWM (11-bit, 4 % floor) also works: scan worst 30–37 ‰, dimming sweep peak 45 ‰ at
   65–75 % duty, 4/4 toggles. That's only just under the 50 ‰ ON threshold, so 2 kHz stays the default for now.
7. **20 kHz made the default, with edge placement (2026-10-19).** The inductor swap didn't stop the singing. At
   20 kHz the PWM phase relative to the touch drive was random at each boot, with a worst offset of 30–49 ‰. A map of
   offset against duty and phase showed that only edges in the first ~15 µs after a drive edge matter, at up to
   65 ‰. The LEDC counter now resets 20 µs after a drive edge; short pulses rise there and long pulses fall there.
   Scan worst 12–16 ‰ (the noise floor), dimming sweep peak 23 ‰, 4/4 toggles. 11-bit, 4 % floor. New console
   commands: `pwm phase <us>`, `pwm test <per mille>|off`.
   Touch strength with this wiring (user, 20 kHz): normal taps 89–95 ‰, holds 122–126 ‰, light taps 57–82 ‰,
   untouched peak 19 ‰ over the same 40 s. All 14 touches were detected. The thresholds stay at ON 50 / OFF 25.
   One toggle 2.6 s after a reset, just before that capture, couldn't be reproduced hands-off. There were no touch
   events in 28 boots (10 at level 81 and 6 each at level 1, at 254 and with the light off): the untouched peak was
   13–22 ‰ in every boot. An 8-minute soak had a peak of 18 ‰ and no events.
8. **Flash at power-up; PWM moved to GPIO15 (2026-10-21).** Two causes. The PT4115's DIM pin has an internal 200 kΩ
   pull-up (4.5 V open), so the 100 kΩ/47 kΩ pull-down left it at about 0.9 V (analogue dimming) while the chip
   was in reset; the pull-down is now 4.7 kΩ at the driver. On 24 V power (via the DC-DC), the supply surge shifted
   the ESP32's ground, so a pin driven low still flashed the driver; it stayed off with the pin floating (reset
   held). The firmware now leaves the pin floating for `LAMP_PWM_POWERON_DELAY_MS` (1 s) after a power-on or
   brown-out reset. The user then moved PWM to GPIO15.
   Final-wiring tests: `test_lamp.py` 15 passed, 1 expected failure, 1 failed (`test_remote_leave`: an unrequested
   touch toggle while rejoining); that test then passed 4/4. A second unrequested toggle came 2.4 s after a factory
   reset. Neither reproduced in 21 more joins and 15 factory-reset boots (peaks 26–27 ‰). Scan worst 13–21 ‰,
   sweep peak 25 ‰, 6-minute soak at min and max peak 24 ‰, baseline 12.0 µs. Touch strength: taps 109–121 ‰,
   holds 132–133 ‰, light touches 49–84 ‰ (4 of 5 detected). Thresholds stay at ON 50 / OFF 25.
9. **Power-on default is now off (2026-10-21).** The Hue app has no power-on setting for third-party lights and never
   writes StartUpOnOff. The default now comes from Kconfig (`LAMP_STARTUP_ON_OFF`, Off). Older firmware saved
   StartUp* to NVS on every state change, so the old "on" default was stuck in flash. StartUp* are now saved only
   when a coordinator writes them (new keys), and the legacy keys are erased at boot, so an `app-flash` is enough.
   Checked on the lamp while it was paired with Hue: it came up off at the previous level and rejoined.
   `test_lamp.py`: the first run had 2 failures (`test_persistence`, `test_startup_on_off`) where the jig got no
   response to a read for about 13 s after the lamp's reboot and rejoin; the lamp state was correct. Both passed when
   rerun on their own, and a second full run gave 16 passed, 1 expected failure, 0 failed.

**Still open**
- **Library workarounds:** the descriptor and Off-with-effect fixes depend on esp-zigbee-lib 2.0.4 internals. Re-check
  them if you upgrade the library.
- **Production:** tune touch thresholds and the PWM duty floor on the real lamp, confirm the final GPIOs, then turn the test console off. Optional OTA and a switching regulator.

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

On/off, level, StartUpOnOff and StartUpCurrentLevel survive reboots. By default the light comes back on at its previous level. (Superseded by entry 9: the default is now off.)

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
