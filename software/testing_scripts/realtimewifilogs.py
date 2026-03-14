#!/usr/bin/env python3
"""
Poll Pico 2W measurements over WiFi (HTTP GET /metrics), plot each measurement
in its own graph in real time, and save rows to a CSV file.

The device should serve JSON at:
  http://<ip>/metrics

Usage:
  python test_http_plot_save.py --ip 192.168.1.50 --out /tmp/pico_http.csv --write-header
  python test_http_plot_save.py --ip 192.168.1.50 --port 80 --path /metrics --out pico.csv

Deps:
  pip install requests matplotlib
"""

from __future__ import annotations

import argparse
import csv
import os
import time
from collections import deque
from typing import Dict, List, Optional

import requests
import matplotlib.pyplot as plt

DEFAULT_HEADER = (
    "ms,temp_c,hum_pct,press_hpa,gas_ohms,alt_m,"
    "vbat_v,vsolar_v,als_lux,white,"
    "sgp41_sraw_voc,sgp41_sraw_nox,sgp41_voc_index,sgp41_nox_index,"
    "sgp41_ok"
)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser()
    p.add_argument("--ip", required=True, help="Device IP address (e.g., 192.168.1.50)")
    p.add_argument("--port", type=int, default=80, help="HTTP port (default: 80)")
    p.add_argument("--path", default="/metrics", help="HTTP path (default: /metrics)")
    p.add_argument("--out", required=True, help="Output CSV file path")
    p.add_argument("--interval", type=float, default=1.0, help="Poll interval in seconds (default: 1.0)")
    p.add_argument("--timeout", type=float, default=2.0, help="HTTP timeout seconds (default: 2.0)")
    p.add_argument("--window", type=float, default=120.0, help="Plot window length in seconds (default: 120)")
    p.add_argument("--max-points", type=int, default=5000, help="Max buffered points (default: 5000)")
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


def try_float(v) -> Optional[float]:
    if v is None:
        return None
    if isinstance(v, bool):
        return 1.0 if v else 0.0
    try:
        return float(v)
    except Exception:
        return None


class Plotter:
    def __init__(self, header: List[str], window_s: float, max_points: int):
        self.header = header
        self.window_s = float(window_s)
        self.t = deque(maxlen=max_points)  # seconds since device boot
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

        self.fig.suptitle("Real-time telemetry over HTTP (each measurement in its own graph)")
        for ax, col in zip(self.axes, cols):
            ax.grid(True, alpha=0.25)
            ax.set_ylabel(col)
            (ln,) = ax.plot([], [], linewidth=1.2)
            self.lines[col] = ln

        self.axes[-1].set_xlabel("Seconds since device boot (ms/1000)")
        plt.tight_layout(rect=[0, 0, 1, 0.97])

        plt.ion()
        plt.show()

    def push_json(self, js: Dict) -> None:
        ms_val = try_float(js.get("ms"))
        if ms_val is None:
            return
        t_s = ms_val / 1000.0
        self.t.append(t_s)

        for col, dq in self.series.items():
            dq.append(try_float(js.get(col)))

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
    header = parse_header(args.header)

    ensure_parent_dir(args.out)
    fh = open(args.out, "w", newline="", encoding="utf-8")
    writer = csv.DictWriter(fh, fieldnames=header)
    if args.write_header:
        writer.writeheader()
        fh.flush()

    url = f"http://{args.ip}:{args.port}{args.path}"
    plotter = Plotter(header, args.window, args.max_points)

    sess = requests.Session()

    print(f"Polling: {url}")
    print(f"Saving CSV to: {args.out}")
    print("Close the plot window to stop.")

    last_flush = time.monotonic()
    next_poll = time.monotonic()

    try:
        while True:
            if plotter.fig is not None and not plt.fignum_exists(plotter.fig.number):
                break

            now = time.monotonic()
            if now < next_poll:
                time.sleep(min(0.05, next_poll - now))
                plotter.update()
                continue

            next_poll = now + float(args.interval)

            try:
                r = sess.get(url, timeout=args.timeout)
                r.raise_for_status()
                js = r.json()
            except Exception:
                # still keep UI responsive
                plotter.update()
                continue

            # Save row in header order, filling missing keys with ""
            row = {k: js.get(k, "") for k in header}
            writer.writerow(row)

            plotter.push_json(js)
            plotter.update()

            if time.monotonic() - last_flush > 0.5:
                fh.flush()
                last_flush = time.monotonic()

    except KeyboardInterrupt:
        pass
    finally:
        try:
            fh.flush()
            fh.close()
        except Exception:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
