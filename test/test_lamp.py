#!/usr/bin/env python3
"""End-to-end test of the Zigbee lamp using a second board as coordinator (test jig).

Board A (lamp) runs this project's firmware with CONFIG_LAMP_TEST_CONSOLE=y.
Board B (jig) runs Espressif's all_device_types_app console (see jig_setup.sh).

Usage:  python3 test_lamp.py [--lamp PORT] [--jig PORT] [--log FILE] [-k NAME]
"""

import argparse
import re
import sys
import time
import traceback

from board import Board

LAMP_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:5F:90-if00"
JIG_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_AC:EB:E6:2C:71:54-if00"

LAMP_EP = 11
JIG_EP = 1

CL_BASIC, CL_IDENTIFY, CL_ON_OFF, CL_LEVEL = 0x0000, 0x0003, 0x0006, 0x0008
HEX_DUMP = re.compile(r":\s+0x[0-9a-f]{8}\s+((?:[0-9a-f]{2} {1,2})+)")


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


class Rig:
    def __init__(self, lamp_port, jig_port, log):
        self.jig = Board("jig", jig_port, log)
        self.lamp = Board("lamp", lamp_port, log)
        self.addr = None
        self.jig_ieee = None

    # ---- jig helpers -------------------------------------------------------

    def zcl_dst(self):
        return f"-d {self.addr} --dst-ep {LAMP_EP} -e {JIG_EP}"

    def jig_cmd(self, cmd, delay=0.3):
        self.jig.send(cmd)
        time.sleep(delay)

    def zcl_raw(self, cluster, cmd, payload=None):
        p = f" -p 0x{payload}" if payload else ""
        self.jig.send(f"zcl send_raw {self.zcl_dst()} -c {cluster} --cmd 0x{cmd:02x}{p}")
        m = self.jig.expect(r"Default response: endpoint\(\d+\) cluster\((0x[0-9a-f]+)\)", 5)
        st = self.jig.expect(r"- command\((0x[0-9a-f]+)\), status\((0x[0-9a-f]+)\)", 2)
        check(int(st.group(2), 16) == 0, f"command 0x{cmd:02x} on cluster {cluster:#x} failed: status {st.group(2)}")

    def read_attr(self, cluster, attr):
        """Read an attribute from the lamp; returns the raw value bytes."""
        self.jig.send(f"zcl send_gen read {self.zcl_dst()} -c {cluster} -a {attr}")
        self.jig.expect(rf"Read attribute response: endpoint\(\d+\) cluster\(0x{cluster:04x}\)", 5)
        mark = self.jig.cursor
        self.jig.expect(rf"- attribute\(0x{attr:04x}\)", 2)
        # The hex dump (16 bytes per line) may share the attribute line or follow it.
        time.sleep(0.3)
        data = []
        for line in self.jig.lines_after(mark - 1)[:6]:
            m = HEX_DUMP.search(line)
            if m:
                data += [int(b, 16) for b in m.group(1).split()]
            elif data:
                break
        if data:
            return bytes(data)
        raise Fail(f"no value in read response for cluster {cluster:#x} attr {attr:#x}")

    def read_u8(self, cluster, attr):
        return self.read_attr(cluster, attr)[0]

    def write_attr(self, cluster, attr, zcl_type, value_hex):
        self.jig.send(f"zcl send_gen write {self.zcl_dst()} -c {cluster} -a {attr} -t {zcl_type} -v 0x{value_hex}")
        st = self.jig.expect(r"- attribute\(0x[0-9a-f]+\), status\((0x[0-9a-f]+)\)", 5)
        check(int(st.group(1), 16) == 0, f"write of {cluster:#x}/{attr:#x} failed: status {st.group(1)}")

    # ---- lamp helpers ------------------------------------------------------

    def lamp_status(self):
        self.lamp.send("status")
        m = self.lamp.expect(r"STATUS on=(\d) level=(\d+) joined=(\d) short=(0x[0-9a-f]+).* out=(0x[0-9a-f]+)", 5)
        return {"on": int(m.group(1)), "level": int(m.group(2)), "joined": int(m.group(3)), "short": m.group(4),
                "out": int(m.group(5), 16)}

    def wait_level_settled(self, timeout=5.0):
        """Wait until the lamp stops reporting LIGHT changes for 0.7 s."""
        end = time.time() + timeout
        last = self.lamp.mark()
        quiet_since = time.time()
        while time.time() < end:
            time.sleep(0.1)
            now = self.lamp.mark()
            if now != last:
                last = now
                quiet_since = time.time()
            elif time.time() - quiet_since > 0.7:
                return
        raise Fail("lamp output did not settle")

    # ---- setup -------------------------------------------------------------

    def setup_network(self):
        """Fresh coordinator network on the jig, factory-new lamp joins it."""
        self.jig.wait_ready()
        self.lamp.wait_ready()

        self.jig.send("factoryreset")
        self.jig.wait_ready()
        for c in ["role zc", "network type c", "zha add 1 dimmer_switch",
                  # client clusters so the jig can read/receive reports for these
                  f"dm add -e {JIG_EP} -c 0 -r C", f"dm add -e {JIG_EP} -c 3 -r C",
                  f"dm add -e {JIG_EP} -c 4 -r C", f"dm add -e {JIG_EP} -c 5 -r C",
                  "dm register", "start"]:
            self.jig_cmd(c, 0.5)
        time.sleep(2)
        self.jig.send("bdb_comm start form")
        self.jig.expect("Formed network successfully", 20)
        self.jig.send("network open -t 254")
        self.jig.expect(r"is open for \d+ seconds", 5)
        self.jig.send("address -x")
        self.jig_ieee = self.jig.expect(r"^(0x[0-9a-f]{16})$", 3).group(1)

        self.lamp.send("factory_reset")
        m = self.jig.expect(r"New device commissioned or rejoined \(short: (0x[0-9a-f]+)\)", 120)
        self.addr = m.group(1)
        self.lamp.expect(r"Joined network", 30)
        time.sleep(2)


