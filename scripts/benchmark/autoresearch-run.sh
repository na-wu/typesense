#!/bin/bash
# autoresearch-run.sh — Build and benchmark on remote instance via SSM
# Usage: bash scripts/benchmark/autoresearch-run.sh [extra-server-flags]
#
# Prerequisites:
#   - AWS SSM access to the benchmark instance
#   - Source code changes committed and pushed to fork/autoresearch/* branch
#   - Snapshot baseline exists on the instance
#
# This script:
#   1. Pushes current branch to fork
#   2. SSM: pulls, builds, runs benchmark
#   3. Polls until benchmark completes
#   4. Prints restore_time and peak_rss

set -euo pipefail

INSTANCE_ID="i-0638c94ed443a37d5"
AWS_PROFILE="method_dev"
EXTRA_FLAGS="${1:---enable-simdjson-restore=true --parallel-restore-scan-threads=4 --max-indexing-concurrency=12}"
BRANCH=$(git branch --show-current)
TIMEOUT=4200  # 70 min max

echo "=== autoresearch-run ==="
echo "Branch: $BRANCH"
echo "Flags:  $EXTRA_FLAGS"
echo ""

# Step 1: Push current branch
echo "[1/5] Pushing branch to fork..."
git push fork "HEAD:refs/heads/$BRANCH" --force 2>&1 | tail -2

# Step 2: Build on instance
echo "[2/5] Building on instance (incremental ~2 min)..."
BUILD_CMD_ID=$(aws --profile "$AWS_PROFILE" ssm send-command \
  --instance-ids "$INSTANCE_ID" \
  --document-name "AWS-RunShellScript" \
  --timeout-seconds 600 \
  --parameters "{\"commands\":[\"export HOME=/root\",\"cd /opt/typesense-repo\",\"git fetch --all --prune 2>&1 | tail -1\",\"git checkout --detach origin/$BRANCH 2>&1 | tail -1\",\"TYPESENSE_VERSION=bench-autoresearch TYPESENSE_TARGET=typesense-server bash ci_build_v2.sh --arm --jobs=12 2>&1 | tail -5\",\"mkdir -p /opt/binaries/autoresearch\",\"cp -f dist/typesense-server /opt/binaries/autoresearch/typesense-server\",\"chmod +x /opt/binaries/autoresearch/typesense-server\",\"echo BUILD_OK\"]}" \
  --output text --query 'Command.CommandId' 2>&1)

echo "  Build command: $BUILD_CMD_ID"

# Wait for build
while true; do
  sleep 15
  STATUS=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
    --command-id "$BUILD_CMD_ID" --instance-id "$INSTANCE_ID" \
    --query 'Status' --output text 2>/dev/null || echo "Pending")
  if [[ "$STATUS" == "Success" ]]; then
    OUTPUT=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
      --command-id "$BUILD_CMD_ID" --instance-id "$INSTANCE_ID" \
      --query 'StandardOutputContent' --output text 2>/dev/null)
    if echo "$OUTPUT" | grep -q "BUILD_OK"; then
      echo "  Build succeeded"
      break
    else
      echo "  Build failed:"
      echo "$OUTPUT" | tail -10
      exit 1
    fi
  elif [[ "$STATUS" == "Failed" ]]; then
    echo "  Build command failed"
    aws --profile "$AWS_PROFILE" ssm get-command-invocation \
      --command-id "$BUILD_CMD_ID" --instance-id "$INSTANCE_ID" \
      --query 'StandardErrorContent' --output text 2>/dev/null | tail -10
    exit 1
  fi
  echo "  Building... ($STATUS)"
done

# Step 3: Run benchmark
echo "[3/5] Starting benchmark..."
BENCH_CMD_ID=$(aws --profile "$AWS_PROFILE" ssm send-command \
  --instance-ids "$INSTANCE_ID" \
  --document-name "AWS-RunShellScript" \
  --timeout-seconds 600 \
  --parameters "{\"commands\":[\"export HOME=/root\",\"pkill -9 -f typesense-server 2>/dev/null; sleep 5\",\"rm -rf /opt/typesense/data/*\",\"rsync -a /opt/typesense/snapshot-baseline/ /opt/typesense/data/\",\"echo 3 > /proc/sys/vm/drop_caches 2>/dev/null\",\"> /tmp/typesense-bench/typesense.log\",\"date +%s > /tmp/typesense-bench/start-ts.txt\",\"nohup /opt/binaries/autoresearch/typesense-server --data-dir /opt/typesense/data --api-key fa6412cd020797f9f1490a802c90bd7d25edf01892760d93f201a8b9c9a34d73 --api-port 8108 --log-dir /tmp/typesense-bench --num-documents-parallel-load 10000 --num-collections-parallel-load 2 $EXTRA_FLAGS > /tmp/typesense-bench/autoresearch-stdout.log 2>&1 &\",\"echo BENCH_STARTED\"]}" \
  --output text --query 'Command.CommandId' 2>&1)

# Wait for benchmark to start
sleep 20
STATUS=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
  --command-id "$BENCH_CMD_ID" --instance-id "$INSTANCE_ID" \
  --query 'Status' --output text 2>/dev/null || echo "Pending")
echo "  Benchmark launched ($STATUS)"

# Step 4: Poll for completion
echo "[4/5] Waiting for restore to complete (polling every 30s)..."
START=$(date +%s)

