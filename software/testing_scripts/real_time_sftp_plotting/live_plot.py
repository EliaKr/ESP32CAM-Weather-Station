#!/usr/bin/env python3
"""
Live multi-axis plot from Raspberry Pi CSV over SSH.

- Config from config.yaml (--config)
- Adjustable SSH port (ssh.port)
- Refresh every N seconds
- GUI timeframe selector (RadioButtons)
- Plots each numeric column in its own subplot (shared x-axis)
- Shows latest values (all columns) on the right

Deps:
  pip install paramiko pandas matplotlib pyyaml
"""

from __future__ import annotations

import argparse
import datetime as dt
import io
import os
import re
from dataclasses import dataclass
from typing import Optional

import pandas as pd
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation
from matplotlib.widgets import RadioButtons

import paramiko
import yaml


def parse_tf(s: str) -> dt.timedelta:
    s = s.strip().lower()
    m = re.fullmatch(r"(\d+)\s*([mhd])", s)
    if not m:
        raise ValueError(f"Bad timeframe '{s}' (use e.g. 15m, 2h, 1d)")
    n = int(m.group(1))
    u = m.group(2)
    if u == "m":
        return dt.timedelta(minutes=n)
    if u == "h":
        return dt.timedelta(hours=n)
    if u == "d":
        return dt.timedelta(days=n)
    raise ValueError(f"Bad timeframe '{s}'")


def load_yaml(path: str) -> dict:
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def deep_get(d: dict, path: list[str], default=None):
    cur = d
    for p in path:
        if not isinstance(cur, dict) or p not in cur:
            return default
        cur = cur[p]
    return cur


@dataclass
class SSHCfg:
    host: str
    port: int
    username: str
    password: Optional[str]
    key_filename: Optional[str]


class SSH:
    def __init__(self, cfg: SSHCfg):
        self.cfg = cfg
        self.client = None
        self.sftp = None

    def connect(self):
        if self.client:
            return
        c = paramiko.SSHClient()
        c.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        kwargs = dict(
            hostname=self.cfg.host,
            port=self.cfg.port,
            username=self.cfg.username,
            timeout=10,
            banner_timeout=10,
            auth_timeout=10,
            look_for_keys=True,
            allow_agent=True,
        )
        if self.cfg.password:
            kwargs["password"] = self.cfg.password
        if self.cfg.key_filename:
            kwargs["key_filename"] = self.cfg.key_filename

        c.connect(**kwargs)
        self.client = c
        self.sftp = c.open_sftp()

    def read_text(self, path: str) -> str:
        self.connect()
        with self.sftp.open(path, "r") as f:
            data = f.read()
        if isinstance(data, bytes):
            return data.decode("utf-8", errors="replace")
        return data

    def close(self):
        try:
            if self.sftp:
                self.sftp.close()
        finally:
            self.sftp = None
        try:
            if self.client:
                self.client.close()
        finally:
            self.client = None


def load_df(csv_text: str) -> pd.DataFrame:
    df = pd.read_csv(io.StringIO(csv_text))
    df["server_ts_utc"] = pd.to_datetime(df["server_ts_utc"], utc=True, errors="coerce")
    df = df.dropna(subset=["server_ts_utc"]).sort_values("server_ts_utc")
    return df


def numeric_columns(df: pd.DataFrame) -> list[str]:
    exclude = {"server_ts_utc", "device_id"}
    cols: list[str] = []
    for c in df.columns:
        if c in exclude:
            continue
        if pd.api.types.is_bool_dtype(df[c]):
            continue
        if pd.api.types.is_numeric_dtype(df[c]):
            cols.append(c)
    return cols


def latest_values_text(row: pd.Series) -> str:
    ts = row.get("server_ts_utc")
    lines: list[str] = []
    if pd.notna(ts):
        ts_utc = pd.to_datetime(ts, utc=True)
        ts_local = ts_utc.tz_convert(None)
        lines.append(f"Latest (UTC):   {ts_utc.strftime('%Y-%m-%d %H:%M:%S')}")
        lines.append(f"Latest (Local): {ts_local.strftime('%Y-%m-%d %H:%M:%S')}")
    if "device_id" in row.index:
        lines.append(f"Device: {row.get('device_id')}")
    lines.append("")

    for c in row.index:
        if c == "server_ts_utc":
            continue
        v = row[c]
        if pd.isna(v):
            lines.append(f"{c}: (na)")
        elif isinstance(v, (float, int)):
            lines.append(f"{c}: {v:.3f}".rstrip("0").rstrip("."))
        else:
            lines.append(f"{c}: {v}")
    return "\n".join(lines)