# ---------------------------------------------------------------------------
# Tests


def test_join_and_descriptor(r: Rig):
    st = r.lamp_status()
    check(st["joined"] == 1, "lamp not joined")
    check(st["short"] == r.addr, f"lamp short {st['short']} != announced {r.addr}")
    r.jig.send(f"zdo request simple_desc -e {LAMP_EP} -d {r.addr}")
    m = r.jig.expect(r"ep:(\d+) profile:(0x[0-9a-f]+) dev:(0x[0-9a-f]+) dev_ver:(0x[0-9a-f]+)", 5)
    check(int(m.group(2), 16) == 0x0104, "profile is not HA")
    check(int(m.group(3), 16) == 0x0101, "device is not a Dimmable Light (0x0101)")
    # esp-zigbee-lib 2.0.4 swaps the version/reserved nibbles both when sending and when parsing simple
    # descriptors. The lamp compensates so version 1 goes out on air (verified with the sniffer; Hue needs
    # it), which the jig, using the same library, therefore decodes as 0.
    check(int(m.group(4), 16) == 0, "app device version byte is not 0x01 on air (Hue requirement)")


def test_basic_attributes(r: Rig):
    manuf = r.read_attr(CL_BASIC, 0x0004)
    model = r.read_attr(CL_BASIC, 0x0005)
    power = r.read_u8(CL_BASIC, 0x0007)
    print(f"    manufacturer={manuf[1:].decode()!r} model={model[1:].decode()!r} power_source={power}")
    check(manuf[0] == len(manuf) - 1 and manuf[0] > 0, "manufacturer name missing")
    check(model[0] == len(model) - 1 and model[0] > 0, "model identifier missing")
    check(power == 0x01, "power source is not mains")


def test_remote_on_off(r: Rig):
    r.zcl_raw(CL_ON_OFF, 0x00)  # Off
    r.lamp.expect(r"LIGHT on=0 .*src=remote", 3)
    check(r.read_u8(CL_ON_OFF, 0) == 0, "OnOff not 0 after Off")
    r.zcl_raw(CL_ON_OFF, 0x01)  # On
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 3)
    check(r.read_u8(CL_ON_OFF, 0) == 1, "OnOff not 1 after On")
    r.zcl_raw(CL_ON_OFF, 0x02)  # Toggle
    r.lamp.expect(r"LIGHT on=0 .*src=remote", 3)
    r.zcl_raw(CL_ON_OFF, 0x02)
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 3)
    check(r.lamp_status()["on"] == 1, "lamp not on after double toggle")


