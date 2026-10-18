# SPDX-License-Identifier: MIT
"""Decode BLE advertising PDUs; shared origin documented in NOTICE."""

TYPES = (
    "ADV_IND",
    "ADV_DIRECT_IND",
    "ADV_NONCONN_IND",
    "SCAN_REQ",
    "SCAN_RSP",
    "CONNECT_IND",
    "ADV_SCAN_IND",
    "ADV_EXT_IND",
)


def advertisement(hex_pdu):
    p = bytes.fromhex(hex_pdu)
    if len(p) < 2 or len(p) != p[1] + 2:
        raise ValueError("PDU length mismatch")
    kind = p[0] & 15
    if kind > 7:
        raise ValueError("reserved PDU")
    body = p[2:]
    out = {
        "kind": TYPES[kind],
        "address": None,
        "name": "",
        "fields": [],
        "random": bool(p[0] & 64),
        "raw": hex_pdu,
        "extended": kind == 7,
    }
    if kind in (0, 1, 2, 4, 6) and len(body) >= 6:
        out["address"] = ":".join(f"{b:02X}" for b in body[:6][::-1])
    elif kind in (3, 5) and len(body) >= 12:
        out["address"] = ":".join(f"{b:02X}" for b in body[6:12][::-1])
        out["scanner"] = ":".join(f"{b:02X}" for b in body[:6][::-1])
        out["random"] = bool(p[0] & 128)
    if kind == 7:
        out["note"] = (
            "Primary extended-advertising header; secondary-channel payload is not followed."
        )
    if kind in (0, 2, 4, 6):
        ad = body[6:]
        at = 0
        while at < len(ad):
            n = ad[at]
            at += 1
            if not n:
                break
            if at + n > len(ad):
                out["malformed_ad"] = True
                break
            typ = ad[at]
            value = ad[at + 1 : at + n]
            at += n
            field = {"type": f"0x{typ:02X}", "hex": value.hex()}
            if typ in (8, 9):
                field["label"] = "Local name"
                field["value"] = value.decode("utf-8", errors="replace")
                out["name"] = field["value"]
            elif typ == 1:
                field.update(label="Flags", value=value.hex())
            elif typ == 10 and len(value) == 1:
                field.update(
                    label="TX power",
                    value=f'{int.from_bytes(value,"little",signed=True)} dBm',
                )
            elif typ == 255 and len(value) >= 2:
                field.update(
                    label="Manufacturer data",
                    value=f'Company 0x{int.from_bytes(value[:2],"little"):04X} · {value[2:].hex()}',
                )
            elif typ in (2, 3) and len(value) % 2 == 0:
                field.update(
                    label="16-bit service UUIDs",
                    value=", ".join(
                        f'{int.from_bytes(value[k:k+2],"little"):04X}'
                        for k in range(0, len(value), 2)
                    ),
                )
            elif typ == 22 and len(value) >= 2:
                field.update(
                    label="Service data",
                    value=f'UUID {int.from_bytes(value[:2],"little"):04X} · {value[2:].hex()}',
                )
            else:
                field.update(label=f"AD type 0x{typ:02X}", value=value.hex())
            out["fields"].append(field)
    return out
