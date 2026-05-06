# full disclosure: i had Claude Sonnet 4.6 help me write this eval script
# i felt that this was an acceptable use of AI as i had created the knobs and their flags
# in the stride.cpp file i just needed to write the master script for performing the sweep in
# one pass
# note that the script uses relative paths from my ~/workspace/final_project/ directory
# to ~/workspace/assignment_2/ and ~/workspace/assignment_3/ in order to reference the
# benchmarks stored there

set -e

PIN=${PIN_ROOT}/pin
TOOL=../stride/obj-intel64/stride.so

HW4_BENCH=../../assignment_3/benchmarks
HW3_BENCH=../../assignment_2/benchmarks

CFG_BASE=../stride/config-base
CFG_DM=../stride/config-dm

# 200M instructions per run. Raise to 1000000000 for full benchmark.
MAX_INST=200000000

RESULTS=../stride/results
mkdir -p $RESULTS

run_one() {
    local benchname=$1
    local cmd=$2
    local cfg=$3
    local extra=$4
    local label=$5
    mkdir -p $RESULTS/$benchname
    local out=$RESULTS/$benchname/${label}.out
    echo "  [$benchname] $label"
    $PIN -t $TOOL -config $cfg -outfile $out -max_inst $MAX_INST $extra -- $cmd 2>/dev/null
}

sweep() {
    local name=$1
    local cmd=$2
    echo "=== $name ==="

    # HW4 baselines (no prefetch)
    run_one $name "$cmd" $CFG_BASE "-prefetch 0 -vc_entries 0" "baseline_8way"
    run_one $name "$cmd" $CFG_DM   "-prefetch 0 -vc_entries 0" "baseline_dm"

    # Victim cache sweep (reproduce PA3)
    for v in 1 2 3 4 5 6 7 8; do
        run_one $name "$cmd" $CFG_DM "-prefetch 0 -vc_entries $v" "vc${v}_dm"
    done

    # Stride prefetcher — RPT size sweep (main parameter)
    for rpt in 16 32 64 128 256; do
        run_one $name "$cmd" $CFG_DM \
            "-prefetch 1 -rpt $rpt -pf_buf_entries 16 -pf_degree 1 -pf_distance 1 -vc_entries 0" \
            "stride_rpt${rpt}"
    done

    # Prefetch degree sweep
    for deg in 1 2 4 8; do
        run_one $name "$cmd" $CFG_DM \
            "-prefetch 1 -rpt 64 -pf_buf_entries 16 -pf_degree $deg -pf_distance 1 -vc_entries 0" \
            "stride_deg${deg}"
    done

    # Prefetch buffer size sweep (hardware cost tradeoff)
    for pfb in 4 8 16 32; do
        run_one $name "$cmd" $CFG_DM \
            "-prefetch 1 -rpt 64 -pf_buf_entries $pfb -pf_degree 1 -pf_distance 1 -vc_entries 0" \
            "stride_pfb${pfb}"
    done

    # Combined: stride + victim cache
    for v in 1 4 8; do
        run_one $name "$cmd" $CFG_DM \
            "-prefetch 1 -rpt 64 -pf_buf_entries 16 -pf_degree 1 -vc_entries $v" \
            "stride_vc${v}"
    done

    # Stride on 8-way associative (shows diminishing returns)
    run_one $name "$cmd" $CFG_BASE \
        "-prefetch 1 -rpt 64 -pf_buf_entries 16 -pf_degree 1 -vc_entries 0" \
        "stride_8way"
}

sweep libquantum "$HW4_BENCH/libquantum_O3 400 25"
sweep hmmer      "$HW4_BENCH/hmmer_O3 $HW4_BENCH/inputs/nph3.hmm $HW4_BENCH/inputs/swiss41"
sweep dealII     "$HW3_BENCH/dealII_O3 10"

echo ""
echo "All done. Run: python3 plot_stride.py"
