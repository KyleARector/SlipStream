#!/usr/bin/env python3
"""Standalone test: send raw ESC/POS bytes to the TM-H2000 directly from
a host computer over USB, bypassing the ESP32 entirely.

Confirms the printer itself and the ESC/POS byte format work, decoupled
from anything on the ESP32 side (useful for isolating USB host bring-up
issues -- see M6 in the spec).

One-time setup:
    brew install libusb
    pip3 install pyusb

Usage:
    python3 test_printer.py                          # sends a default test line
    python3 test_printer.py "Hey, print this!"        # sends your own message
    python3 test_printer.py --cut                     # also attempts a full cut after
    python3 test_printer.py --cut --partial           # partial cut instead of full
"""

import argparse
import sys

import usb.core
import usb.util

VENDOR_ID = 0x04B8   # EPSON
PRODUCT_ID = 0x0202  # TM-H2000

ESC = 0x1B
GS = 0x1D


def escpos_text_frame(text: str, extra_feed_lines: int = 5) -> bytes:
    """Same shape as escpos_formatter's output (init + text + line feed),
    plus extra trailing feed lines so the print is visibly pushed out past
    the tear bar for this manual visual test -- the firmware's actual
    escpos_format() intentionally emits only one trailing line feed, this
    standalone script just adds more so a human can see the result."""
    return bytes([ESC, 0x40]) + text.encode("ascii") + b"\n" + (b"\n" * extra_feed_lines)


def escpos_cut(partial: bool = False) -> bytes:
    """GS V m -- m=0 full cut, m=1 partial cut.

    Whether a given unit has a cutter installed isn't guaranteed. Sending
    this to a printer with no cutter is typically a harmless no-op, but
    that's not guaranteed for every model -- watch the printer, don't
    walk away during this test.
    """
    return bytes([GS, 0x56, 0x01 if partial else 0x00])


def find_bulk_out_endpoint(dev):
    cfg = dev.get_active_configuration()
    for intf in cfg:
        ep_out = usb.util.find_descriptor(
            intf,
            custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress)
            == usb.util.ENDPOINT_OUT,
        )
        if ep_out is not None:
            return ep_out
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("message", nargs="?", default="SlipStream test print - hello!",
                         help="text to print (defaults to a canned test line)")
    parser.add_argument("--cut", action="store_true", help="also send a cut command after the test line")
    parser.add_argument("--partial", action="store_true", help="use partial cut instead of full cut (with --cut)")
    args = parser.parse_args()

    dev = usb.core.find(idVendor=VENDOR_ID, idProduct=PRODUCT_ID)
    if dev is None:
        sys.exit(f"Printer not found (VID=0x{VENDOR_ID:04X} PID=0x{PRODUCT_ID:04X}). Is it plugged in and powered?")

    try:
        dev.set_configuration()
    except usb.core.USBError as exc:
        print(f"Note: set_configuration() reported: {exc} (often harmless if already configured)")

    ep_out = find_bulk_out_endpoint(dev)
    if ep_out is None:
        sys.exit("Could not find a bulk OUT endpoint on this device.")

    try:
        frame = escpos_text_frame(args.message)
    except UnicodeEncodeError:
        sys.exit("Message contains characters this printer's default codepage can't handle -- try plain ASCII.")

    print(f"Found bulk OUT endpoint 0x{ep_out.bEndpointAddress:02X}, sending: {args.message!r}")
    ep_out.write(frame)
    print("Sent. Check the printer.")

    if args.cut:
        print(f"Sending {'partial' if args.partial else 'full'} cut command...")
        ep_out.write(escpos_cut(partial=args.partial))
        print("Cut command sent. Watch the printer to see what happens.")


if __name__ == "__main__":
    main()
