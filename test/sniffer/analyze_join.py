#!/usr/bin/env python3
"""Decode a sniffer capture of a Zigbee join attempt and test candidate trust-center link keys.

Input lines (from the sniffer firmware, optionally prefixed with a timestamp):
    [ts] F <channel> <rssi> <lqi> <hex PSDU incl. 2-byte FCS>

It finds MAC association exchanges for the joining device, then any APS-secured frames sent
to it without NWK security (that is how the network key is delivered: APS Transport-Key).
Each such frame is tried against the candidate link keys:
  - key id 2 (key-transport key): AES-CCM* with HMAC-MMO(K, 0x00)
  - key id 0 (data key):          AES-CCM* with K itself
Decrypted network keys are never printed, only reported as recovered.

Usage: analyze_join.py CAPTURE --joiner AC:EB:E6:FF:FE:2C:5F:90 [--key HEX]...
"""

import argparse
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.ciphers.aead import AESCCM

STANDARD_KEYS = {
    "Zigbee 3.0 default TC link key (ZigBeeAlliance09)": bytes.fromhex("5A6967426565416C6C69616E63653039"),
    "Zigbee 3.0 distributed global key": bytes.fromhex("D0D1D2D3D4D5D6D7D8D9DADBDCDDDEDF"),
}

APS_CMDS = {0x05: "Transport-Key", 0x06: "Update-Device", 0x07: "Remove-Device", 0x08: "Request-Key",
            0x09: "Switch-Key", 0x0E: "Tunnel", 0x0F: "Verify-Key", 0x10: "Confirm-Key"}
MAC_CMDS = {0x01: "Association Request", 0x02: "Association Response", 0x04: "Data Request",
            0x07: "Beacon Request"}
NWK_CMDS = {0x04: "Leave", 0x05: "Route Record", 0x06: "Rejoin Request", 0x07: "Rejoin Response",
            0x08: "Link Status"}


def aes_ecb(key, block):
    e = Cipher(algorithms.AES(key), modes.ECB()).encryptor()
    return e.update(block) + e.finalize()


def mmo_hash(data):
    """Matyas-Meyer-Oseas hash (Zigbee spec B.6), for messages < 2^16 bits."""
    bitlen = len(data) * 8
    padded = data + b"\x80"
    while (len(padded) + 2) % 16:
        padded += b"\x00"
    padded += struct.pack(">H", bitlen)
    h = bytes(16)
    for i in range(0, len(padded), 16):
        m = padded[i:i + 16]
        h = bytes(a ^ b for a, b in zip(aes_ecb(h, m), m))
    return h


def zigbee_hmac(key, msg):
    ipad = bytes(k ^ 0x36 for k in key)
    opad = bytes(k ^ 0x5C for k in key)
    return mmo_hash(opad + mmo_hash(ipad + msg))


def ext(b):
    return ":".join(f"{x:02X}" for x in reversed(b))


class Frame:
    def __init__(self, ts, raw):
        self.ts = ts
        self.raw = raw


def parse_mac(psdu):
    if len(psdu) < 5:
        return None
    fcf = struct.unpack_from("<H", psdu, 0)[0]
    ftype = fcf & 7
    sec = (fcf >> 3) & 1
    pan_comp = (fcf >> 6) & 1
    dmode = (fcf >> 10) & 3
    smode = (fcf >> 14) & 3
    o = 3
    mac = {"type": ftype, "seq": psdu[2], "sec": sec}
    if dmode:
        mac["dpan"] = struct.unpack_from("<H", psdu, o)[0]
        o += 2
        if dmode == 2:
            mac["dst"] = f"0x{struct.unpack_from('<H', psdu, o)[0]:04x}"
            o += 2
        else:
            mac["dst"] = ext(psdu[o:o + 8])
            o += 8
    if smode:
        if not pan_comp:
            o += 2
        if smode == 2:
            mac["src"] = f"0x{struct.unpack_from('<H', psdu, o)[0]:04x}"
            o += 2
        else:
            mac["src"] = ext(psdu[o:o + 8])
            o += 8
    mac["payload"] = psdu[o:-2]  # strip FCS
    return mac


def parse_nwk(p):
    if len(p) < 8:
        return None
    fcf = struct.unpack_from("<H", p, 0)[0]
    n = {"type": fcf & 3, "sec": (fcf >> 9) & 1}
    n["dst"] = f"0x{struct.unpack_from('<H', p, 2)[0]:04x}"
    n["src"] = f"0x{struct.unpack_from('<H', p, 4)[0]:04x}"
    o = 8
    if (fcf >> 11) & 1:
        n["dst_ieee"] = ext(p[o:o + 8])
        o += 8
    if (fcf >> 12) & 1:
        n["src_ieee"] = ext(p[o:o + 8])
        o += 8
    if (fcf >> 8) & 1:
        o += 1
    if (fcf >> 10) & 1:
        cnt = p[o]
        o += 2 + 2 * cnt
    n["payload"] = p[o:]
    return n


