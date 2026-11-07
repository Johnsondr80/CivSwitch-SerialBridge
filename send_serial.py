#!/usr/bin/env python3
"""Send a JS payload to the Switch via the ESP32 bridge over USB serial.

Usage:
  python3 send_serial.py <switch-ip> payload.js
  python3 send_serial.py <switch-ip> <port> payload.js
  python3 send_serial.py --port /dev/cu.usbserial-0001 <switch-ip> payload.js
  python3 send_serial.py --port COM3 <switch-ip> payload.js

Requires: pip install pyserial
"""
import argparse
import glob
import platform
import sys
import time

import serial


def find_esp32_port():
    system = platform.system()
    if system == "Darwin":
        candidates = (
            glob.glob("/dev/cu.usbserial-*") +
            glob.glob("/dev/cu.wchusbserial-*") +
            glob.glob("/dev/cu.SLAB_USBtoUART*") +
            glob.glob("/dev/cu.usbmodem*")
        )
    elif system == "Linux":
        candidates = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
    elif system == "Windows":
        # Can't glob COM ports, user needs to specify --port
        return None
    else:
        return None

    if len(candidates) == 1:
        return candidates[0]
    preferred = [c for c in candidates
                 if "usbserial" in c or "SLAB" in c
                 or "wchusbserial" in c or "ttyUSB" in c]
    if preferred:
        return preferred[0]
    if candidates:
        return candidates[0]
    return None


def send_via_serial(serial_port, switch_ip, switch_port, js_code):
    ser = serial.Serial(serial_port, 115200, timeout=1)
    time.sleep(0.5)
    ser.reset_input_buffer()

    payload = js_code.encode("utf-8")
    cmd = f"SEND {switch_ip} {switch_port} {len(payload)}\n"
    ser.write(cmd.encode())
    time.sleep(0.1)
    ser.write(payload)
    ser.flush()

    output = []
    deadline = time.time() + 130
    while time.time() < deadline:
        line = ser.readline().decode("utf-8", "replace").rstrip("\r\n")
        if line:
            print(line)
            output.append(line)
            if line.strip() == "END":
                break
            deadline = time.time() + 10
    ser.close()
    return "\n".join(output)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="ESP32 serial port (auto-detected on Mac/Linux)")
    ap.add_argument("switch_ip", help="Switch IP on the bridge network (usually 192.168.4.2)")
    ap.add_argument("args", nargs="+", help="[tcp_port] payload.js")
    args = ap.parse_args()

    serial_port = args.port or find_esp32_port()
    if not serial_port:
        print("ERROR: Can't find the ESP32. Use --port to specify it.")
        print("  Mac:     --port /dev/cu.usbserial-XXXX")
        print("  Linux:   --port /dev/ttyUSB0")
        print("  Windows: --port COM3")
        sys.exit(1)

    tcp_port = 9026
    rest = args.args
    if rest[0].isdigit():
        tcp_port = int(rest[0])
        rest = rest[1:]
    if not rest:
        print("ERROR: No payload file specified")
        sys.exit(1)

    js_code = open(rest[0], encoding="utf-8").read()

    print(f"Sending {len(js_code)} bytes via {serial_port} -> {args.switch_ip}:{tcp_port}")
    send_via_serial(serial_port, args.switch_ip, tcp_port, js_code)


if __name__ == "__main__":
    main()
