# Zigbee dimmable lamp — Waveshare ESP32-C6-Zero

Firmware for an ESP32-C6 that acts as a Zigbee 3.0 **Dimmable Light** (HA device 0x0101, router role),
intended for use with a Philips Hue Bridge. It's going into a SAD lamp: the production build reads a touch pad
and drives the lamp's LED driver with PWM. The dev build, for the bare board and the test jig, uses the BOOT
button and the onboard WS2812 RGB LED (rendered white). See [Build profiles](#build-profiles).

## Behaviour

| Action | Result |
|---|---|
| Short press or tap (< 0.4 s) | Toggle on/off |
| Hold | Dim; release stops. Direction alternates each hold; always up from minimum, down from maximum. Holding while off turns on at minimum and dims up. Full sweep ≈ 3 s. |
| Press within 5 s of power-up and keep holding for 15 s | Factory reset: the light blinks at 2 Hz, leaves the network, wipes state and starts pairing. Until 15 s the press acts normally (tap toggles, hold dims). |
| Not on a network | Network steering retries every 10 s. The dev build breathes the LED blue; the production build works as a normal lamp with no indication. |
| Removed by the coordinator (e.g. deleted in the Hue app) | Factory resets itself and starts pairing again |
| Identify / Hue "blink" | Light blinks at 1 Hz |

On/off, level, StartUpOnOff and StartUpCurrentLevel are persisted in NVS. The default power-on behaviour is
**on at the previous level**; the coordinator can change it by writing StartUpOnOff (0x0006/0x4003) and
StartUpCurrentLevel (0x0008/0x4000).

## Building

Requires ESP-IDF **v5.5.5** (`~/esp/esp-idf`) and pulls `espressif/esp-zigbee-lib==2.0.4` via the component manager.

The Hue link key isn't committed. Put it in `sdkconfig.local` (git-ignored) before the first build:

```sh
cp sdkconfig.local.example sdkconfig.local   # then fill in CONFIG_LAMP_ALT_GLOBAL_LINK_KEY
```

Without it the build warns and the lamp joins standard Zigbee 3.0 coordinators only. `sdkconfig.local` is applied
when `sdkconfig` is generated; after changing it, delete `sdkconfig` or set the key in menuconfig.

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 erase-flash flash monitor   # erase-flash for a clean Zigbee state
```

Settings are under `idf.py menuconfig` → *Lamp configuration*: input (button or touch) and output (WS2812 or PWM)
with their GPIOs and tuning, pairing indication, hold/sweep timings, boot reset window, Basic cluster strings,
steering retry interval, an alternative global link key (from `sdkconfig.local`), and the test console
(`CONFIG_LAMP_TEST_CONSOLE`, **disable for production**).

## Build profiles

| Profile | Input | Output | Build |
|---|---|---|---|
| Dev (default) | BOOT button, GPIO9 | WS2812, GPIO8 | `idf.py build` |
| Production | Touch pad, GPIO2 → GPIO3 | PWM to the LED driver, GPIO15 | `idf.py -B build-prod -DSDKCONFIG=build-prod/sdkconfig -DLAMP_PROFILE=production build` |

The production profile adds `sdkconfig.defaults.production` and keeps its own `sdkconfig` in `build-prod/`, so it
doesn't disturb the dev configuration. Flash it with the same `-B`/`-DSDKCONFIG`/`-DLAMP_PROFILE` arguments plus
`-p <port> app-flash`. The test console and the interference indicator are off in this profile, and the onboard LED
is turned off at boot. To use `touch scan`, the jig or the indicator, set `CONFIG_LAMP_TEST_CONSOLE=y` and
`CONFIG_LAMP_INTERFERENCE_LED=y` in `build-prod/sdkconfig` and rebuild.

### Production wiring

```
GPIO2 (SEND) ──[ 1–2.2 MΩ ]──┬──[ 1 kΩ ]── GPIO3 (RECV)
                             └── short wire ── copper pad (behind the plastic, no ground pour under it)
GPIO15 (PWM) ──[ 100 Ω ]──┬── LED driver PWM/DIM input (3.3 V logic)
                          └──[ 4.7 kΩ ]── GND, at the driver (keeps it off while the chip resets and boots)
```

- Check the pins against the board pinout. Avoid the strapping pins (GPIO4, 5, 8, 9), USB (GPIO12, 13) and
  UART0 TX (GPIO16, which prints bootloader output). GPIO15 is a strapping pin too, but it only selects the JTAG
  source when an eFuse enables that, and the pull-down holds it low, which is a safe level.
- The PT4115's DIM pin has an internal 200 kΩ pull-up (about 4.5 V open): 47 kΩ or more leaves it at analogue
  dimming during reset, so the pull-down must be about 10 kΩ or less.
- Keep the pad wire short, and route it away from the PWM line and the driver's output wiring. Share ground with the driver.
- `LAMP_PWM_INVERT` handles an active-low driver input (swap the 100 kΩ to 3.3 V in that case).
  `LAMP_PWM_MIN_DUTY_PERMILLE` sets a floor for drivers that don't light below some duty.

### Touch sensing

The ESP32-C6 has no touch peripheral, so the firmware measures charge time. An MCPWM generator drives SEND with a
1 kHz square wave. The resistor and the pad's capacitance slow the edges at RECV, and an MCPWM capture channel times
each edge against the drive edge with 12.5 ns resolution. A finger adds capacitance, so the delay grows. Everything
runs in hardware; the interrupt only adds up the delays.

Every 10 ms the firmware averages the last 20 ms, which is one 50 Hz mains cycle. It compares the result with a
baseline taken in the first 300 ms after power-up, so don't touch the pad during that time. "Touched" means more than
`LAMP_TOUCH_ON_PERMILLE` above the baseline (default 5 %). "Released" means below `LAMP_TOUCH_OFF_PERMILLE` (2.5 %)
for `LAMP_TOUCH_RELEASE_MS` (50 ms), so short dips don't split one touch into two. While untouched, the baseline
follows slow drift. A touch that lasts longer than `LAMP_TOUCH_STUCK_MS` (60 s) is treated as an environment change,
and the baseline is recalibrated. The production profile raises the hold threshold to 500 ms, because taps through
plastic last about 300 ms.

On the lamp, a light touch reads about 6–8 % and a firm one 15–60 %. Untouched noise is under 2 %.

### PWM and touch interference

The LED driver's switching couples strongly into the pad. Measured untouched, it shifted the reading by 15–80 %,
depending on where the PWM edges fall relative to the touch drive edges. The firmware avoids this in three ways:

- **Exact PWM frequency.** The duty resolution is the highest that gives an exact LEDC divider (14-bit at 2 kHz).
  At 15-bit the PWM ran at 2003.2 Hz and beat against the touch drive at 3.2 Hz, which made a firm touch toggle the
  lamp about three times a second.
- **PWM at exactly twice the drive frequency.** Each PWM period then contains one drive edge, at a fixed phase.
- **Edge placement.** At start-up the firmware resets the LEDC counter 30 µs after a touch drive edge. It then sets
  each pulse's start (`hpoint`): the rising edge 60 µs after a drive edge, and for long pulses so the falling edge
  lands 10 µs before the next drive edge. Pulses that would leave a 12–43 µs gap are rounded to the nearer end of that
  band. This only affects levels of about 245–251, and the step is hard to see. Pulses with a shorter gap end 1 µs
  before the counter wraps. The untouched offset is now within about ±1 % at every duty, and switching the light on no
  longer causes a phantom touch. The alignment is checked every 10 s.
- **No pulse crosses the LEDC counter wrap.** The LEDC loads a new duty when the counter wraps. If a shorter duty
  arrives in the middle of a pulse that crosses the wrap, the output stays high for almost a whole period. That showed
  up as a flash when dimming down through one particular level, which changed from boot to boot (about 9 % duty, or 32 %
  in the Hue app). Resetting the counter at a fixed phase is what keeps every pulse inside one counter cycle.

These timings were measured with a 2.2 MΩ resistor, 1 kHz drive and 2 kHz PWM. If the resistor, drive frequency or
wiring changes, re-measure them (see `light_output.c`). `LAMP_PWM_INVERT` swaps which LEDC edge is the electrical rising
edge, so the placement would need re-measuring there too.

**At 20 kHz (the production default, because the driver's inductor sings at 2 kHz)** a PWM period is 50 µs, so
pulses can't avoid the drive edges altogether. But every period has the same phase relative to them, because 20 kHz
is a multiple of twice the drive. A map of untouched offset against duty and phase (`pwm phase`, `pwm test`) showed
that only edges in roughly the first 15 µs after a drive edge matter, while the pad is charging. They shift the
reading by up to 65 ‰; edges from about 20 µs on shift it by under 10 ‰. So the LEDC counter is reset 20 µs after a
drive edge. Pulses that fit rise there, and longer pulses fall there, with the off-gap in the quiet window. The worst
scan offset is 12–16 ‰, about the noise floor. With a random phase it was 30–49 ‰, and the worst band changed from
boot to boot. The resolution is 11-bit, and the level-1 floor is 4 % (the PT4115's minimum duty at 20 kHz). With
touch input and a PWM frequency that isn't a multiple of twice the drive frequency, the firmware warns at boot and
doesn't align.

To tune, run `touch watch 10` on the console and touch the pad through the plastic. Set the ON threshold about halfway
between the untouched noise and the touched delta, and the OFF threshold about half of that. Do this on the lamp's own
supply: USB from a PC grounds the board and gives a much bigger signal than an isolated mains supply. `edges` should
read about 40. Fewer means the RC time constant is too long for the drive frequency, so use a smaller resistor or a
lower `LAMP_TOUCH_DRIVE_HZ`.

### Interference indicator

With `LAMP_INTERFERENCE_LED` (off in the finished production profile; turn it on for wiring work), the onboard WS2812 shows how much the LED
driver disturbs the touch reading. It goes from green (clean), through yellow (half the ON threshold), to red (at the
ON threshold, which causes false touches).

- **Normally** it shows the recent peak of the untouched reading's offset, so dim the lamp up and down (from Hue or the
  pad) and watch it. A finger near the pad shows too.
- **`touch scan [s]`** (console; default 60 s, `touch scan 0` stops) is the hands-off test. Each 2.3 s cycle turns the
  PWM off for 250 ms to take a reference, then ramps it from 0 to 100 % in 2 s. Touches are ignored meanwhile. The
  LED shows the worst offset from the last sweep (blue until the first one finishes), and the console prints
  `SCAN worst=<per mille> ... <worst per 10 % band of duty>`. Move the wiring while it runs: the LED updates every
  cycle. Readings below about 20 leave a comfortable margin under the 50 ON threshold.

Wiring tips for the pad lead:
- The pad node (between the 2.2 MΩ resistor, the 1 kΩ to RECV and the pad) is high impedance and picks up anything
  near it. Keep that run as short as possible and well away from the driver's inductor, its SW pin, and the LED
  wires. These carry the switching edges.
- Twisting the pad lead with a ground wire helps with magnetic pickup, but it doesn't shield against the electric
  field that does most of the coupling. It also adds capacitance, which shrinks the touch signal. If you try it,
  check the touch delta with `touch watch` as well as the scan.
- Bring the board's ground back to the driver's input ground directly, not through the LED wiring. Then the
  driver's switching current doesn't move the board's ground relative to the pad.
- A ground pour or wire right under the pad reduces sensitivity; keep it away from the pad itself.

## Pairing with Hue

1. Make sure no other Zigbee network nearby is open for joining. For example, the test jig shouldn't be running as a coordinator.
2. Factory reset the lamp: press BOOT (or the touch pad) within 5 s of power-up and hold it for 15 s, or run
   `factory_reset` on the test console.
3. In the Hue app, go to *Settings → Lights → Add light → Search*. The lamp joins within one or two pairing attempts, about 15–40 s, and
   appears as a dimmable light.

### What Hue needs, and the library workarounds

These findings come from sniffer captures of a real Hue Bridge (see `test/sniffer/`):

| Issue | Fix in this firmware |
|---|---|
| Hue bulbs deliver the network key encrypted with the **ZLL pre-configured link key**. The network uses **distributed security**: the key-transport source is FF..FF and there's no trust centre. | `CONFIG_LAMP_ALT_GLOBAL_LINK_KEY` (set in `sdkconfig.local`, see *Building*). Pairing attempts alternate between *Hue key + distributed security* and *standard Zigbee 3.0 key + centralized security*, so standard coordinators still work. |
| The bridge ignores lights whose simple descriptor reports application device version 0. | esp-zigbee-lib 2.0.4 sends this byte with its nibbles swapped. The firmware stores the swapped value so `0x01` goes out on air. |
| Hue switches lights off with **Off with effect**. esp-zigbee-lib 2.0.4 crashes on it (null dereference while storing the global scene). | The command is intercepted with a raw ZCL handler: the app switches off and sends the Default Response itself. |
| The bridge never binds or configures reporting on third-party lights, so button changes wouldn't show in the app. | The lamp remembers the last device to send it On/Off or Level commands (the bridge) and sends it unsolicited On/Off and CurrentLevel reports, coalesced to at most one every 300 ms. |
| A failed join makes the stack emit a leave signal. | Only treated as removal (factory reset) when the lamp was actually joined. |

## Code layout

| File | Purpose |
|---|---|
| `main/zigbee_light.c` | Device model, commissioning, ZCL callbacks, persistence, local-change API |
| `main/light_output.c` | Gamma, smoothing, status effects (pairing/identify/reset), WS2812 and PWM (LEDC) backends |
| `main/button.c` | Debounced button or touch → short press / hold / boot-reset events (plus test injection) |
| `main/touch.c` | Charge-time touch sensor (MCPWM drive + capture), baseline tracking, interference meter and scan |
| `main/interference_led.c` | Shows touch interference on the onboard WS2812 in production builds |
| `main/local_control.c` | Maps button events to toggle and dimming ramps |
| `main/test_console.c` | `btn`, `status`, `temp`, `scan`, `factory_reset` and (touch builds) `touch` console commands |

## Testing

See [test/README.md](test/README.md). A second board runs as a Zigbee coordinator, and `test/test_lamp.py`
checks the following end to end: joining, descriptors, Basic attributes, remote on/off/level/transitions, identify,
Off with effect, groups, scenes, unsolicited and configured attribute reporting, button toggle and dimming (via injected
presses), persistence, StartUpOnOff, and removal by the coordinator. `test/sniffer/` has an 802.15.4 sniffer and decoders,
which were used to debug the Hue integration.

## Known limitations

- **Zigbee scenes don't work.** esp-zigbee-lib 2.0.4 accepts Add, Store and Recall Scene but never stores anything: Recall returns
  NOT_FOUND. This is also true in 2.0.5 according to its release notes. In practice Hue scenes still work: the bridge sends
  Recall Scene to a group and then sends the scene's on/off and level directly to the light (seen in sniffer captures).
  `test_scenes` documents the library behaviour and is reported as an expected failure.
- **Rooms after a factory reset:** a factory reset clears the lamp's group memberships. If the lamp is re-paired while the bridge
  still lists it in a room, room controls don't reach it until the bridge restores the membership. The bridge does this by
  itself after a while, without another re-pair.
- The WS2812's 8-bit channels give coarse steps at the bottom of the dimming range. The PWM output is 11-bit at 20 kHz (14-bit at 2 kHz).
- **Touch next to the LED driver:** the firmware works around the coupling (see "PWM and touch interference"), but the
  margin depends on the wiring. Keeping the pad and its wire well away from the driver and LED wiring is the real fix.
- **Audible PWM:** the driver's inductor sings at 2 kHz and swapping it didn't help, so production runs at 20 kHz.
  That works only with the pad lead routed away from the driver (README "Interference indicator") and with the
  20 kHz edge placement above. With the original wiring, 20–21 kHz made the lamp switch itself on and off. The cost
  is a brighter minimum level (4 % duty).
- If the light is off and receives *Move to Level with On/Off* with a transition, the stack turns it on at the stored level and then moves. The output smoothing softens this, but the start may be briefly brighter.
- The factory-reset gesture isn't in the automated tests. The console is up within the 5 s window (about 1.5 s after
  boot), so `btn hold 16000` straight after a reset would exercise it, but it unpairs the lamp.
