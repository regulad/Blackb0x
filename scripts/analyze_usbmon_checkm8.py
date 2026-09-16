#!/usr/bin/env python3
"""Summarise a usbmon capture of a checkm8 run.

Answers the one question the exploit's own logging can't: did each stage's
control requests actually reach the device, and what did the device do with
them? Every heap-grooming request in checkm8 is *expected* to time out, so
the tool's own "did it time out?" check passes identically whether the
device serviced the request and the host gave up on it (a real leak) or the
host cancelled it before it ever went out (no leak at all, heap untouched).
Only the wire tells those apart.

Capture with (kernel lockdown blocks the debugfs text interface whenever
Secure Boot is on -- libpcap talks to /dev/usbmonN instead, which is not
restricted):

    sudo tcpdump -i usbmon<BUS> -w /tmp/pwn.pcap

then run this against the resulting file. Bus number comes from the kernel
log line for the device ("usb 3-3: ..." is bus 3).
"""

import argparse
import collections
import struct
import sys

# linux/drivers/usb/mon/mon_bin.c's struct mon_bin_hdr, as captured by
# DLT_USB_LINUX_MMAPPED. Fixed 64 bytes, little-endian, no padding (every
# field in that struct already lands on its natural alignment).
#
#   u64 id; u8 type; u8 xfer_type; u8 epnum; u8 devnum; u16 busnum;
#   char flag_setup; char flag_data; s64 ts_sec; s32 ts_usec; int status;
#   unsigned len_urb; unsigned len_cap; union { u8 setup[8]; ... };
#   int interval; int start_frame; unsigned xfer_flags; unsigned ndesc;
#
# `type` is decoded as a char, not a u8: it carries 'S'/'C'/'E' as ASCII.
USBMON_HDR = "<QcBBBHccqiiII8siiII"
USBMON_HDR_LEN = struct.calcsize(USBMON_HDR)
assert USBMON_HDR_LEN == 64, USBMON_HDR_LEN

DLT_USB_LINUX = 189
DLT_USB_LINUX_MMAPPED = 220
DLT_USB_DARWIN = 266

XFER_CONTROL = 2

# Darwin (macOS) pseudo-header, DLT_USB_DARWIN, as captured by
#   sudo ifconfig XHC20 up && sudo tcpdump -i XHC20 -w out.pcap
#
# UNVERIFIED AGAINST A REAL CAPTURE. The field order below comes from
# Wireshark's own dissector (bcd_version, header_len, request_type, io_status,
# io_id, device_location, speed, device_address, endpoint_address,
# endpoint_type, then isochronous fields), but the source fetch truncated
# before the offset arithmetic, and libpcap only defines the DLT number rather
# than the struct. So the exact padding is a judgement call, and two plausible
# layouts are tried and scored against the data instead of one being assumed.
#
# Everything here is checked before any number is reported: header_len has to
# agree with the layout, request_type has to be SUBMIT or COMPLETE, and speed
# and endpoint_type have to be inside their documented enums. If a layout does
# not validate, this refuses to summarise rather than emitting confidently
# wrong statistics -- the same failure the Linux parser had (event_type
# decoded as an integer and compared against bytes) and which only a
# known-answer test caught.
DARWIN_IO_SUBMIT = 0
DARWIN_IO_COMPLETE = 1

DARWIN_LAYOUTS = {
    # Tightly packed, io_status as a byte.
    "packed": {
        "size": 38, "bcd_version": 0, "header_len": 2, "request_type": 4,
        "io_status": 5, "io_status_size": 1, "io_id": 6, "device_location": 14,
        "speed": 18, "device_address": 19, "endpoint_address": 20,
        "endpoint_type": 21,
    },
    # Naturally aligned, io_status as a 32-bit IOReturn (which is what Darwin
    # status codes actually are).
    "aligned": {
        "size": 40, "bcd_version": 0, "header_len": 2, "request_type": 4,
        "io_status": 8, "io_status_size": 4, "io_id": 16, "device_location": 24,
        "speed": 28, "device_address": 29, "endpoint_address": 30,
        "endpoint_type": 31,
    },
}

# Statuses worth naming: these are what distinguish "the device saw it and
# we gave up" from "we pulled it before it went anywhere".
STATUS_NAMES = {
    0: "OK",
    -110: "ETIMEDOUT (device serviced it, host gave up -- a real leak)",
    -104: "ECONNRESET (unlinked/cancelled by the host)",
    -32: "EPIPE (stall -- checkm8's expected stall signal)",
    -71: "EPROTO (protocol error)",
    -75: "EOVERFLOW (babble)",
    -19: "ENODEV (device gone)",
    -2: "ENOENT (unlinked before submission)",
    -115: "EINPROGRESS",
}


