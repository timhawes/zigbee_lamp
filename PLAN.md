# Plan: Zigbee dimmable light firmware for Waveshare ESP32-C6-Zero

## Context
New, empty project (`/home/claude/Projects/lamp`). Build firmware that makes an ESP32-C6-Zero a Zigbee dimmable light that will ultimately join a **Philips Hue Bridge**. For now the output is the onboard WS2812 RGB LED (GPIO8, rendered as white) and the input is the BOOT button (GPIO9, active-low). Two boards are attached:
- **Board A (light, DUT):** `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:5F:90-if00` (ttyACM0)
- **Board B (test jig):** `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:71:54-if00` (ttyACM1)

Button behaviour:
- **Short press** (<400 ms): toggle on/off.
- **Hold:** ramp the level up or down; release stops. The direction alternates on each hold, but always goes up from the minimum and down from the maximum. Holding while the light is off turns it on at the minimum level and ramps up. A full sweep takes about 3 s.
- **Factory reset:** press within the first 1 s after boot and keep holding for 2 s. The LED blinks fast, the device leaves the network, wipes its Zigbee data, and starts pairing again. This works because GPIO9 is only sampled as a strapping pin at reset, so a press after release doesn't enter download mode.
- The input and output GPIOs go in Kconfig so production can use a different input pin.

## Toolchain setup (passwordless sudo is OK)
1. `sudo apt install git wget flex bison gperf python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0`
2. `sudo usermod -aG dialout claude`. Because group membership needs a new login, run serial commands via `sg dialout -c ...` in this session.
3. Clone ESP-IDF **v5.5.5** to `~/esp/esp-idf` and run `install.sh esp32c6`. v5.5.6 came out today, so it's skipped per the two-week rule; the Zigbee SDK recommends v5.5.x. If Python 3.14 breaks the IDF venv, install Python 3.12 via `uv` and point `IDF_PYTHON` at it.
4. Pin the component `espressif/esp-zigbee-lib` to **==2.0.4** (2.0.5 also came out today). This uses the new v2 `ezb_*` API.

## Project layout
```
lamp/
  CMakeLists.txt, sdkconfig.defaults (Zigbee on, 4MB flash, custom partitions, USB-Serial-JTAG console)
  partitions.csv            # from SDK example: nvs, otadata, phy_init, factory, zb_fct
  main/
    idf_component.yml       # esp-zigbee-lib ==2.0.4, espressif/led_strip ~3.0
    Kconfig.projbuild       # LED gpio(8), button gpio(9)+active level, dim timing, optional Hue/ZLL link key, test console enable
    main.c                  # nvs init, start light_output, button, zigbee task
    zigbee_light.c/.h       # device model, commissioning, signals, attribute callbacks, local-change API
    light_output.c/.h       # WS2812 via led_strip/RMT, 100 Hz render task, gamma curve, smoothing, effects (identify/pairing/reset blink)
    button.c/.h             # 10 ms poll + debounce → events: SHORT_PRESS, HOLD_START, HOLD_END, BOOT_RESET
    local_control.c/.h      # maps button events → toggle / ramp logic (direction memory, limits)
    test_console.c          # (Kconfig, default on for dev) esp_console cmds: `btn press`, `btn hold <ms>`, `status`, `factory_reset`
  test/
    README.md               # how to build/flash jig + run tests
    jig_setup.sh            # clone esp-zigbee-sdk (pinned tag), build all_device_types_app, flash Board B
    test_lamp.py            # pyserial end-to-end test driving both boards
  README.md                 # behaviour, building, flashing, pairing with Hue
```