def make_layout(n: int):
    """
    Create a grid with up to 3 columns, enough rows for n plots.
    Returns (fig, axes_2d, plot_axes_list, right_panel_axis)
    """
    cols = min(3, max(1, n))
    rows = (n + cols - 1) // cols

    fig = plt.figure(figsize=(18, 9))
    # left: grid of plots, right: latest values + timeframe control
    gs = fig.add_gridspec(
        nrows=rows,
        ncols=cols + 1,
        width_ratios=[1] * cols + [0.95],
        left=0.05,
        right=0.98,
        top=0.93,
        bottom=0.08,
        wspace=0.25,
        hspace=0.35,
    )

    plot_axes = []
    for r in range(rows):
        for c in range(cols):
            idx = r * cols + c
            if idx >= n:
                break
            ax = fig.add_subplot(gs[r, c])
            plot_axes.append(ax)

    right_ax = fig.add_subplot(gs[:, cols])
    right_ax.axis("off")
    return fig, plot_axes, right_ax, rows, cols


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--config", default="config.yaml")
    args = ap.parse_args()

    cfg = load_yaml(args.config)

    password = (deep_get(cfg, ["ssh", "password"], "") or "").strip() or os.getenv("WX_PI_PASSWORD", "").strip() or None
    key_filename = (deep_get(cfg, ["ssh", "key_filename"], "") or "").strip() or None

    ssh_cfg = SSHCfg(
        host=str(deep_get(cfg, ["ssh", "host"], "192.168.2.60")),
        port=int(deep_get(cfg, ["ssh", "port"], 22)),
        username=str(deep_get(cfg, ["ssh", "username"], "pi")),
        password=password,
        key_filename=key_filename,
    )
    remote_csv = str(deep_get(cfg, ["remote", "csv"], "/home/pi/wx_server_deployment/processing/server_log.csv"))

    refresh_s = int(deep_get(cfg, ["ui", "refresh_seconds"], 10))
    tf_default = str(deep_get(cfg, ["ui", "timeframe_default"], "2h")).strip().lower()
    tfs = deep_get(cfg, ["ui", "timeframes"], None) or ["30m", "1h", "2h", "6h", "1d"]

    tfs = [str(x).strip().lower() for x in tfs]
    if tf_default not in tfs:
        tfs.insert(0, tf_default)

    ssh = SSH(ssh_cfg)

    plt.style.use("seaborn-v0_8-darkgrid")

    # Determine numeric columns once (initial fetch) so the layout stays stable
    try:
        init_df = load_df(ssh.read_text(remote_csv))
    except Exception as e:
        print(f"Failed initial read: {type(e).__name__}: {e}")
        return 2

    cols = numeric_columns(init_df)
    if not cols:
        print("No numeric columns found to plot.")
        return 2

    fig, plot_axes, right_ax, rows, grid_cols = make_layout(len(cols))

    # timeframe selector placed on right panel using an extra axes
    # Place it near top of figure on the right side.
    rb_ax = fig.add_axes([0.78, 0.70, 0.20, 0.22])
    rb_ax.set_title("Timeframe", fontsize=10, pad=6)
    rb = RadioButtons(rb_ax, tfs, active=tfs.index(tf_default), activecolor="tab:blue")

    status = right_ax.text(0.02, 0.65, "Starting…", va="top", ha="left", family="monospace")

    state = {"tf": tf_default, "td": parse_tf(tf_default)}

    def on_tf(label: str):
        state["tf"] = label.strip().lower()
        state["td"] = parse_tf(state["tf"])
        fig.canvas.draw_idle()

    rb.on_clicked(on_tf)

    def update(_i):
        status.set_text(
            "Connecting / fetching...\n\n"
            f"Host: {ssh_cfg.host}:{ssh_cfg.port}\n"
            f"User: {ssh_cfg.username}\n"
            f"File: {remote_csv}\n"
            f"TF:   {state['tf']}\n"
        )
        fig.canvas.draw_idle()

        try:
            text = ssh.read_text(remote_csv)
            df = load_df(text)
            if df.empty:
                status.set_text("No data.")
                return

            end = df["server_ts_utc"].max()
            dfw = df[df["server_ts_utc"] >= (end - state["td"])].copy()
            if dfw.empty:
                status.set_text("No data in selected timeframe.")
                return

            # redraw each subplot
            for ax_i, col in zip(plot_axes, cols):
                ax_i.clear()
                if col not in dfw.columns:
                    ax_i.set_title(col)
                    ax_i.text(0.5, 0.5, "missing", ha="center", va="center", transform=ax_i.transAxes)
                    continue

                ax_i.plot(dfw["server_ts_utc"], dfw[col], linewidth=1.3)
                ax_i.set_title(col, fontsize=10)
                ax_i.grid(True, alpha=0.35)

                # only label x on bottom row
                # (rough heuristic based on axes position in list)
                # keep it simple: hide x tick labels for all but last row
                # (works well enough visually)
                # We'll just rotate and let tight_layout handle it; can be refined later.

            fig.autofmt_xdate()

            # overall title
            fig.suptitle(
                f"Live: {os.path.basename(remote_csv)}  (window={state['tf']}, refresh={refresh_s}s)",
                fontsize=12,
            )

            status.set_text(latest_values_text(dfw.iloc[-1]))

        except Exception as e:
            status.set_text(f"ERROR (will retry)\n\n{type(e).__name__}: {e}")

    anim = FuncAnimation(fig, update, interval=max(1, refresh_s) * 1000, cache_frame_data=False)

    def on_close(_evt):
        ssh.close()

    fig.canvas.mpl_connect("close_event", on_close)

    plt.show()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
