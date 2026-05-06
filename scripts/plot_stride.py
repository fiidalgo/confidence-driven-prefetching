import os, re, glob
from collections import defaultdict
import matplotlib.pyplot as plt

RESULTS_DIR = "../stride/results"
DCACHE_RE = re.compile(r"D-Cache Miss:\s*(\d+)\s+out of\s+(\d+)")
VC_RE = re.compile(r"Victim Cache Hits:\s*(\d+)")
PFH_RE = re.compile(r"Prefetch Buffer Hits:\s*(\d+)")
EFF_RE = re.compile(r"EFFECTIVE D-Cache Misses[^:]*:\s*(\d+)")

def parse(fp):
    with open(fp) as f: text = f.read()
    out = {}
    m = DCACHE_RE.search(text)
    if m: out["dmiss"] = int(m.group(1)); out["dreq"] = int(m.group(2))
    m = VC_RE.search(text);  out["vc"]  = int(m.group(1)) if m else 0
    m = PFH_RE.search(text); out["pfh"] = int(m.group(1)) if m else 0
    m = EFF_RE.search(text)
    if m:
        val = int(m.group(1))
        out["eff"] = 0 if val > (1 << 62) else val
    else:
        out["eff"] = out.get("dmiss", 0)
    return out

def collect():
    data = defaultdict(dict)
    for bench_dir in sorted(glob.glob(os.path.join(RESULTS_DIR, "*"))):
        bench = os.path.basename(bench_dir)
        for fp in sorted(glob.glob(os.path.join(bench_dir, "*.out"))):
            label = os.path.basename(fp)[:-4]
            data[bench][label] = parse(fp)
    return data

def plot_misses(data):
    for bench, runs in data.items():
        plt.figure(figsize=(11, 5))
        labels, eff = [], []
        for name in sorted(runs.keys()):
            r = runs[name]
            if "eff" not in r: continue
            labels.append(name)
            eff.append(float(r["eff"]))
        x = list(range(len(labels)))
        plt.bar(x, eff)
        plt.xticks(x, labels, rotation=75, ha="right", fontsize=7)
        plt.ylabel("Effective D-Cache misses")
        plt.title(f"Effective L1D misses: {bench}")
        plt.grid(axis="y", alpha=0.3)
        plt.tight_layout()
        plt.savefig(f"stride_misses_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote stride_misses_{bench}.png")

def plot_breakdown(data):
    for bench, runs in data.items():
        rpt_runs = [(int(name.split("_")[1][3:]), runs[name]) for name in runs if name.startswith("stride_rpt")]
        if not rpt_runs: continue
        rpt_runs.sort()
        xs = [r[0] for r in rpt_runs]
        raw = [float(r[1].get("dmiss", 0)) for r in rpt_runs]
        eff = [float(r[1].get("eff", 0)) for r in rpt_runs]
        plt.figure(figsize=(7, 4))
        plt.plot(xs, raw, "o-", label="Raw L1D misses")
        plt.plot(xs, eff, "s-", label="After prefetcher")
        if "baseline_dm" in runs:
            plt.axhline(runs["baseline_dm"]["dmiss"], ls="--", color="grey", label="DM baseline")
        plt.xscale("log")
        plt.xlabel("RPT entries"); plt.ylabel("L1D misses")
        plt.title(f"Stride prefetcher: {bench}")
        plt.legend(); plt.grid(True, alpha=0.3)
        plt.tight_layout()
        plt.savefig(f"stride_breakdown_{bench}.png", dpi=140)
        plt.close()
        print(f"wrote stride_breakdown_{bench}.png")

if __name__ == "__main__":
    data = collect()
    if not data:
        raise SystemExit(1)
    plot_misses(data)
    plot_breakdown(data)