## Zigbee implementation (based on SDK `examples/home_automation_devices/color_dimmable_light`, v2 API)
- **Role:** Router (`EZB_NWK_DEVICE_TYPE_ROUTER`), which suits a mains light. Commissioning steers to all channels (0x07FFF800).
- **Device:** `ezb_zha_create_dimmable_light(ep 10, EZB_ZHA_DIMMABLE_LIGHT_CONFIG())`, device ID 0x0101. It includes the Basic, Identify, Groups, Scenes, On/Off and Level clusters. Groups and scenes matter because Hue uses them for rooms.
- **Hue compatibility items** (from the [wejn.org Hue write-up](https://wejn.org/2025/01/zigbee-hue-llo-world/) and SDK issues [#358](https://github.com/espressif/esp-zigbee-sdk/issues/358) and [#168](https://github.com/espressif/esp-zigbee-sdk/issues/168)):
  - Endpoint `app_device_version = 1`. The Hue Bridge caches device info, so get this right before the first pairing.
  - Add the On/Off attributes `GlobalSceneControl`, `OnTime`, `OffWaitTime` and `StartUpOnOff`, and the Level attribute `StartUpCurrentLevel`. These are needed for "off with effect" and power-on behaviour.
  - Basic attributes: manufacturer name, model ID, SW build ID, power source = mains.
  - Joining: try the standard Zigbee 3.0 TC link key first. If the Hue Bridge rejects the join, use the Kconfig option `LAMP_ALT_GLOBAL_LINK_KEY` (empty by default; you supply the value) with `ezb_secur_set_global_link_key()`, and enable distributed-security joining.
  - Leave received from the network (light deleted in Hue): call `esp_zigbee_factory_reset()`, which reboots into pairing mode.
- **Signals:** on first start or when factory-new, run network steering and retry every 10 s indefinitely while unjoined, so the light is found whenever the Hue app searches. The LED shows a slow blue pulse while pairing. On reboot, rejoin normally.
- **Attribute callback** (`EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID`): the On/Off and CurrentLevel values feed `light_output_set(on, level)`. Remote transitions come in as a series of CurrentLevel updates from the stack's level-control server, and the renderer smooths them.
- **Identify:** while IdentifyTime > 0, blink the LED. Hue's "blink" feature uses this.
- **Local changes:** `zigbee_light_local_set(on, level)` takes `esp_zigbee_lock_acquire` and calls `ezb_zcl_set_attribute_val` on the On/Off and CurrentLevel attributes so the attribute table stays the source of truth. Configure default reporting (On/Off on change, Level min 1 s / max 300 s, delta 1) so Hue and the jig see button changes. While ramping, update the attribute about every 100 ms and update the LED output directly at 100 Hz.
- **Persistence:** check during implementation whether the stack restores On/Off and Level after a reboot. If not, save them to NVS ourselves with a 2 s debounce and restore them at boot.

## LED output
- `led_strip` RMT driver, 1 pixel, GRB, GPIO8. The level (1–254) maps to brightness through a gamma (~2.2) curve. Clamp the lowest nonzero level to 1 so minimum brightness is still visible. Apply a global brightness cap (Kconfig) because the onboard LED is very bright.
- The render task interpolates the current output toward the target (≈150 ms time constant) for smooth changes. Status effects (pairing pulse, identify blink, reset blink) override normal output.

## Test jig (Board B) and verification
1. **Build check:** `idf.py set-target esp32c6 && idf.py build` with no warnings in our code.
2. **Jig:** `test/jig_setup.sh` builds the SDK's `examples/all_device_types_app` console firmware and flashes Board B. The script configures it as a coordinator: `zha add 1 on_off_switch` (plus a dimmer-switch endpoint if available), `dm register`, `bdb_comm start form`, `network open -t 180`.
3. **Flash Board A** with `erase-flash` and then `flash`.
4. **Automated e2e test** (`test/test_lamp.py`, pyserial on both by-id ports):
   - Board A joins the jig network. Check the device announce on Board B and the "steering success" log on Board A.
   - **Remote control:** send On/Off/Toggle and Move-to-Level (with transition) from the jig with `zcl send_raw`, then read back the attributes with `zcl send_gen read`. Confirm Board A logs the matching output level.
   - **Local control:** use the Board A test console commands `btn press` and `btn hold 1500` to inject button events. Confirm the attribute changes on Board A, and that the jig reads and receives reports with the new on/off and level values. Check that hold direction alternates and stops at the limits.
   - **Reboot persistence:** reset Board A over USB-JTAG (esptool `--after hard_reset`). It should rejoin, and its state should be restored.
   - **Leave/reset:** send the `factory_reset` console command, or a ZDO leave from the jig. Board A should wipe and rejoin.
5. **Manual checks by you:**
   - Press the real button to check the toggle and the feel of hold-to-dim.
   - Factory reset: press within 1 s of boot and hold 2 s.
   - Hue: delete or reset the light, run "Add lights" in the Hue app, and check that it appears and can be dimmed and toggled. Button changes should show up in the app. If the Hue Bridge rejects the join, apply the alternate-key fallback described above. If you give me the bridge IP and an API key, I can drive the light search and the checks through the bridge's local API.