def test_remote_level(r: Rig):
    r.zcl_raw(CL_LEVEL, 0x04, "C80000")  # Move to Level with On/Off: 200, immediate
    r.wait_level_settled()
    check(r.read_u8(CL_LEVEL, 0) == 200, "CurrentLevel != 200")
    r.lamp.skip()
    r.zcl_raw(CL_LEVEL, 0x00, "1E0A00")  # Move to Level: 30 over 1.0 s
    r.wait_level_settled()
    steps = [int(m) for m in re.findall(r"LIGHT on=1 level=(\d+) src=remote", "\n".join(r.lamp.lines_after(r.lamp.cursor)))]
    check(r.read_u8(CL_LEVEL, 0) == 30, "CurrentLevel != 30 after transition")
    check(len(steps) >= 5, f"transition not gradual (steps: {steps})")
    st = r.lamp_status()
    check(st["level"] == 30 and st["on"] == 1, f"lamp state {st}")


def test_identify(r: Rig):
    r.jig.send(f"zcl send_raw {r.zcl_dst()} -c {CL_IDENTIFY} --cmd 0x00 -p 0x0200")  # Identify 2 s
    r.lamp.expect(r"Identify for 2 s", 3)


def test_off_with_effect(r: Rig):
    """Hue switches off with 'Off with effect'; esp-zigbee-lib 2.0.4 crashes on it unless the app handles it."""
    r.zcl_raw(CL_ON_OFF, 0x01)
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 3)
    r.zcl_raw(CL_ON_OFF, 0x40, "0000")  # fade-out effect; zcl_raw checks the Default Response status
    r.lamp.expect(r"Off with effect 0x00/0x00", 3)
    time.sleep(1)
    check(r.read_u8(CL_ON_OFF, 0) == 0, "OnOff not 0 after Off with effect")
    r.zcl_raw(CL_ON_OFF, 0x01)
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 3)
    r.zcl_raw(CL_ON_OFF, 0x40, "0000")  # again after On (GlobalSceneControl is TRUE again)
    r.lamp.expect(r"Off with effect", 3)
    st = r.lamp_status()
    check(st["on"] == 0 and st["joined"] == 1, f"unexpected state after second Off with effect: {st}")
    check(not any("Guru Meditation" in l for l in r.lamp.lines), "lamp crashed")
    r.zcl_raw(CL_ON_OFF, 0x01)


GROUP = 0x1234


def group_raw(r: Rig, cluster, cmd, payload=None):
    p = f" -p 0x{payload}" if payload else ""
    r.jig.send(f"zcl send_raw -d 0x{GROUP:04x} -e {JIG_EP} -c {cluster} --cmd 0x{cmd:02x}{p}")


def test_groups(r: Rig):
    """Hue switches rooms with group-addressed commands, including Off with effect."""
    CL_GROUPS = 0x0004
    g = f"{GROUP & 0xFF:02x}{GROUP >> 8:02x}"
    r.jig.send(f"zcl send_raw {r.zcl_dst()} -c {CL_GROUPS} --cmd 0x00 -p 0x{g}00")  # Add Group (empty name)
    time.sleep(1.5)  # answered with an Add Group Response, which the jig console can't print
    group_raw(r, CL_ON_OFF, 0x00)          # group-addressed Off
    r.lamp.expect(r"LIGHT on=0 .*src=remote", 5)
    group_raw(r, CL_ON_OFF, 0x01)          # group-addressed On
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 5)
    group_raw(r, CL_LEVEL, 0x04, "600000") # group-addressed Move to Level 96
    r.wait_level_settled()
    check(r.read_u8(CL_LEVEL, 0) == 96, "group Move to Level not applied")
    group_raw(r, CL_ON_OFF, 0x40, "0000")  # group-addressed Off with effect (Hue room off)
    r.lamp.expect(r"Off with effect", 5)
    time.sleep(1)
    st = r.lamp_status()
    check(st["on"] == 0 and st["joined"] == 1, f"unexpected state after group Off with effect: {st}")
    check(not any("Guru Meditation" in l for l in r.lamp.lines), "lamp crashed")
    group_raw(r, CL_ON_OFF, 0x01)
    r.lamp.expect(r"LIGHT on=1 .*src=remote", 5)