def read_pcap(path):
    with open(path, "rb") as fh:
        magic = fh.read(4)
        if magic == b"\xd4\xc3\xb2\xa1":
            endian, nano = "<", False
        elif magic == b"\xa1\xb2\xc3\xd4":
            endian, nano = ">", False
        elif magic == b"\x4d\x3c\xb2\xa1":
            endian, nano = "<", True
        elif magic == b"\xa1\xb2\x3c\x4d":
            endian, nano = ">", True
        else:
            raise SystemExit(
                "%s is not a classic pcap file (pcapng is not supported -- "
                "capture with `tcpdump -w`, not Wireshark's default)" % path
            )
        _, _, _, _, _, link = struct.unpack(endian + "HHiIII", fh.read(20))
        if link not in (DLT_USB_LINUX, DLT_USB_LINUX_MMAPPED, DLT_USB_DARWIN):
            raise SystemExit(
                "link type %d is not a USB capture -- use `-i usbmon<BUS>` on "
                "Linux or `-i XHC20` on macOS" % link
            )
        yield link
        while True:
            rec = fh.read(16)
            if len(rec) < 16:
                return
            _, _, caplen, _ = struct.unpack(endian + "IIII", rec)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            yield data


def parse(data):
    if len(data) < USBMON_HDR_LEN:
        return None
    (
        urb_id, event_type, xfer_type, epnum, devnum, busnum,
        flag_setup, _flag_data, _ts_sec, _ts_usec, status,
        _length, _len_cap, setup, _interval, _start_frame, _xfer_flags, _ndesc,
    ) = struct.unpack(USBMON_HDR, data[:USBMON_HDR_LEN])
    return {
        "id": urb_id,
        "event": event_type,          # b'S' submit, b'C' complete, b'E' error
        "xfer": xfer_type,
        "epnum": epnum,
        "devnum": devnum,
        "busnum": busnum,
        "has_setup": flag_setup == b"\x00",
        "status": status,
        "setup": setup,
        "ts": _ts_sec * 1000000 + _ts_usec,
        "length": _length,            # bytes actually transferred, on a complete
    }


def darwin_valid(data, layout):
    """Whether one record decodes sanely under a candidate Darwin layout."""
    if len(data) < layout["size"]:
        return False
    header_len = struct.unpack_from("<H", data, layout["header_len"])[0]
    # Exact, not a lower bound: header_len is what actually discriminates the
    # candidate layouts from each other, since the enum fields of one layout
    # frequently land on in-range bytes of the other.
    if header_len != layout["size"] or header_len > len(data):
        return False
    if data[layout["request_type"]] not in (DARWIN_IO_SUBMIT, DARWIN_IO_COMPLETE):
        return False
    if data[layout["speed"]] > 4:            # Low/Full/High/Super/SuperPlus
        return False
    if data[layout["endpoint_type"]] > 3:    # Control/Isoc/Bulk/Interrupt
        return False
    return True


def pick_darwin_layout(records):
    """Score both candidate layouts over real records and take a clear winner."""
    scores = {name: sum(1 for r in records if darwin_valid(r, lay))
              for name, lay in DARWIN_LAYOUTS.items()}
    best = max(scores, key=lambda n: scores[n])
    if not records or scores[best] < len(records) * 0.9:
        raise SystemExit(
            "this Darwin capture does not match either candidate pseudo-header "
            "layout (packed scored %d/%d, aligned %d/%d).\n"
            "The Darwin layout in this script is UNVERIFIED -- see its comment. "
            "Rather than print numbers that would be wrong, it stops here. Dump a "
            "few records with `tcpdump -r <file> -xx | head -40` and correct "
            "DARWIN_LAYOUTS." % (scores["packed"], len(records),
                                 scores["aligned"], len(records)))
    return best, DARWIN_LAYOUTS[best]


def parse_darwin(data, layout):
    if not darwin_valid(data, layout):
        return None
    header_len = struct.unpack_from("<H", data, layout["header_len"])[0]
    if layout["io_status_size"] == 1:
        status = struct.unpack_from("<b", data, layout["io_status"])[0]
    else:
        status = struct.unpack_from("<i", data, layout["io_status"])[0]
    is_submit = data[layout["request_type"]] == DARWIN_IO_SUBMIT
    payload = data[header_len:]
    # For a control SUBMIT the 8-byte setup packet leads the payload, the same
    # place the Linux header carries it.
    setup = payload[:8] if (is_submit and len(payload) >= 8) else None
    # Darwin and Linux number their endpoint types differently -- Darwin is
    # Control/Isoc/Bulk/Interrupt = 0/1/2/3, Linux is Isoc/Interrupt/Control/
    # Bulk = 0/1/2/3 -- so Darwin's Control(0) collides with Linux's Isoc(0)
    # and has to be translated, not passed through. Normalised to the Linux
    # numbering here so the rest of this script stays single-path.
    darwin_to_linux_xfer = {0: 2, 1: 0, 2: 3, 3: 1}
    return {
        "id": struct.unpack_from("<Q", data, layout["io_id"])[0],
        "event": b"S" if is_submit else b"C",
        "xfer": darwin_to_linux_xfer.get(data[layout["endpoint_type"]], -1),
        "epnum": data[layout["endpoint_address"]],
        "devnum": data[layout["device_address"]],
        "busnum": 0,
        "has_setup": setup is not None,
        "status": status,
        "setup": setup or b"\0" * 8,
        "ts": 0,
        "length": max(len(payload) - (8 if setup else 0), 0),
    }


