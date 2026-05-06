import os
import re
import glob
from collections import defaultdict
import matplotlib.pyplot as plt

RESULTS_DIR = "../tage/results"
ACC_RE = re.compile(r"Accuracy:\s*([\d.]+)%")
SEEN_RE = re.compile(r"Count Seen:\s*(\d+)")
CORRECT_RE = re.compile(r"Count Correct:\s*(\d+)")

def cost_1bit(sz): return sz * 1
def cost_2bit(sz): return sz * 2
def cost_2lvl(hhrt, pt): return hhrt * 12 + pt * 2
def cost_gshare(sz, h): return sz * 2 + h
def cost_tage(n, ts, tag_bits, base):
    return base * 2 + n * ts * (3 + tag_bits + 2)

def parse_outfile(path):
    with open(path) as f:
        text = f.read()
    m = ACC_RE.search(text)
    if m: return float(m.group(1))
    s = SEEN_RE.search(text); c = CORRECT_RE.search(text)
    if s and c and int(s.group(1)) > 0:
        return 100.0 * int(c.group(1)) / int(s.group(1))
    return None

def parse_label(label):
    if label.startswith("1bit_bpb"):
        sz = int(label.replace("1bit_bpb", ""))
        return ("1bit", {"size": sz}, cost_1bit(sz))
    if label.startswith("2bit_bpb"):
        sz = int(label.replace("2bit_bpb", ""))
        return ("2bit", {"size": sz}, cost_2bit(sz))
    if label.startswith("2lvl_"):
        sz = int(label.split("_")[1])
        return ("2lvl", {"size": sz}, cost_2lvl(sz, sz))
    if label.startswith("gshare_"):
        parts = label.split("_")
        sz = int(parts[1]); h = int(parts[2][1:])
        return ("gshare", {"size": sz, "h": h}, cost_gshare(sz, h))
    if label.startswith("tage_"):
        parts = label.split("_")
        n = int(parts[1][1:]); ts = int(parts[2][2:])
        return ("tage", {"n": n, "ts": ts}, cost_tage(n, ts, tag_bits=11, base=16384))
    return None

def collect():
    data = defaultdict(list)
    for bench_dir in sorted(glob.glob(os.path.join(RESULTS_DIR, "*"))):
        bench = os.path.basename(bench_dir)
        for fp in sorted(glob.glob(os.path.join(bench_dir, "*.out"))):
            label = os.path.basename(fp)[:-4]
            parsed = parse_label(label)
            if not parsed: continue
            scheme, _, bits = parsed
            acc = parse_outfile(fp)
            if acc is None: continue
            data[bench].append((scheme, label, bits, acc))
    return data

def plot_design_space(data):
    schemes = ["1bit", "2bit", "2lvl", "gshare", "tage"]
    colors  = {"1bit": "C0", "2bit": "C1", "2lvl": "C2", "gshare": "C3", "tage": "C4"}
    for bench, points in data.items():
        plt.figure(figsize=(8,5))
        for s in schemes:
            xs = sorted([p for p in points if p[0] == s], key=lambda x: x[2])
            if not xs: continue
            plt.plot([p[2] for p in xs], [p[3] for p in xs], "o-", label=s, color=colors[s])
        plt.xscale("log")
        plt.xlabel("Predictor budget (bits, log scale)")
        plt.ylabel("Branch prediction accuracy (%)")
        plt.title(f"Branch predictor design space: {bench}")
        plt.legend()
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.savefig(f"tage_design_space_{bench}.png", dpi=130)
        plt.close()
        print(f"wrote tage_design_space_{bench}.png")

def plot_summary(data):
    benches = sorted(data.keys())
    schemes = ["1bit", "2bit", "2lvl", "gshare", "tage"]
    best = {b: {s: 0 for s in schemes} for b in benches}
    for b, points in data.items():
        for scheme, _, _, acc in points:
            if acc > best[b][scheme]: best[b][scheme] = acc
    fig, ax = plt.subplots(figsize=(9, 5))
    width = 0.15
    x = list(range(len(benches)))
    for i, s in enumerate(schemes):
        ax.bar([xi + (i - 2) * width for xi in x], [best[b][s] for b in benches], width, label=s)
    ax.set_xticks(x)
    ax.set_xticklabels(benches)
    ax.set_ylabel("Best accuracy (%)")
    ax.set_title("Best-of-each-scheme accuracy across benchmarks")
    ax.set_ylim([85, 100])
    ax.legend()
    ax.grid(axis="y", alpha=0.3)
    plt.tight_layout()
    plt.savefig("tage_summary.png", dpi=130)
    plt.close()
    print("wrote tage_summary.png")

if __name__ == "__main__":
    data = collect()
    if not data:
        raise SystemExit(1)
    plot_design_space(data)
    plot_summary(data)