def test_scenes(r: Rig):
    """Store and recall a scene. Known to fail: esp-zigbee-lib 2.0.4 stores no scenes (recall -> NOT_FOUND)."""
    CL_SCENES = 0x0005
    g = f"{GROUP & 0xFF:02x}{GROUP >> 8:02x}"
    r.zcl_raw(CL_LEVEL, 0x04, "500000")    # on, level 80
    r.wait_level_settled()
    r.jig.send(f"zcl send_raw {r.zcl_dst()} -c {CL_SCENES} --cmd 0x04 -p 0x{g}07")  # Store Scene 7
    time.sleep(1.5)
    r.zcl_raw(CL_LEVEL, 0x04, "F00000")    # level 240
    r.wait_level_settled()
    r.jig.send(f"zcl send_raw {r.zcl_dst()} -c {CL_SCENES} --cmd 0x05 -p 0x{g}07")  # Recall Scene 7
    st = r.jig.expect(r"- command\(0x05\), status\((0x[0-9a-f]+)\)", 5)
    check(int(st.group(1), 16) == 0, f"Recall Scene failed with status {st.group(1)}")
    r.wait_level_settled()
    check(r.read_u8(CL_LEVEL, 0) == 80, "scene recall did not restore level 80")


def test_unsolicited_report(r: Rig):
    """Without any binding (as with Hue), a button change is reported to the last controller."""
    r.zcl_raw(CL_ON_OFF, 0x01)  # makes the jig the lamp's controller
    r.wait_level_settled()
    r.jig.skip()
    r.lamp.send("btn press 150")
    r.lamp.expect(r"Toggle -> off", 2)
    check(expect_report(r, CL_ON_OFF, 3) == 0, "unsolicited on/off report has wrong value")
    r.lamp.send("btn press 150")
    r.lamp.expect(r"Toggle -> on", 2)
    check(expect_report(r, CL_ON_OFF, 3) == 1, "unsolicited on/off report has wrong value")


def test_reporting_setup(r: Rig):
    for cl in (CL_ON_OFF, CL_LEVEL):
        r.jig.send(f"zdo bind -c {cl} -S {LAMP_EP} -D {JIG_EP} -s {r.addr} -d {r.jig_ieee}")
        time.sleep(1)
    r.jig.send(f"zcl send_gen config_rp {r.zcl_dst()} -c 6 -a 0 -t 0x10 --min 0 --max 300")
    r.jig.expect(r"Config report response", 5)
    r.jig.send(f"zcl send_gen config_rp {r.zcl_dst()} -c 8 -a 0 -t 0x20 --min 1 --max 300 --delta 0x01")
    r.jig.expect(r"Config report response", 5)


def expect_report(r: Rig, cluster, timeout=5):
    """Wait for an attribute report for cluster attr 0 from the lamp; return its value byte."""
    end = time.time() + timeout
    while time.time() < end:
        r.jig.expect(rf"Report attribute: endpoint\(\d+\) cluster\(0x{cluster:04x}\)", end - time.time())
        mark = r.jig.cursor
        r.jig.expect(r"- attribute\(0x0000\)", 2)
        for line in r.jig.lines_after(mark)[:3]:
            m = HEX_DUMP.search(line)
            if m:
                return int(m.group(1).split()[0], 16)
    raise Fail(f"no report for cluster {cluster:#x}")


def test_button_toggle(r: Rig):
    before = r.lamp_status()
    r.jig.skip()
    r.lamp.send("btn press 150")
    r.lamp.expect(r"Toggle -> ", 2)
    val = expect_report(r, CL_ON_OFF)
    check(val == (0 if before["on"] else 1), "on/off report has wrong value")
    check(r.lamp_status()["on"] == (not before["on"]), "lamp did not toggle")
    r.lamp.send("btn press 150")
    r.lamp.expect(r"Toggle -> ", 2)
    check(r.lamp_status()["on"] == before["on"], "lamp did not toggle back")