while true; do
  ELAPSED=$(( $(date +%s) - START ))
  if (( ELAPSED > TIMEOUT )); then
    echo "  TIMEOUT after ${ELAPSED}s"
    exit 1
  fi

  CHECK_CMD_ID=$(aws --profile "$AWS_PROFILE" ssm send-command \
    --instance-ids "$INSTANCE_ID" \
    --document-name "AWS-RunShellScript" \
    --timeout-seconds 30 \
    --parameters '{"commands":["grep \"Finished loading collections from disk\" /tmp/typesense-bench/typesense.log 2>/dev/null && echo RESTORE_DONE || echo STILL_LOADING","cat /tmp/typesense-bench/start-ts.txt 2>/dev/null || echo 0","date +%s","head -1 /tmp/typesense-bench/typesense.log 2>/dev/null | grep -oP \"\\d{8} \\d{2}:\\d{2}:\\d{2}\" || echo none","grep \"Finished loading collections from disk\" /tmp/typesense-bench/typesense.log 2>/dev/null | grep -oP \"\\d{8} \\d{2}:\\d{2}:\\d{2}\" || echo none"]}' \
    --output text --query 'Command.CommandId' 2>/dev/null)

  sleep 30

  RESULT=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
    --command-id "$CHECK_CMD_ID" --instance-id "$INSTANCE_ID" \
    --query 'StandardOutputContent' --output text 2>/dev/null || echo "")

  if echo "$RESULT" | grep -q "RESTORE_DONE"; then
    # Extract timestamps (prefer epoch from start-ts.txt, fall back to log timestamps)
    START_TS=$(echo "$RESULT" | sed -n '3p' | tr -d '[:space:]')
    END_TS=$(echo "$RESULT" | sed -n '4p' | tr -d '[:space:]')

    if [[ -n "$START_TS" && "$START_TS" != "0" && -n "$END_TS" && "$END_TS" =~ ^[0-9]+$ ]]; then
      RESTORE_TIME=$((END_TS - START_TS))
    else
      # Fallback: extract timestamps from glog lines (format: YYYYMMDD HH:MM:SS)
      LOG_START=$(echo "$RESULT" | sed -n '5p' | tr -d '[:space:]')
      LOG_END=$(echo "$RESULT" | sed -n '6p' | tr -d '[:space:]')
      echo "  [fallback] Using log timestamps: start=$LOG_START end=$LOG_END"
      if [[ "$LOG_START" != "none" && "$LOG_END" != "none" ]]; then
        # Convert YYYYMMDD HH:MM:SS to epoch
        S_EPOCH=$(date -d "${LOG_START:0:4}-${LOG_START:4:2}-${LOG_START:6:2} ${LOG_START:8}" +%s 2>/dev/null || echo 0)
        E_EPOCH=$(date -d "${LOG_END:0:4}-${LOG_END:4:2}-${LOG_END:6:2} ${LOG_END:8}" +%s 2>/dev/null || echo 0)
        RESTORE_TIME=$((E_EPOCH - S_EPOCH))
      else
        echo "  WARNING: Could not extract timestamps"
        RESTORE_TIME=0
      fi
    fi
    echo "  Restore completed in ${RESTORE_TIME}s"
    break
  fi

  # Show progress
  PROGRESS_CMD_ID=$(aws --profile "$AWS_PROFILE" ssm send-command \
    --instance-ids "$INSTANCE_ID" \
    --document-name "AWS-RunShellScript" \
    --timeout-seconds 15 \
    --parameters '{"commands":["grep accounts /tmp/typesense-bench/typesense.log | tail -1"]}' \
    --output text --query 'Command.CommandId' 2>/dev/null)
  sleep 8
  PROGRESS=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
    --command-id "$PROGRESS_CMD_ID" --instance-id "$INSTANCE_ID" \
    --query 'StandardOutputContent' --output text 2>/dev/null || echo "")
  echo "  [${ELAPSED}s] $PROGRESS"
done

# Step 5: Get final metrics
echo "[5/5] Collecting metrics..."
METRICS_CMD_ID=$(aws --profile "$AWS_PROFILE" ssm send-command \
  --instance-ids "$INSTANCE_ID" \
  --document-name "AWS-RunShellScript" \
  --timeout-seconds 30 \
  --parameters '{"commands":["ACTUAL_PID=$(pgrep -f \"autoresearch/typesense-server\" | head -1)","awk \"/VmHWM/{print \\$2}\" /proc/$ACTUAL_PID/status 2>/dev/null || echo 0","pkill -9 -f typesense-server 2>/dev/null"]}' \
  --output text --query 'Command.CommandId' 2>/dev/null)

sleep 10
PEAK_RSS_KB=$(aws --profile "$AWS_PROFILE" ssm get-command-invocation \
  --command-id "$METRICS_CMD_ID" --instance-id "$INSTANCE_ID" \
  --query 'StandardOutputContent' --output text 2>/dev/null | head -1)
PEAK_RSS_GB=$(echo "scale=1; ${PEAK_RSS_KB:-0} / 1048576" | bc 2>/dev/null || echo "0.0")

echo ""
echo "=== RESULTS ==="
echo "restore_time: ${RESTORE_TIME}s"
echo "peak_rss:     ${PEAK_RSS_GB}GB"
echo "branch:       $BRANCH"
echo "flags:        $EXTRA_FLAGS"
