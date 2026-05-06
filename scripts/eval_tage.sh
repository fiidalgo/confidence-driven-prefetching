# full disclosure: i had Claude Sonnet 4.6 help me write this eval script
# i felt that this was an acceptable use of AI as i had created the knobs and their flags
# in the tage.cpp file i just needed to write the master script for performing the sweep in
# one pass
# note that the script uses relative paths from my ~/workspace/final_project/ directory
# to ~/workspace/assignment_2/ and ~/workspace/assignment_3/ in order to reference the
# benchmarks stored there

set -e

PIN=${PIN_ROOT}/pin
TOOL=../tage/obj-intel64/tage.so

HW3_BENCH=../../assignment_2/benchmarks
HW4_BENCH=../../assignment_3/benchmarks

# 100M branches is enough for accuracy to converge on these benchmarks.
# Raise to 500000000 if you want tighter numbers (adds ~10 min/benchmark).
BRANCH_LIMIT=10000000

mkdir -p ../tage/results/libquantum
mkdir -p ../tage/results/dealII
mkdir -p ../tage/results/hmmer

echo "=== libquantum (1 of 3) ==="
$PIN -t $TOOL -multi 1 -outdir ../tage/results/libquantum \
    -l $BRANCH_LIMIT -- $HW3_BENCH/libquantum_O3 400 25
echo "Done."

echo ""
echo "=== dealII (2 of 3) ==="
$PIN -t $TOOL -multi 1 -outdir ../tage/results/dealII \
    -l $BRANCH_LIMIT -- $HW3_BENCH/dealII_O3 10
echo "Done."

echo ""
echo "=== hmmer (3 of 3) ==="
$PIN -t $TOOL -multi 1 -outdir ../tage/results/hmmer \
    -l $BRANCH_LIMIT \
    -- $HW4_BENCH/hmmer_O3 $HW4_BENCH/inputs/nph3.hmm $HW4_BENCH/inputs/swiss41
echo "Done."

echo ""
echo "All done. Run: python3 plot_tage.py"