def test_button_toggle_at_min(r: Rig):
    """At the lowest levels the LED must still physically turn on and off (regression)."""
    # Plain Move to Level: the with-on/off variant turns the light off at the minimum level (ZCL rule).
    r.zcl_raw(CL_ON_OFF, 0x01)
    r.wait_level_settled()
    r.zcl_raw(CL_LEVEL, 0x00, "010000")  # level 1
    r.wait_level_settled()
    time.sleep(1.0)  # let the renderer settle
    st = r.lamp_status()
    check(st["on"] == 1 and st["level"] == 1, f"lamp not on at level 1: {st}")
    check(st["out"] != 0, "LED dark while on at level 1")
    for want_on in (0, 1):
        r.lamp.send("btn press 150")
        r.lamp.expect(r"Toggle -> ", 2)
        time.sleep(1.0)
        st = r.lamp_status()
        check(st["on"] == want_on, f"lamp did not toggle: {st}")
        check((st["out"] != 0) == bool(want_on), f"LED output wrong after toggle: {st}")
    r.zcl_raw(CL_LEVEL, 0x04, "800000")  # leave it at mid level for later tests


def hold(r: Rig, ms):
    r.lamp.send(f"btn hold {ms}")
    start = r.lamp.expect(r"Dim (up|down) from (\d+)", ms / 1000 + 2)
    stop = r.lamp.expect(r"Dim stopped at (\d+)", ms / 1000 + 2)
    time.sleep(0.3)
    return start.group(1), int(start.group(2)), int(stop.group(1))


def test_button_dimming(r: Rig):
    r.zcl_raw(CL_LEVEL, 0x04, "800000")  # on, level 128
    r.wait_level_settled()
    r.jig.skip()

    d1, a1, b1 = hold(r, 1500)
    print(f"    hold 1: {d1} {a1} -> {b1}")
    check(b1 != a1, "level did not change while holding")
    rep = None
    try:
        rep = expect_report(r, CL_LEVEL, 3)
    except (Fail, TimeoutError):
        pass
    check(rep is not None, "no level report after dimming")
    time.sleep(1.5)  # min report interval is 1 s; let the final value go out
    check(r.read_u8(CL_LEVEL, 0) == b1, "CurrentLevel attribute does not match final dim level")

    d2, a2, b2 = hold(r, 1000)
    print(f"    hold 2: {d2} {a2} -> {b2}")
    check(d2 != d1, "direction did not alternate between holds")

    # Long hold up must stop at the maximum.
    if d2 == "up":
        hold(r, 600)  # flip direction so the next is up
    d3, a3, b3 = hold(r, 5000)
    print(f"    hold 3: {d3} {a3} -> {b3}")
    check(d3 == "up" and b3 == 254, f"long hold up did not reach max (dir {d3}, level {b3})")
    d4, _, b4 = hold(r, 800)
    print(f"    hold 4 at max: {d4} -> {b4}")
    check(d4 == "down", "hold at max did not dim down")


def test_button_hold_from_off(r: Rig):
    r.zcl_raw(CL_ON_OFF, 0x00)
    r.wait_level_settled()
    d, a, b = hold(r, 800)
    print(f"    hold from off: {d} {a} -> {b}")
    st = r.lamp_status()
    check(d == "up" and a == 1, "hold from off did not start at min going up")
    check(st["on"] == 1 and st["level"] < 100, f"unexpected state {st}")


