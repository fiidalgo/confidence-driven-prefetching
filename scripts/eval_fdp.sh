# full disclosure: i had Claude Sonnet 4.6 help me write this eval script
# i felt that this was an acceptable use of AI as i had created the knobs and their flags
# in the fdp.cpp file i just needed to write the master script for performing the sweep in
# one pass
# note that the script uses relative paths from my ~/workspace/final_project/ directory
# to ~/workspace/assignment_2/ and ~/workspace/assignment_3/ in order to reference the
# benchmarks stored there

set -e

PIN=${PIN_ROOT}/pin
TOOL=../fdp/obj-intel64/fdp.so

HW3=../../assignment_2/benchmarks
HW4=../../assignment_3/benchmarks
CFG=../fdp/config-dm

LIBQ_CMD="$HW4/libquantum_O3 400 25"
HMMER_CMD="$HW4/hmmer_O3 $HW4/inputs/nph3.hmm $HW4/inputs/swiss41"
DEALII_CMD="$HW3/dealII_O3 10"

MAX_INST=200000000
RPT=64
PFBUF=16

RES=../fdp/results
mkdir -p $RES

run_one() {
    local bench=$1
    local cmd=$2
    local mode=$3
    local extra=$4
    local label=$5
    mkdir -p $RES/$bench
    local out=$RES/$bench/${label}.out
    echo "  [$bench] $label"
    $PIN -t $TOOL -config $CFG -outfile $out -max_inst $MAX_INST \
        -fdp_mode $mode -rpt $RPT -pf_buf_entries $PFBUF \
        $extra -- $cmd 2>/dev/null
}

sweep() {
    local bench=$1
    local cmd=$2
    echo "=== $bench ==="

    # --- Mode 0: no prefetch baseline ---
    run_one $bench "$cmd" 0 "" "mode0_no_prefetch"

    # --- Mode 1: fixed stride (single config; reproduces stride.cpp result) ---
    run_one $bench "$cmd" 1 "" "mode1_fixed_stride"

    # --- Mode 2: global FDP, window sweep ---
    for win in 50000 100000 500000 1000000; do
        run_one $bench "$cmd" 2 "-fdp_window $win" "mode2_global_w${win}"
    done

    # --- Mode 3: per-PC FDP, window sweep (NOVEL) ---
    for win in 50000 100000 500000 1000000; do
        run_one $bench "$cmd" 3 "-fdp_window $win" "mode3_per_pc_w${win}"
    done

    # --- Mode 3: threshold sweep at fixed window=100K ---
    for ah in 60 75 90; do
        for al in 25 40 55; do
            run_one $bench "$cmd" 3 \
                "-fdp_window 100000 -fdp_acc_high $ah -fdp_acc_low $al" \
                "mode3_thresh_ah${ah}_al${al}"
        done
    done

    # --- Best per-PC config + victim cache combinations ---
    for v in 1 4 8; do
        run_one $bench "$cmd" 3 \
            "-fdp_window 100000 -vc_entries $v" \
            "mode3_vc${v}"
    done
}

sweep libquantum "$LIBQ_CMD"
sweep hmmer      "$HMMER_CMD"
sweep dealII     "$DEALII_CMD"

echo ""
echo "Done. Run: python3 plot_fdp.py"
