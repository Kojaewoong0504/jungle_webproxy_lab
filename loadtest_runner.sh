#!/bin/bash
#
# loadtest_runner.sh - loadtest.sh를 10회 실행하여 평균/표준편차 분석
#

ITER=10
SCRIPT="./cache_test2.sh"
TMPFILE="./wrk_results.tmp"
rm -f $TMPFILE

echo "🔁 Running $SCRIPT $ITER times..."

for i in $(seq 1 $ITER); do
    echo "▶️ Test run $i..."
    $SCRIPT | tee runlog_$i.txt | grep "Requests/sec" | awk '{ print $2 }' >> $TMPFILE
done

# 평균, 표준편차 계산
echo
echo "📊 Performance Summary (Requests/sec):"
awk '{
  sum += $1;
  sumsq += ($1)^2;
}
END {
  mean = sum / NR;
  stddev = sqrt(sumsq / NR - mean^2);
  printf "Avg: %.2f, Stddev: %.2f, Min: %.2f, Max: %.2f, Samples: %d\n", mean, stddev, min, max, NR;
}' min=$(sort -n $TMPFILE | head -n1) max=$(sort -n $TMPFILE | tail -n1) $TMPFILE

rm -f $TMPFILE