def describe(setup):
    bm, breq, wval, widx, wlen = struct.unpack("<BBHHH", setup)
    return (bm, breq, wval, widx, wlen)


def label(key):
    bm, breq, wval, widx, wlen = key
    known = {
        (0x02, 3): "SET_FEATURE(ENDPOINT_HALT) -- checkm8 stall primitive",
        (0x80, 6): "GET_DESCRIPTOR -- checkm8 leak primitive",
        (0x21, 1): "DFU_DNLOAD",
        (0x21, 4): "DFU_CLRSTATUS/ABORT",
        (0xA1, 3): "DFU_GETSTATUS",
        (0x00, 0): "GET_STATUS(device) -- checkm8 overwrite carrier",
    }.get((bm, breq), "")
    return "bmRequestType=0x%02X bRequest=%d wValue=0x%04X wIndex=0x%04X wLength=%d%s" % (
        bm, breq, wval, widx, wlen, ("  [%s]" % known if known else "")
    )


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pcap")
    ap.add_argument("--device", type=int, default=None,
                    help="only this USB device number (default: all)")
    args = ap.parse_args()

    submits = collections.Counter()
    completes = collections.Counter()
    statuses = collections.defaultdict(collections.Counter)
    latencies = collections.defaultdict(list)
    moved = collections.defaultdict(list)
    pending = {}
    devices = collections.Counter()
    total = 0

    stream = read_pcap(args.pcap)
    link = next(stream)
    records = list(stream)

    if link == DLT_USB_DARWIN:
        name, layout = pick_darwin_layout(records[:200])
        print("Darwin (macOS) capture, using the '%s' pseudo-header layout.\n"
              "NOTE: that layout is unverified against real hardware -- it validated "
              "against this file's own header_len and enum fields, but sanity-check "
              "the first result you get from it.\n" % name)
        decode = lambda d: parse_darwin(d, layout)
    else:
        decode = parse

    for data in records:
        pkt = decode(data)
        if pkt is None or pkt["xfer"] != XFER_CONTROL:
            continue
        if args.device is not None and pkt["devnum"] != args.device:
            continue
        total += 1
        devices[pkt["devnum"]] += 1
        if pkt["event"] == b"S" and pkt["has_setup"]:
            key = describe(pkt["setup"])
            submits[key] += 1
            pending[pkt["id"]] = (key, pkt["ts"])
        elif pkt["event"] in (b"C", b"E"):
            entry = pending.pop(pkt["id"], None)
            if entry is None:
                continue
            key, submitted_at = entry
            completes[key] += 1
            statuses[key][pkt["status"]] += 1
            latencies[key].append(pkt["ts"] - submitted_at)
            moved[key].append(pkt["length"])

    if not total:
        raise SystemExit("no control transfers in this capture")

    print("control transfers: %d, across device numbers: %s" % (
        total, ", ".join("%d (%d pkts)" % (d, n) for d, n in sorted(devices.items()))))
    print("(a device number changes on every re-enumeration -- checkm8 resets "
          "the device several times, so several here is expected)\n")

    for key in sorted(submits, key=lambda k: -submits[k]):
        sub, comp = submits[key], completes.get(key, 0)
        print(label(key))
        print("    submitted: %d   completed: %d%s" % (
            sub, comp, "   *** %d NEVER COMPLETED ***" % (sub - comp) if sub != comp else ""))
        for st, n in statuses[key].most_common():
            print("      status %-5d x%-6d %s" % (st, n, STATUS_NAMES.get(st, "")))
        lat = sorted(latencies[key])
        if lat:
            # How long the URB was actually alive on the host. A value at or
            # just past the caller's own timeout means it lived its full
            # allotted time and was then cancelled; a near-zero one means it
            # was rejected or torn down immediately and never had a chance to
            # reach the device at all.
            print("      alive on host (us): min %d  median %d  max %d" % (
                lat[0], lat[len(lat) // 2], lat[-1]))
        byts = moved[key]
        if byts:
            print("      bytes transferred: min %d  max %d  (%d of %d moved nothing)" % (
                min(byts), max(byts), sum(1 for b in byts if b == 0), len(byts)))
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())
