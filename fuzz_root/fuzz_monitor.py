#!/usr/bin/env python3
"""
Hourly fuzzing cluster monitor.

Every hour (configurable via -i), reads plot_data from every fuzzer
instance launched by run_parallel.py, and saves two PNG charts per
instance under <output_dir>/monitor/:

    instance_<id>_crashes.png    -- unique_crashes over time
    instance_<id>_edge_cov.png   -- bitmap (edge) coverage over time

Usage (inside Docker, from fuzz_root/):
    python3 fuzz_monitor.py                  # default output dir ./outputs
    python3 fuzz_monitor.py -d /path/to/out  # custom output dir
    python3 fuzz_monitor.py -i 1800          # every 30 min
    python3 fuzz_monitor.py -1               # run once then exit

Dependencies: matplotlib  (pip3 install matplotlib)
              schedule    (pip3 install schedule)
"""

import argparse
import os
import sys
import time
from datetime import datetime

import schedule

import matplotlib
matplotlib.use("Agg")  # headless backend, no display needed
import matplotlib.pyplot as plt
import matplotlib.dates as mdates

from afl_config import starting_core_id, parallel_num

# ── plot_data CSV column indices (from afl-fuzz.cpp) ────────────
#  0  unix_time
#  1  cycles_done
#  2  cur_path
#  3  paths_total
#  4  pending_total
#  5  pending_favs
#  6  map_size            (e.g. "2.35%")
#  7  unique_crashes
#  8  unique_hangs
#  9  max_depth
# 10  execs_per_sec
# 11  total_execs
# ...
# 35  num_grammar_edge_cov
# 36  num_grammar_path_cov
COL_TIME           = 0
COL_MAP_SIZE       = 6
COL_UNIQUE_CRASHES = 7
COL_TOTAL_EXECS    = 11


def parse_plot_data(path):
    """Parse a plot_data CSV file.

    Returns (timestamps, crashes, edge_cov) where each is a list of
    floats.  timestamps are epoch seconds, edge_cov are percentages.
    Returns None if the file is missing or empty.
    """
    timestamps = []
    crashes    = []
    edge_cov   = []

    try:
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("unix_time"):
                    continue  # skip header
                cols = line.split(",")
                if len(cols) < COL_TOTAL_EXECS + 1:
                    continue  # malformed row

                try:
                    t = float(cols[COL_TIME])
                    c = int(cols[COL_UNIQUE_CRASHES])
                    # map_size has a trailing '%', e.g. "2.35%"
                    m = float(cols[COL_MAP_SIZE].replace("%", ""))
                except (ValueError, IndexError):
                    continue

                timestamps.append(t)
                crashes.append(c)
                edge_cov.append(m)
    except (IOError, OSError):
        return None

    if not timestamps:
        return None
    return timestamps, crashes, edge_cov


def epoch_to_hours(timestamps):
    """Convert absolute epoch list to relative hours from first entry."""
    t0 = timestamps[0]
    return [(t - t0) / 3600.0 for t in timestamps]


def save_chart(x, y, title, xlabel, ylabel, out_path):
    """Save a single line chart to PNG."""
    fig, ax = plt.subplots(figsize=(10, 5))
    ax.plot(x, y, linewidth=1.5)
    ax.set_title(title, fontsize=14)
    ax.set_xlabel(xlabel, fontsize=12)
    ax.set_ylabel(ylabel, fontsize=12)
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_path, dpi=120)
    plt.close(fig)


def generate_charts(output_base, monitor_dir):
    """Read plot_data from all instances and generate per-instance charts."""
    os.makedirs(monitor_dir, exist_ok=True)
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    generated = 0

    for i in range(parallel_num):
        inst_id = starting_core_id + i
        plot_path = os.path.join(output_base, "outputs_" + str(i), "plot_data")
        result = parse_plot_data(plot_path)

        if result is None:
            print("[%s] Instance %d: plot_data not found or empty (%s)"
                  % (now, inst_id, plot_path))
            continue

        timestamps, crashes, edge_cov = result
        hours = epoch_to_hours(timestamps)

        # crashes over time
        crash_png = os.path.join(
            monitor_dir, "instance_%d_crashes.png" % inst_id)
        save_chart(
            hours, crashes,
            title="Instance %d  --  Unique Crashes" % inst_id,
            xlabel="Time (hours)",
            ylabel="Unique Crashes",
            out_path=crash_png,
        )

        # edge coverage over time
        cov_png = os.path.join(
            monitor_dir, "instance_%d_edge_cov.png" % inst_id)
        save_chart(
            hours, edge_cov,
            title="Instance %d  --  Edge Coverage" % inst_id,
            xlabel="Time (hours)",
            ylabel="Edge Coverage (%)",
            out_path=cov_png,
        )

        print("[%s] Instance %d: %d data points  ->  %s, %s"
              % (now, inst_id, len(timestamps),
                 os.path.basename(crash_png),
                 os.path.basename(cov_png)))
        generated += 1

    return generated


def print_summary(output_base):
    """Print a short human-readable summary to stdout."""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print("=" * 70)
    print("  Fuzz Monitor  |  %s" % now)
    print("=" * 70)

    alive = 0
    for i in range(parallel_num):
        inst_id = starting_core_id + i
        plot_path = os.path.join(output_base, "outputs_" + str(i), "plot_data")
        result = parse_plot_data(plot_path)

        if result is None:
            print("  [%d]  DOWN / no data" % inst_id)
            continue

        alive += 1
        timestamps, crashes, edge_cov = result
        hours_elapsed = (timestamps[-1] - timestamps[0]) / 3600.0
        print("  [%d]  %.1f hrs  crashes %d  edge_cov %.2f%%  (%d points)"
              % (inst_id, hours_elapsed, crashes[-1], edge_cov[-1],
                 len(timestamps)))

    print("-" * 70)
    print("  alive: %d / %d instances" % (alive, parallel_num))
    print("=" * 70)
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Hourly fuzz cluster monitor with per-instance charts")
    parser.add_argument(
        "-d", "--output-dir", default="./outputs",
        help="Base output directory (default: ./outputs)")
    parser.add_argument(
        "-i", "--interval", type=int, default=3600,
        help="Collection interval in seconds (default: 3600)")
    parser.add_argument(
        "-1", "--once", action="store_true",
        help="Run once then exit (no loop)")
    args = parser.parse_args()

    output_base = os.path.abspath(args.output_dir)
    monitor_dir = os.path.join(output_base, "monitor")

    print("Fuzz monitor started")
    print("  instances  : %d (core %d..%d)"
          % (parallel_num, starting_core_id,
             starting_core_id + parallel_num - 1))
    print("  output dir : %s" % output_base)
    print("  charts dir : %s" % monitor_dir)
    print("  interval   : %d s" % args.interval)
    print()

    def monitor_job():
        """Single monitoring pass: summary + charts."""
        print_summary(output_base)
        n = generate_charts(output_base, monitor_dir)
        print("  -> %d/%d instances charted\n" % (n, parallel_num))

    # Always run the job once immediately.
    monitor_job()

    if args.once:
        return

    # Schedule recurring runs at the requested interval.
    schedule.every(args.interval).seconds.do(monitor_job)

    try:
        while True:
            schedule.run_pending()
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nMonitor stopped by user.")


if __name__ == "__main__":
    main()
