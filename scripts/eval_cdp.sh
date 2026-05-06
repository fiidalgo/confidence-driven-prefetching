# full disclosure: i had Claude Sonnet 4.6 help me write this eval script
# i felt that this was an acceptable use of AI as i had created the knobs and their flags
# in the cdp.cpp file i just needed to write the master script for performing the sweep in
# one pass
# note that the script uses relative paths from my ~/workspace/final_project/ directory
# to ~/workspace/assignment_2/ and ~/workspace/assignment_3/ in order to reference the
# benchmarks stored there

set -e

PIN=${PIN_ROOT}/pin
TOOL=../cdp/obj-intel64/cdp.so
HW3=../../assignment_2/benchmarks
HW4=../../assignment_3/benchmarks
CFG=../cdp/config-dm
LIBQ_CMD="$HW4/libquantum_O3 400 25"
HMMER_CMD="$HW4/hmmer_O3 $HW4/inputs/nph3.hmm $HW4/inputs/swiss41"
DEALII_CMD="$HW3/dealII_O3 10"
MAX_INST=200000000
RES=../cdp/results
mkdir -p $RES

run_one() {
    local bench=$1; local cmd=$2; local mode=$3; local extra=$4; local label=$5
    mkdir -p $RES/$bench
    local out=$RES/$bench/${label}.out
    echo "  [$bench] $label"
    $PIN -t $TOOL -config $CFG -outfile $out -max_inst $MAX_INST \
        -cdp_mode $mode $extra -- $cmd 2>/dev/null
}

sweep() {
    local bench=$1; local cmd=$2
    echo "=== $bench ==="

    # Baselines
    run_one $bench "$cmd" 0 "" "mode0_no_prefetch"
    run_one $bench "$cmd" 1 "" "mode1_fixed_stride"
    run_one $bench "$cmd" 2 "-fdp_window 100000" "mode2_global_fdp"

    # CDP default
    run_one $bench "$cmd" 3 "" "mode3_cdp_default"

    # CDP threshold sweep — explore design space
    # Sweep: when does prefetching start, how aggressive does it get?
    for pf in 1 2 3; do
        for d2 in 3 4 5; do
            for d4 in 5 6 7; do
                if [ $pf -lt $d2 ] && [ $d2 -lt $d4 ]; then
                    run_one $bench "$cmd" 3 \
                        "-cdp_threshold_pf $pf -cdp_threshold_d2 $d2 -cdp_threshold_d4 $d4" \
                        "mode3_cdp_pf${pf}_d2${d2}_d4${d4}"
                fi
            done
        done
    done

    # CDP with different max confidence
    for mc in 3 7 15; do
        run_one $bench "$cmd" 3 "-cdp_max_conf $mc" "mode3_cdp_maxconf${mc}"
    done

    # CDP + victim cache
    for v in 1 4 8; do
        run_one $bench "$cmd" 3 "-vc_entries $v" "mode3_cdp_vc${v}"
    done
}

sweep libquantum "$LIBQ_CMD"
sweep hmmer      "$HMMER_CMD"
sweep dealII     "$DEALII_CMD"

echo ""
echo "Done. Run: python3 plot_cdp.py"
