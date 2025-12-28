"""
Simple auto-reconnecting serial monitor for Windows.

Usage:
  pip install pyserial
  python tools/serial_monitor.py COM8 115200
Press Ctrl+C to exit.
"""

import sys
import time
from typing import Optional

try:
    import serial  # type: ignore
except ImportError:
    print("pyserial is required. Install with: pip install pyserial")
    sys.exit(1)


def monitor(port: str, baud: int) -> None:
    while True:
        try:
            with serial.Serial(port, baud, timeout=1) as ser:
                print(f"[serial] connected to {port} @ {baud}")
                while True:
                    data = ser.readline()
                    if data:
                        try:
                            print(data.decode("utf-8", errors="replace"), end="")
                        except Exception:
                            # Fallback raw bytes if decoding fails
                            print(data)
        except serial.SerialException as e:
            print(f"[serial] disconnected: {e}")
            time.sleep(0.5)
        except KeyboardInterrupt:
            print("\n[serial] stopped by user")
            return


def parse_int(s: str, default: int) -> int:
    try:
        return int(s)
    except Exception:
        return default


def main(argv: list[str]) -> None:
    if len(argv) < 2:
        print("Usage: python tools/serial_monitor.py <COM-port> [baud]")
        print("Example: python tools/serial_monitor.py COM8 115200")
        return
    port = argv[1]
    baud = parse_int(argv[2], 115200) if len(argv) > 2 else 115200
    monitor(port, baud)


if __name__ == "__main__":
    main(sys.argv)