def test_persistence(r: Rig):
    r.write_attr(CL_ON_OFF, 0x4003, "0x30", "ff")  # StartUpOnOff = previous (the default is off)
    r.zcl_raw(CL_LEVEL, 0x04, "4D0000")  # on, level 77
    r.wait_level_settled()
    time.sleep(3)  # save debounce is 2 s
    r.lamp.reset()
    r.lamp.expect(r"LIGHT on=1 level=77 src=boot", 10)
    r.lamp.expect(r"Rejoined network", 15)
    r.lamp.wait_ready()
    time.sleep(2)
    check(r.read_u8(CL_LEVEL, 0) == 77, "level attribute not restored after reboot")
    check(r.read_u8(CL_ON_OFF, 0) == 1, "on/off attribute not restored after reboot")
    r.write_attr(CL_ON_OFF, 0x4003, "0x30", "00")  # back to the default (off)
    time.sleep(1)


def test_startup_on_off(r: Rig):
    # The default is off; check that a written StartUpOnOff = on overrides it and is persisted.
    r.write_attr(CL_ON_OFF, 0x4003, "0x30", "01")  # StartUpOnOff = on
    r.zcl_raw(CL_ON_OFF, 0x00)  # off
    time.sleep(3)
    r.lamp.reset()
    r.lamp.expect(r"LIGHT on=1 level=\d+ src=boot", 10)
    r.lamp.expect(r"Rejoined network", 15)
    r.lamp.wait_ready()
    time.sleep(2)
    check(r.read_u8(CL_ON_OFF, 0) == 1, "lamp not on after reboot with StartUpOnOff=1")
    check(r.read_u8(CL_ON_OFF, 0x4003) == 1, "StartUpOnOff not persisted")
    r.write_attr(CL_ON_OFF, 0x4003, "0x30", "00")  # back to the default (off)
    time.sleep(1)


def test_remote_leave(r: Rig):
    """Coordinator removes the lamp (like deleting it in the Hue app): it must reset and re-pair."""
    r.jig.send(f"zdo nwk_leave -d {r.addr}")
    r.lamp.expect(r"Left network \(type 0\)", 10)
    r.lamp.expect(r"Factory new: start network steering", 15)
    r.jig.send("network open -t 200")
    m = r.jig.expect(r"New device commissioned or rejoined \(short: (0x[0-9a-f]+)\)", 120)
    r.addr = m.group(1)
    r.lamp.expect(r"Joined network", 30)
    time.sleep(2)
    st = r.lamp_status()
    check(st["joined"] == 1, "lamp did not rejoin")
    check(st["on"] == 0 and st["level"] == 254, f"state not reset to defaults: {st}")


TESTS = [
    test_join_and_descriptor,
    test_basic_attributes,
    test_remote_on_off,
    test_remote_level,
    test_identify,
    test_off_with_effect,
    test_groups,
    test_scenes,
    test_unsolicited_report,
    test_reporting_setup,
    test_button_toggle,
    test_button_toggle_at_min,
    test_button_dimming,
    test_button_hold_from_off,
    test_persistence,
    test_startup_on_off,
    test_remote_leave,
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lamp", default=LAMP_PORT)
    ap.add_argument("--jig", default=JIG_PORT)
    ap.add_argument("--log", default="test_lamp.log")
    ap.add_argument("-k", help="only run tests whose name contains this")
    args = ap.parse_args()

    rig = Rig(args.lamp, args.jig, args.log)
    print("Setting up network...")
    rig.setup_network()
    print(f"Lamp joined as {rig.addr}; jig IEEE {rig.jig_ieee}")

    # Known library limitations: reported, but don't fail the run.
    expected_fail = {test_scenes}
    failed = []
    xfailed = []
    ran = 0
    for t in TESTS:
        if args.k and args.k not in t.__name__ and t is not test_reporting_setup:
            continue
        ran += 1
        try:
            t(rig)
            print(f"{'XPASS' if t in expected_fail else 'PASS'} {t.__name__}")
        except (Fail, TimeoutError) as e:
            if t in expected_fail:
                xfailed.append(t.__name__)
                print(f"XFAIL {t.__name__}: {e}")
                continue
            failed.append(t.__name__)
            print(f"FAIL {t.__name__}: {e}")
        except Exception:
            failed.append(t.__name__)
            print(f"ERROR {t.__name__}")
            traceback.print_exc()

    rig.jig.close()
    rig.lamp.close()
    print(f"\n{ran - len(failed) - len(xfailed)} passed, {len(xfailed)} expected failures, {len(failed)} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
