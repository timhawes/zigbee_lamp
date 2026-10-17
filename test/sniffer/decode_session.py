#!/usr/bin/env python3
"""Decrypt and summarise the traffic of one device after it joined a network.

The network key is recovered in memory from the captured APS Transport-Key (using the given link
key) and is never printed. Prints ZDO and ZCL exchanges to/from the device's short address.

Usage: decode_session.py CAPTURE --joiner IEEE --link-key HEX [--short 0xNNNN]
"""

import argparse
import struct
import sys

from cryptography.hazmat.primitives.ciphers.aead import AESCCM

sys.path.insert(0, __import__("os").path.dirname(__file__))

from analyze_join import ext, parse_aps_secured, parse_mac, parse_nwk, try_decrypt

ZDO = {0x0000: "NWK_addr_req", 0x0001: "IEEE_addr_req", 0x0002: "Node_Desc_req", 0x0003: "Power_Desc_req",
       0x0004: "Simple_Desc_req", 0x0005: "Active_EP_req", 0x0006: "Match_Desc_req", 0x0013: "Device_annce",
       0x0021: "Bind_req", 0x0031: "Mgmt_Lqi_req", 0x0034: "Mgmt_Leave_req", 0x0036: "Mgmt_Permit_Join_req"}
ZCL_CLUSTERS = {0x0000: "Basic", 0x0003: "Identify", 0x0004: "Groups", 0x0005: "Scenes", 0x0006: "OnOff",
                0x0008: "Level", 0x0300: "Color", 0x1000: "Touchlink", 0x0019: "OTA", 0xFC03: "Hue-FC03"}
ZCL_GLOBAL = {0x00: "Read", 0x01: "Read-Rsp", 0x02: "Write", 0x04: "Write-Rsp", 0x06: "Config-Report",
              0x07: "Config-Report-Rsp", 0x0A: "Report", 0x0B: "Default-Rsp", 0x0C: "Discover-Attr",
              0x0D: "Discover-Attr-Rsp"}


def nwk_decrypt(raw_nwk, nwk_hdr_len, key):
    aux = raw_nwk[nwk_hdr_len:]
    sc = aux[0]
    fc = aux[1:5]
    src = aux[5:13]
    o = 14  # sc + fc + ext src + key seq
    sc5 = (sc & ~7) | 5
    nonce = src + fc + bytes([sc5])
    aad = raw_nwk[:nwk_hdr_len] + bytes([sc5]) + aux[1:o]
    try:
        return AESCCM(key, tag_length=4).decrypt(nonce, aux[o:], aad)
    except Exception:
        return None


def nwk_header_len(p):
    fcf = struct.unpack_from("<H", p, 0)[0]
    o = 8
    if (fcf >> 11) & 1:
        o += 8
    if (fcf >> 12) & 1:
        o += 8
    if (fcf >> 8) & 1:
        o += 1
    if (fcf >> 10) & 1:
        o += 2 + 2 * p[o]
    return o


def describe_aps(aps):
    fcf = aps[0]
    ftype = fcf & 3
    delivery = (fcf >> 2) & 3
    if ftype == 2:
        return "APS ack"
    if ftype == 1:
        return f"APS command 0x{aps[2]:02x}"
    o = 1
    dst_ep = None
    if delivery == 3:
        o += 2
    else:
        dst_ep = aps[o]
        o += 1
    cluster, profile = struct.unpack_from("<HH", aps, o)
    o += 4
    src_ep = aps[o]
    o += 2  # src ep + counter
    if (fcf >> 7) & 1:
        o += 1
    payload = aps[o:]
    if profile == 0:
        name = ZDO.get(cluster & 0x7FFF, f"0x{cluster & 0x7FFF:04x}")
        if cluster & 0x8000:
            status = payload[1] if len(payload) > 1 else None
            return f"ZDO {name.replace('_req', '_rsp')} status={status} {payload[2:].hex()}"
        return f"ZDO {name} {payload[1:].hex()}"
    fc = payload[0]
    o = 1
    manuf = None
    if fc & 0x04:
        manuf = struct.unpack_from("<H", payload, o)[0]
        o += 2
    tsn, cmd = payload[o], payload[o + 1]
    body = payload[o + 2:]
    cl = ZCL_CLUSTERS.get(cluster, f"0x{cluster:04x}")
    kind = ZCL_GLOBAL.get(cmd, f"cmd 0x{cmd:02x}") if (fc & 3) == 0 else f"cluster-cmd 0x{cmd:02x}"
    d = "to-client" if fc & 0x08 else "to-server"
    m = f" manuf=0x{manuf:04x}" if manuf is not None else ""
    return f"ZCL ep{src_ep}->ep{dst_ep} {d} prof=0x{profile:04x} {cl} {kind}{m} {body.hex()}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--joiner", required=True)
    ap.add_argument("--link-key", required=True)
    ap.add_argument("--key-capture", help="capture containing the joiner's key delivery (default: CAPTURE)")
    ap.add_argument("--short", help="joiner short address, if no association is in the capture")
    args = ap.parse_args()
    joiner = args.joiner.upper()
    link_key = bytes.fromhex(args.link_key.replace(" ", ""))

    def load(path):
        out = []
        for line in open(path):
            p = line.split()
            if "F" not in p:
                continue
            try:
                out.append((float(p[0]), bytes.fromhex(p[p.index("F") + 4])))
            except (ValueError, IndexError):
                continue
        return out

    frames = load(args.capture)
    key_frames = load(args.key_capture) if args.key_capture else frames
    t0 = frames[0][0]

    # Recover the network key and the joiner's short address from the last successful key delivery.
    nwk_key, short = None, None
    for ts, raw in key_frames:
        m = parse_mac(raw)
        if m and m["type"] == 3 and m["payload"][:1] == b"\x02" and m.get("dst") == joiner:
            s, status = struct.unpack_from("<HB", m["payload"], 1)
            if status == 0:
                short = f"0x{s:04x}"
        if not m or m["type"] != 1:
            continue
        n = parse_nwk(m["payload"])
        if not n or n["sec"] or not n["payload"]:
            continue
        a = parse_aps_secured(n["payload"])
        if a:
            pt = try_decrypt(a, link_key, None)
            if pt and pt[0] == 0x05 and pt[1] == 1 and ext(pt[19:27]) == joiner:
                nwk_key = pt[2:18]
    if not nwk_key:
        print("network key not recovered")
        return 1
    short = args.short or short
    print(f"network key recovered (withheld); joiner short address {short}")

    seen = set()
    for ts, raw in frames:
        m = parse_mac(raw)
        if not m or m["type"] != 1:
            continue
        p = m["payload"]
        n = parse_nwk(p)
        if not n or not n["sec"] or short not in (n["src"], n["dst"]):
            continue
        hl = nwk_header_len(p)
        pt = nwk_decrypt(p, hl, nwk_key)
        if pt is None:
            continue
        key = (n["src"], n["dst"], pt)
        if key in seen:  # drop MAC-level retransmissions / relayed copies
            continue
        seen.add(key)
        if (struct.unpack_from("<H", p, 0)[0] & 3) == 1:
            desc = f"NWK command 0x{pt[0]:02x}"
        else:
            try:
                desc = describe_aps(pt)
            except (IndexError, struct.error):
                desc = f"APS ? {pt.hex()}"
        who = "lamp->" + n["dst"] if n["src"] == short else n["src"] + "->lamp"
        print(f"{ts - t0:8.2f}s {who:14s} {desc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
