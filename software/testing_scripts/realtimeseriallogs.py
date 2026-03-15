#!/usr/bin/env python3
"""
Read Arduino CSV telemetry from serial, plot each measurement on its own graph
in real time, and save all rows to a CSV file.

- Ignores lines starting with '#'
- By default, uses a known header (can be overridden with --header)
- Skips the device header line if it matches

Usage:
  python plot_serial_csv.py --port /dev/ttyACM0 --out /tmp/log.csv --write-header
  python plot_serial_csv.py --port COM5 --out "C:\\logs\\log.csv" --baud 115200 --write-header
"""

from __future__ import annotations

import argparse
import csv
import os
import sys
import time
from collections import deque
from typing import Dict, List, Optional

import serial  # pip install pyserial
import matplotlib.pyplot as plt

DEFAULT_HEADER = (
    "ms,temp_c,hum_pct,press_hpa,gas_ohms,alt_m,"
    "vbat_v,vsolar_v,als_lux,white,"
    "sgp41_sraw_voc,sgp41_sraw_nox,sgp41_voc_index,sgp41_nox_index"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--port", required=True, help="Serial port (e.g., COM5 or /dev/ttyACM0)")
    p.add_argument("--out", required=True, help="Output CSV file path")
    p.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    p.add_argument("--window", type=float, default=120.0, help="Plot window length in seconds (default: 120)")
    p.add_argument("--max-points", type=int, default=5000, help="Max buffered points (default: 5000)")
    p.add_argument("--update-ms", type=int, default=200, help="Plot update interval in ms (default: 200)")
    p.add_argument(
        "--header",
        default=DEFAULT_HEADER,
        help=f"CSV header as a comma-separated string (default: {DEFAULT_HEADER})",
    )
    p.add_argument("--write-header", action="store_true", help="Write CSV header to output file (default: off)")
    return p.parse_args()


def ensure_parent_dir(path: str) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    if parent and not os.path.exists(parent):
        os.makedirs(parent, exist_ok=True)


def parse_header(header_str: str) -> List[str]:
    cols = [c.strip() for c in header_str.split(",") if c.strip() != ""]
    if not cols or cols[0] != "ms":
        raise ValueError("Header must start with 'ms'")
    return cols


def try_float(s: str) -> Optional[float]:
    s = s.strip()
    if s == "":
        return None
    try:
        return float(s)
    except ValueError:
        return None


class Plotter:
    def __init__(self, header: List[str], window_s: float, max_points: int):
        self.header = header
        self.window_s = float(window_s)
        self.t = deque(maxlen=max_points)  # seconds
        self.series: Dict[str, deque] = {c: deque(maxlen=max_points) for c in self.header if c != "ms"}

        self.fig = None
        self.axes = []
        self.lines = {}

        self._init_plots()

    def _init_plots(self) -> None:
        cols = [c for c in self.header if c != "ms"]
        n = len(cols)

        self.fig, axs = plt.subplots(n, 1, sharex=True, figsize=(12, max(6, 1.7 * n)))
        if n == 1:
            axs = [axs]
        self.axes = list(axs)

        self.fig.suptitle("Real-time telemetry (each measurement in its own graph)")
        for ax, col in zip(self.axes, cols):
            ax.grid(True, alpha=0.25)
            ax.set_ylabel(col)
            (ln,) = ax.plot([], [], linewidth=1.2)
            self.lines[col] = ln

        self.axes[-1].set_xlabel("Seconds since device boot (ms/1000)")
        plt.tight_layout(rect=[0, 0, 1, 0.97])

        plt.ion()
        plt.show()

    def push(self, row: Dict[str, str]) -> None:
        ms_val = try_float(row.get("ms", ""))
        if ms_val is None:
            return
        t_s = ms_val / 1000.0
        self.t.append(t_s)

        for col, dq in self.series.items():
            dq.append(try_float(row.get(col, "")))

    def update(self) -> None:
        if not self.t:
            return

        t_latest = self.t[-1]
        t_min = max(0.0, t_latest - self.window_s)
        t_list = list(self.t)

        for col, ln in self.lines.items():
            y_raw = self.series[col]
            y_list = [float("nan") if v is None else v for v in y_raw]
            ln.set_data(t_list, y_list)

        for ax in self.axes:
            ax.set_xlim(t_min, t_latest if t_latest > t_min else t_min + 1e-6)
            ax.relim()
            ax.autoscale_view(scalex=False, scaley=True)

        self.fig.canvas.draw_idle()
        self.fig.canvas.flush_events()


def main() -> int:
    args = parse_args()

    try:
        header = parse_header(args.header)
    except ValueError as e:
        print(f"Invalid --header: {e}", file=sys.stderr)
        return 2

    ensure_parent_dir(args.out)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
        ser.reset_input_buffer()
    except Exception as e:
        print(f"Failed to open serial port {args.port}: {e}", file=sys.stderr)
        return 2

    fh = open(args.out, "w", newline="", encoding="utf-8")
    writer = csv.DictWriter(fh, fieldnames=header)
    if args.write_header:
        writer.writeheader()
        fh.flush()

    plotter = Plotter(header, args.window, args.max_points)

    expected_header_line = ",".join(header)

    print(f"Reading from {args.port} @ {args.baud} baud")
    print(f"Saving CSV to: {args.out}")
    print("Header:")
    print(expected_header_line)

    last_flush = time.monotonic()

    try:
        while True:
            # Exit if window closed
            if plotter.fig is not None and not plt.fignum_exists(plotter.fig.number):
                break

            raw = ser.readline()
            if not raw:
                time.sleep(args.update_ms / 1000.0)
                plotter.update()
                continue

            line = raw.decode("utf-8", errors="replace").strip()
            if not line or line.startswith("#"):
                continue

            # If device prints a header, skip it if it matches our expected header
            if line.replace(" ", "") == expected_header_line:
                continue

            parts = [p.strip() for p in line.split(",")]
            if len(parts) != len(header):
                continue

            row = dict(zip(header, parts))
            plotter.push(row)
            writer.writerow(row)

            now = time.monotonic()
            if now - last_flush > 0.5:
                fh.flush()
                last_flush = now

            plotter.update()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            ser.close()
        except Exception:
            pass
        try:
            fh.flush()
            fh.close()
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
