import os, re, glob
from collections import defaultdict
import matplotlib.pyplot as plt
import numpy as np

RESULTS_DIR = "../cdp/results"
EFF_RE = re.compile(r"EFFECTIVE D-Cache Misses:\s*(\d+)")
ISSUED_RE = re.compile(r"Stride Prefetches Issued:\s*(\d+)")
PFHIT_RE = re.compile(r"Prefetch Buffer Hits:\s*(\d+)")
ACC_RE = re.compile(r"Prefetch Accuracy:\s*([\d.]+)%")
DCMISS_RE = re.compile(r"D-Cache Miss:\s*(\d+)")
HIST_RE = re.compile(r"conf=(\d+):\s*(\d+)")

def parse(fp):
    out = {"hist": {}}
    with open(fp) as f: text = f.read()
    for k, r in [("eff", EFF_RE), ("issued", ISSUED_RE), ("pfhit", PFHIT_RE), ("dmiss", DCMISS_RE)]:
        m = r.search(text)
        out[k] = int(m.group(1)) if m else 0
    m = ACC_RE.search(text)
    out["accuracy"] = float(m.group(1)) if m else 0.0
    for m in HIST_RE.finditer(text):
        out["hist"][int(m.group(1))] = int(m.group(2))
    return out

def collect():
    data = defaultdict(dict)
    for bd in sorted(glob.glob(os.path.join(RESULTS_DIR, "*"))):
        bench = os.path.basename(bd)
        for fp in sorted(glob.glob(os.path.join(bd, "*.out"))):
            label = os.path.basename(fp)[:-4]
            data[bench][label] = parse(fp)
    return data

def plot_summary(data):
    benches = sorted(data.keys())
    modes = [
        ("mode0_no_prefetch", "No prefetch"),
        ("mode1_fixed_stride", "Fixed stride"),
        ("mode2_global_fdp", "Global FDP"),
        ("mode3_cdp_default", "CDP (novel)"),
    ]
    fig, ax = plt.subplots(figsize=(10, 5))
    x = np.arange(len(benches))
    width = 0.18
    # i just clicked around on the color wheel
    colors = ["#888888", "#4477AA", "#DDAA33", "#117733"]
    for i, (key, label) in enumerate(modes):
        vals = [data[b].get(key, {}).get("eff", 0) / 1e6 for b in benches]
        ax.bar(x + (i - 1.5) * width, vals, width, label=label, color=colors[i])
    ax.set_xticks(x); ax.set_xticklabels(benches)
    ax.set_ylabel("Effective L1D misses (millions)")
    ax.set_title("CDP comparison: no prefetch vs Chen-Baer vs FDP vs CDP")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig("cdp_summary.png", dpi=140)
    plt.close()
    print("wrote cdp_summary.png")

def plot_per_bench_bars(data):
    modes = [
        ("mode0_no_prefetch", "No prefetch"),
        ("mode1_fixed_stride", "Fixed stride"),
        ("mode2_global_fdp", "Global FDP"),
        ("mode3_cdp_default", "CDP"),
    ]
    colors = ["#888888", "#4477AA", "#DDAA33", "#117733"]
    for bench, runs in data.items():
        labels = [m[1] for m in modes]
        eff = [float(runs.get(m[0], {}).get("eff", 0)) for m in modes]
        plt.figure(figsize=(8, 4.5))
        bars = plt.bar(labels, eff, color=colors)
        plt.ylabel("Effective L1D misses")
        plt.title(f"CDP comparison: {bench}")
        plt.grid(axis="y", alpha=0.3)
        for bar, v in zip(bars, eff):
            txt = f"{v/1e6:.2f}M" if v > 1e6 else f"{v/1e3:.0f}K"
            plt.text(bar.get_x() + bar.get_width()/2, v, txt, ha="center", va="bottom", fontsize=10)
        plt.tight_layout()
        plt.savefig(f"cdp_comparison_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote cdp_comparison_{bench}.png")

def plot_accuracy(data):
    benches = sorted(data.keys())
    modes = [
        ("mode1_fixed_stride", "Fixed stride"),
        ("mode2_global_fdp", "Global FDP"),
        ("mode3_cdp_default", "CDP"),
    ]
    fig, ax = plt.subplots(figsize=(8, 4.5))
    x = np.arange(len(benches))
    width = 0.25
    colors = ["#4477AA", "#DDAA33", "#117733"]
    for i, (key, label) in enumerate(modes):
        vals = [data[b].get(key, {}).get("accuracy", 0) for b in benches]
        ax.bar(x + (i - 1) * width, vals, width, label=label, color=colors[i])
    ax.set_xticks(x); ax.set_xticklabels(benches)
    ax.set_ylabel("Prefetch accuracy (%)")
    ax.set_title("Prefetch accuracy: PF hits / PFs issued")
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig("cdp_accuracy.png", dpi=140)
    plt.close()
    print("wrote cdp_accuracy.png")

def plot_threshold_sweep(data):
    for bench, runs in data.items():
        sweep_keys = sorted([k for k in runs if k.startswith("mode3_cdp_pf")])
        if not sweep_keys: continue
        labels = []
        eff = []
        for k in sweep_keys:
            # extract pf/d2/d4 from label
            m = re.match(r"mode3_cdp_pf(\d)_d2(\d)_d4(\d)", k)
            if not m: continue
            labels.append(f"({m.group(1)},{m.group(2)},{m.group(3)})")
            eff.append(float(runs[k].get("eff", 0)))
        plt.figure(figsize=(11, 4.5))
        plt.bar(range(len(labels)), eff, color="#117733")
        plt.xticks(range(len(labels)), labels, rotation=70, fontsize=8)
        plt.ylabel("Effective L1D misses")
        plt.xlabel("(threshold_pf, threshold_d2, threshold_d4)")
        plt.title(f"CDP threshold sweep: {bench}")
        # baseline references
        for key, color, dash in [
            ("mode1_fixed_stride", "#4477AA", "--"),
            ("mode3_cdp_default", "#117733", ":"),
        ]:
            v = runs.get(key, {}).get("eff", 0)
            if v > 0:
                plt.axhline(v, ls=dash, color=color, label=f"{key}: {v/1e6:.2f}M")
        plt.legend()
        plt.grid(axis="y", alpha=0.3)
        plt.tight_layout()
        plt.savefig(f"cdp_threshold_sweep_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote cdp_threshold_sweep_{bench}.png")

def plot_confidence_hist(data):
    for bench, runs in data.items():
        if "mode3_cdp_default" not in runs: continue
        hist = runs["mode3_cdp_default"].get("hist", {})
        if not hist: continue
        levels = sorted(hist.keys())
        counts = [hist[l] for l in levels]
        plt.figure(figsize=(8, 4))
        plt.bar(levels, counts, color="#117733")
        plt.xlabel("Confidence level (0 = none, max = saturated)")
        plt.ylabel("Number of RPT entries")
        plt.title(f"Final confidence distribution: {bench}")
        plt.xticks(levels)
        plt.grid(axis="y", alpha=0.3)
        for x, y in zip(levels, counts):
            if y > 0:
                plt.text(x, y, str(y), ha="center", va="bottom", fontsize=10)
        plt.tight_layout()
        plt.savefig(f"cdp_confidence_hist_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote cdp_confidence_hist_{bench}.png")

if __name__ == "__main__":
    data = collect()
    if not data:
        raise SystemExit(1)
    plot_summary(data)
    plot_per_bench_bars(data)
    plot_accuracy(data)
    plot_threshold_sweep(data)
    plot_confidence_hist(data)
