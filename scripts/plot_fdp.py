import os, re, glob
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = "../fdp/results"
EFF_RE = re.compile(r"EFFECTIVE D-Cache Misses:\s*(\d+)")
ISSUED_RE = re.compile(r"Stride Prefetches Issued:\s*(\d+)")
PFHIT_RE = re.compile(r"Prefetch Buffer Hits:\s*(\d+)")
ADJ_RE = re.compile(r"FDP Feedback Adjustments:\s*(\d+)")
ACC_RE = re.compile(r"Prefetch Accuracy:\s*([\d.]+)%")
DCMISS_RE = re.compile(r"D-Cache Miss:\s*(\d+)")

def parse(fp):
    out = {}
    with open(fp) as f: text = f.read()
    for k, r in [("eff", EFF_RE), ("issued", ISSUED_RE), ("pfhit", PFHIT_RE), ("adj", ADJ_RE), ("dmiss", DCMISS_RE)]:
        m = r.search(text)
        out[k] = int(m.group(1)) if m else 0
    m = ACC_RE.search(text)
    out["accuracy"] = float(m.group(1)) if m else 0.0
    return out

def collect():
    data = defaultdict(dict)
    for bd in sorted(glob.glob(os.path.join(RESULTS_DIR, "*"))):
        bench = os.path.basename(bd)
        for fp in sorted(glob.glob(os.path.join(bd, "*.out"))):
            label = os.path.basename(fp)[:-4]
            data[bench][label] = parse(fp)
    return data

def plot_main_comparison(data):
    benches = sorted(data.keys())
    modes = [
        ("mode0_no_prefetch", "No prefetch"),
        ("mode1_fixed_stride","Fixed stride"),
        ("mode2_global_w100000", "Global FDP"),
        ("mode3_per_pc_w100000", "Per-PC FDP"),
    ]
    for bench in benches:
        runs = data[bench]
        labels = [m[1] for m in modes]
        eff = [float(runs[m[0]]["eff"]) if m[0] in runs else 0 for m in modes]
        plt.figure(figsize=(8, 4.5))
        colors = ["#888888", "#4477AA", "#DDAA33", "#BB5566"]
        bars = plt.bar(labels, eff, color=colors)
        plt.ylabel("Effective L1D misses")
        plt.title(f"FDP comparison: {bench}")
        plt.grid(axis="y", alpha=0.3)
        for bar, v in zip(bars, eff):
            plt.text(bar.get_x() + bar.get_width()/2, v, f"{v/1e6:.2f}M" if v > 1e6 else f"{v/1e3:.0f}K", ha="center", va="bottom", fontsize=10)
        plt.tight_layout()
        plt.savefig(f"fdp_comparison_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote fdp_comparison_{bench}.png")

def plot_window_sweep(data):
    for bench, runs in data.items():
        windows = [50000, 100000, 500000, 1000000]
        global_eff = []
        per_pc_eff = []
        for w in windows:
            gn = f"mode2_global_w{w}"
            pn = f"mode3_per_pc_w{w}"
            global_eff.append(float(runs[gn]["eff"]) if gn in runs else None)
            per_pc_eff.append(float(runs[pn]["eff"]) if pn in runs else None)
        baseline = float(runs.get("mode0_no_prefetch", {}).get("eff", 0))
        fixed    = float(runs.get("mode1_fixed_stride", {}).get("eff", 0))
        plt.figure(figsize=(7.5, 4.5))
        plt.plot(windows, global_eff, "o-", label="Global FDP",  color="#DDAA33", lw=2, ms=8)
        plt.plot(windows, per_pc_eff, "s-", label="Per-PC FDP",  color="#BB5566", lw=2, ms=8)
        if baseline: plt.axhline(baseline, ls="--", color="#888888", label=f"No prefetch ({baseline/1e6:.2f}M)")
        if fixed: plt.axhline(fixed, ls=":", color="#4477AA", label=f"Fixed stride ({fixed/1e6:.2f}M)")
        plt.xscale("log")
        plt.xlabel("FDP feedback window (instructions, log scale)")
        plt.ylabel("Effective L1D misses")
        plt.title(f"FDP window-size sensitivity: {bench}")
        plt.legend()
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.savefig(f"fdp_window_sweep_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote fdp_window_sweep_{bench}.png")

def plot_summary(data):
    benches = sorted(data.keys())
    modes = [
        ("mode0_no_prefetch", "No prefetch"),
        ("mode1_fixed_stride", "Fixed stride"),
        ("mode2_global_w100000","Global FDP"),
        ("mode3_per_pc_w100000","Per-PC FDP (novel)"),
    ]
    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(benches))
    width = 0.18
    colors = ["#888888", "#4477AA", "#DDAA33", "#BB5566"]
    for i, (key, label) in enumerate(modes):
        vals = []
        for b in benches:
            v = data[b].get(key, {}).get("eff", 0)
            vals.append(v / 1e6)
        ax.bar(x + (i - 1.5) * width, vals, width, label=label, color=colors[i])
    ax.set_xticks(x); ax.set_xticklabels(benches)
    ax.set_ylabel("Effective L1D misses (millions)")
    ax.set_title("FDP comparison across benchmarks")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig("fdp_summary.png", dpi=140)
    plt.close()
    print("wrote fdp_summary.png")

def plot_accuracy(data):
    benches = sorted(data.keys())
    modes = [
        ("mode1_fixed_stride", "Fixed stride"),
        ("mode2_global_w100000","Global FDP"),
        ("mode3_per_pc_w100000", "Per-PC FDP"),
    ]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = np.arange(len(benches))
    width = 0.25
    colors = ["#4477AA", "#DDAA33", "#BB5566"]
    for i, (key, label) in enumerate(modes):
        vals = [data[b].get(key, {}).get("accuracy", 0) for b in benches]
        ax.bar(x + (i - 1) * width, vals, width, label=label, color=colors[i])
    ax.set_xticks(x); ax.set_xticklabels(benches)
    ax.set_ylabel("Prefetch accuracy (%)")
    ax.set_title("Prefetch accuracy: PF hits / PFs issued")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig("fdp_accuracy.png", dpi=140)
    plt.close()
    print("wrote fdp_accuracy.png")

if __name__ == "__main__":
    data = collect()
    if not data:
        raise SystemExit(1)
    plot_main_comparison(data)
    plot_window_sweep(data)
    plot_summary(data)
    plot_accuracy(data)