def parse_aps_secured(p):
    """Return the parts needed to decrypt an APS-secured command frame, or None."""
    fcf = p[0]
    if not (fcf >> 5) & 1:
        return None
    ftype = fcf & 3
    delivery = (fcf >> 2) & 3
    o = 1
    if ftype == 0:  # data frame: dst ep, cluster, profile, src ep (unicast)
        if delivery == 3:
            o += 2
        else:
            o += 1
        o += 4 + 1
    elif ftype == 1 and delivery == 3:
        o += 2
    o += 1  # APS counter
    hdr_end = o
    sc = p[o]
    level = sc & 7
    key_id = (sc >> 3) & 3
    ext_nonce = (sc >> 5) & 1
    fc = p[o + 1:o + 5]
    o += 5
    src = None
    if ext_nonce:
        src = p[o:o + 8]
        o += 8
    if key_id == 1:
        o += 1
    return {"ftype": ftype, "hdr": p[:hdr_end], "sc": sc, "level": level, "key_id": key_id,
            "fc": fc, "src": src, "aux": p[hdr_end:o], "body": p[o:]}


def try_decrypt(aps, link_key, src_ieee_le):
    key = zigbee_hmac(link_key, b"\x00") if aps["key_id"] == 2 else link_key
    sc = (aps["sc"] & ~7) | 5  # security level is always 5 on the wire-reconstructed frame
    src = aps["src"] or src_ieee_le
    if src is None:
        return None
    nonce = src + aps["fc"] + bytes([sc])
    aad = aps["hdr"] + bytes([sc]) + aps["aux"][1:]
    try:
        return AESCCM(key, tag_length=4).decrypt(nonce, aps["body"], aad)
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--joiner", required=True, help="IEEE address of the joining device, e.g. AC:EB:...")
    ap.add_argument("--key", action="append", default=[], help="candidate link key (hex, spaces allowed)")
    args = ap.parse_args()

    joiner = args.joiner.upper()
    keys = dict(STANDARD_KEYS)
    for i, k in enumerate(args.key):
        keys[f"candidate #{i + 1}"] = bytes.fromhex(k.replace(" ", "").replace(":", ""))

    frames = []
    for line in open(args.capture):
        parts = line.split()
        if "F" not in parts:
            continue
        i = parts.index("F")
        ts = float(parts[0]) if i > 0 else 0.0
        try:
            frames.append(Frame(ts, bytes.fromhex(parts[i + 4])))
        except (IndexError, ValueError):
            pass
    print(f"{len(frames)} frames")

    joiner_shorts = set()
    t0 = frames[0].ts if frames else 0
    candidates = []
    for f in frames:
        mac = parse_mac(f.raw)
        if not mac:
            continue
        involves = joiner in (mac.get("src"), mac.get("dst")) or mac.get("dst") in joiner_shorts
        if mac["type"] == 3 and mac["payload"]:
            cmd = mac["payload"][0]
            if involves and cmd in (0x01, 0x02):
                info = MAC_CMDS.get(cmd, hex(cmd))
                if cmd == 0x02 and len(mac["payload"]) >= 4:
                    short, status = struct.unpack_from("<HB", mac["payload"], 1)
                    info += f": short 0x{short:04x}, status {status}"
                    if status == 0:
                        joiner_shorts.add(f"0x{short:04x}")
                print(f"{f.ts - t0:8.2f}s MAC {mac.get('src')} -> {mac.get('dst')}  {info}")
            continue
        if mac["type"] != 1 or not involves:
            continue
        nwk = parse_nwk(mac["payload"])
        if not nwk:
            continue
        if nwk["sec"]:
            print(f"{f.ts - t0:8.2f}s NWK {nwk['src']} -> {nwk['dst']}  NWK-secured frame (needs network key)")
            continue
        if nwk["type"] == 1 and nwk["payload"]:
            print(f"{f.ts - t0:8.2f}s NWK {nwk['src']} -> {nwk['dst']}  "
                  f"unsecured NWK command {NWK_CMDS.get(nwk['payload'][0], hex(nwk['payload'][0]))}")
            continue
        aps = parse_aps_secured(nwk["payload"]) if nwk["payload"] else None
        if not aps:
            print(f"{f.ts - t0:8.2f}s NWK {nwk['src']} -> {nwk['dst']}  unsecured APS frame")
            continue
        print(f"{f.ts - t0:8.2f}s APS {nwk['src']} -> {nwk['dst']}  APS-secured frame, key id {aps['key_id']}"
              f" ({ {0: 'data/link key', 1: 'network key', 2: 'key-transport key', 3: 'key-load key'}[aps['key_id']] })"
              f", nonce source {ext(aps['src']) if aps['src'] else '-'}")
        candidates.append((f, nwk, aps))

    print()
    if not candidates:
        print("No APS-secured frames to the joiner were captured: the network key was never sent "
              "(or the sniffer missed it).")
        return 2

    any_ok = False
    for f, nwk, aps in candidates:
        for name, key in keys.items():
            pt = try_decrypt(aps, key, None)
            if pt is not None:
                any_ok = True
                cmd = APS_CMDS.get(pt[0], hex(pt[0]))
                detail = ""
                if pt[0] == 0x05 and len(pt) > 1:
                    detail = f", key type {pt[1]} ({'network key' if pt[1] == 1 else 'other'}) [value withheld]"
                print(f"{f.ts - t0:8.2f}s decrypted with {name}: APS {cmd}{detail}")
                break
        else:
            print(f"{f.ts - t0:8.2f}s frame did NOT decrypt with any of: {', '.join(keys)}")
    return 0 if any_ok else 1


if __name__ == "__main__":
    sys.exit(main())
