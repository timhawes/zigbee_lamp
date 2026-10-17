# Zigbee dimmable lamp — Waveshare ESP32-C6-Zero

Firmware for an ESP32-C6 that acts as a Zigbee 3.0 **Dimmable Light** (HA device 0x0101, router role),
intended for use with a Philips Hue Bridge. During development the output is the onboard WS2812 RGB LED
(rendered white) and the input is the BOOT button.

## Behaviour

| Action | Result |
|---|---|
| Short press (< 0.4 s) | Toggle on/off |
| Hold | Dim; release stops. Direction alternates each hold; always up from minimum, down from maximum. Holding while off turns on at minimum and dims up. Full sweep ≈ 3 s. |
| Press within 1 s of boot and hold 2 s | Factory reset: LED blinks red, leaves the network, wipes state and starts pairing |
| Not on a network | LED breathes blue; network steering retries every 10 s |
| Removed by the coordinator (e.g. deleted in the Hue app) | Factory resets itself and starts pairing again |
| Identify / Hue "blink" | LED blinks white |

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

Settings are under `idf.py menuconfig` → *Lamp configuration*: LED/button GPIOs and polarity, LED brightness cap,
hold/sweep timings, boot reset window, Basic cluster strings, steering retry interval, an alternative global
link key (from `sdkconfig.local`), and the test console (`CONFIG_LAMP_TEST_CONSOLE`, **disable for production**).

## Pairing with Hue

1. Make sure no other Zigbee network nearby is open for joining. For example, the test jig shouldn't be running as a coordinator.
2. Factory reset the lamp: press BOOT within 1 s of power-up and hold it for 2 s, or run `factory_reset` on the test console.
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
| `main/light_output.c` | WS2812 driver, gamma, smoothing, status effects (pairing/identify/reset) |
| `main/button.c` | Debounced button → short press / hold / boot-reset events (plus test injection) |
| `main/local_control.c` | Maps button events to toggle and dimming ramps |
| `main/test_console.c` | `btn`, `status`, `temp`, `scan`, `factory_reset` console commands for the test jig |

To move to production hardware, change the GPIOs in menuconfig and replace `light_output.c` with a PWM or
driver backend that keeps the same `light_output_set()` interface.

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
- The WS2812's 8-bit channels give coarse steps at the bottom of the dimming range. A real output should use higher-resolution PWM.
- If the light is off and receives *Move to Level with On/Off* with a transition, the stack turns it on at the stored level and then moves. The output smoothing softens this, but the start may be briefly brighter.
- The boot-time factory-reset gesture can't be tested automatically: the console isn't up within the 1 s window.
