#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp_base=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$test_tmp_base/0mk-v02-test.XXXXXX")
runner_pid=
child_pid=

cleanup() {
    if test -n "$runner_pid"; then
        kill -TERM "$runner_pid" 2>/dev/null || :
    fi
    if test -n "$child_pid"; then
        kill -TERM "$child_pid" 2>/dev/null || :
    fi
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'v0.2 test failure: %s\n' "$*" >&2
    exit 1
}

expect_status() {
    expected_status=$1
    shift
    set +e
    "$@"
    actual_status=$?
    set -e
    test "$actual_status" -eq "$expected_status" ||
        fail "expected exit $expected_status, got $actual_status: $*"
}

assert_text() {
    grep -Fq -- "$2" "$1" || fail "missing text in $1: $2"
}

assert_event() {
    grep -Eq "^$2[[:space:]]+$3([[:space:]]|$)" "$1" ||
        fail "missing $2 event for $3 in $1"
}

mode_of() {
    if stat -c '%a' "$1" >/dev/null 2>&1; then
        stat -c '%a' "$1"
    elif stat -f '%Lp' "$1" >/dev/null 2>&1; then
        stat -f '%Lp' "$1"
    else
        fail 'need a stat implementation that reports Unix modes'
    fi
}

if test "$#" -gt 1; then
    fail 'usage: tests/v02.sh [MK0_BINARY]'
fi

if test "$#" -eq 1; then
    case $1 in
        /*) mk0_bin=$1 ;;
        *) mk0_bin=$(CDPATH= cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1") ;;
    esac
else
    test_cxx=${CXX:-c++}
    mk0_bin="$tmp/0mk"
    "$test_cxx" ${CPPFLAGS:-} ${CXXFLAGS:-} \
        -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
        "$root/0mk.cpp" -o "$mk0_bin"
fi

test -x "$mk0_bin" || fail "not an executable: $mk0_bin"
source_state_was_absent=false
if test ! -e "$root/tests/.0mk"; then
    source_state_was_absent=true
fi

project="$tmp/v0.2 project with spaces"
mkdir -p "$project/fixtures"
cp "$root/tests/input.txt" "$root"/tests/fixtures/*file "$project/"
cp -R "$root/tests/fixtures/bin" "$project/fixtures/"
cd "$project"

state="$project/state with spaces"

# The command-oriented CLI is queryable without running work.
"$mk0_bin" -f V02file --state-dir "$state" targets > targets.out
assert_text targets.out 'executable.sh'
assert_text targets.out 'output-tree'
assert_text targets.out 'counted.txt'
assert_text targets.out 'tool-output.txt'
assert_text targets.out 'env-output.txt'
"$mk0_bin" -f V02file --state-dir "$state" plan executable.sh > plan.out
assert_text plan.out 'executable.sh'
test ! -e executable.sh
test ! -e "$state" || fail 'planning must not create the state directory'

# A state directory must be 0mk-owned and may not consume ordinary project data.
mkdir -p work objects
printf '%s\n' keep > work/project-data
printf '%s\n' keep > objects/project-data
expect_status 2 "$mk0_bin" -f V02file --state-dir . cache gc > unsafe-state.out 2>&1
test "$(cat work/project-data)" = keep
test "$(cat objects/project-data)" = keep
rm -rf work objects
unmarked_state="$project/unmarked state"
mkdir -p "$unmarked_state/work"
printf '%s\n' keep > "$unmarked_state/work/project-data"
expect_status 2 "$mk0_bin" -f V02file --state-dir "$unmarked_state" cache gc \
    > unmarked-state.out 2>&1
test "$(cat "$unmarked_state/work/project-data")" = keep

# Files and directory trees retain their complete artifact mode when restored.
"$mk0_bin" -f V02file --state-dir "$state" run executable.sh output-tree
test "$(mode_of executable.sh)" = 751
test "$(mode_of output-tree)" = 750
test "$(mode_of output-tree/nested/data.txt)" = 640
cmp input.txt output-tree/nested/data.txt

# A relative executable nested in a declared tree input is provided once by
# that immutable tree snapshot while retaining its tool provenance.
"$mk0_bin" -f V02file --state-dir "$state" tree-tool-output.txt
test "$(cat tree-tool-output.txt)" = tool-v1

"$mk0_bin" -f V02file --state-dir "$state" inspect executable.sh > inspect.out
assert_text inspect.out 'task'
assert_text inspect.out 'output'
assert_text inspect.out 'input'
assert_text inspect.out 'input.txt'
assert_text inspect.out 'executor'
"$mk0_bin" -f V02file --state-dir "$state" --json inspect executable.sh > inspect.json
grep -Eq '^\{.*"target".*"executable.sh".*\}$' inspect.json ||
    fail 'inspect --json did not emit one JSON object'

# Machine-readable events own stdout; recipe output is diagnostic stderr.
json_state="$project/json state"
"$mk0_bin" -f V02file --state-dir "$json_state" --json noisy.txt \
    > noisy.jsonl 2> noisy.stderr
assert_text noisy.stderr 'recipe-noise'
python3 - noisy.jsonl <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    events = [json.loads(line) for line in stream if line.strip()]
if not events or any(not isinstance(event, dict) for event in events):
    raise SystemExit("invalid JSONL event stream")
PY

rm -rf executable.sh output-tree
"$mk0_bin" -f V02file --state-dir "$state" run executable.sh output-tree
test "$(mode_of executable.sh)" = 751
test "$(mode_of output-tree)" = 750
test "$(mode_of output-tree/nested/data.txt)" = 640

chmod 600 executable.sh
"$mk0_bin" -f V02file --state-dir "$state" run executable.sh
test "$(mode_of executable.sh)" = 751 || fail 'cache restore did not repair the output mode'

# A changed executable changes the executor fingerprint and cannot reuse stale output.
tool_state="$project/tool state"
rm -f tool-output.txt
"$mk0_bin" -f V02file --state-dir "$tool_state" tool-output.txt
test "$(cat tool-output.txt)" = tool-v1
sed 's/tool-v1/tool-v2/' fixtures/bin/tool-version > fixtures/bin/tool-version.new
mv fixtures/bin/tool-version.new fixtures/bin/tool-version
chmod +x fixtures/bin/tool-version
"$mk0_bin" -f V02file --state-dir "$tool_state" why tool-output.txt > tool-why.out
assert_text tool-why.out 'tool:fixtures/bin/tool-version changed (previous sha256:'
if grep -Fq 'tool:fixtures/bin/tool-version was removed' tool-why.out; then
    fail 'why misreported the current tool input as removed'
fi
"$mk0_bin" -f V02file --state-dir "$tool_state" tool-output.txt
test "$(cat tool-output.txt)" = tool-v2 || fail 'changed executable reused a stale result'

# Only explicitly declared environment variables participate in task identity.
env_state="$project/environment state"
rm -f env-output.txt
MK0_TEST_VALUE=one "$mk0_bin" -f V02file --state-dir "$env_state" \
    --env MK0_TEST_VALUE env-output.txt
test "$(cat env-output.txt)" = one
MK0_TEST_VALUE=two "$mk0_bin" -f V02file --state-dir "$env_state" \
    --env MK0_TEST_VALUE env-output.txt
test "$(cat env-output.txt)" = two || fail 'declared environment change reused a stale result'
MK0_TEST_VALUE=two "$mk0_bin" -f V02file --state-dir "$env_state" \
    --env MK0_TEST_VALUE inspect env-output.txt > env-inspect.out
if grep -Fq 'MK0_TEST_VALUE=two' env-inspect.out; then
    fail 'inspect exposed a declared environment value instead of its identity'
fi

"$mk0_bin" -f V02file --state-dir "$state" cache verify > cache-verify.out
assert_text cache-verify.out 'verified'
"$mk0_bin" -f V02file --state-dir "$state" cache du > cache-du.out
grep -Eq '[0-9]+' cache-du.out || fail 'cache du did not report a size'
"$mk0_bin" -f V02file --state-dir "$state" cache gc > cache-gc.out
"$mk0_bin" -f V02file --state-dir "$state" cache verify > cache-verify-after-gc.out

# Receipt manifest sizes are part of cache integrity for both outputs and inputs.
size_state="$project/manifest size state"
rm -f counted.txt
"$mk0_bin" -f V02file --state-dir "$size_state" counted.txt >/dev/null
size_receipt=$(find "$size_state/receipts" -type f -name '*.txt' -print -quit)
test -n "$size_receipt" || fail 'manifest-size test did not create a receipt'
output_size=$(wc -c < counted.txt | tr -d ' ')
input_size=$(wc -c < input.txt | tr -d ' ')
sed 's/^OUTPUT_SIZE=.*/OUTPUT_SIZE=999999/' "$size_receipt" > "$size_receipt.tmp"
mv "$size_receipt.tmp" "$size_receipt"
expect_status 4 "$mk0_bin" -f V02file --state-dir "$size_state" \
    cache verify > corrupt-output-size.out 2>&1
sed "s/^OUTPUT_SIZE=.*/OUTPUT_SIZE=$output_size/; \
     s/^INPUT_0_ARTIFACT_SIZE=.*/INPUT_0_ARTIFACT_SIZE=999999/" \
    "$size_receipt" > "$size_receipt.tmp"
mv "$size_receipt.tmp" "$size_receipt"
expect_status 4 "$mk0_bin" -f V02file --state-dir "$size_state" \
    cache verify > corrupt-input-size.out 2>&1
sed "s/^INPUT_0_ARTIFACT_SIZE=.*/INPUT_0_ARTIFACT_SIZE=$input_size/" \
    "$size_receipt" > "$size_receipt.tmp"
mv "$size_receipt.tmp" "$size_receipt"
"$mk0_bin" -f V02file --state-dir "$size_state" cache verify >/dev/null

# Corrupt fanout symlinks are reported and never traversed by verification or GC.
unsafe_cache_state="$project/unsafe cache state"
"$mk0_bin" -f V02file --state-dir "$unsafe_cache_state" counted.txt >/dev/null
unsafe_cache_external="$project/unsafe cache external"
mkdir -p "$unsafe_cache_external"
printf '%s\n' keep > "$unsafe_cache_external/sentinel"
ln -s "$unsafe_cache_external" "$unsafe_cache_state/objects/aa"
expect_status 4 "$mk0_bin" -f V02file --state-dir "$unsafe_cache_state" \
    cache verify > unsafe-cache-verify.out 2>&1
expect_status 2 "$mk0_bin" -f V02file --state-dir "$unsafe_cache_state" \
    cache gc > unsafe-cache-gc.out 2>&1
test "$(cat "$unsafe_cache_external/sentinel")" = keep

# Cache policies are explicit, and local execution cannot claim hermeticity.
declared_state="$project/declared state"
rm -f counted.txt
"$mk0_bin" -f V02file --state-dir "$declared_state" \
    --cache declared counted.txt > declared-first.out
"$mk0_bin" -f V02file --state-dir "$declared_state" \
    --cache declared counted.txt > declared-second.out
assert_event declared-first.out run counted.txt
assert_event declared-second.out current counted.txt
"$mk0_bin" -f V02file --state-dir "$declared_state" --cache off \
    why counted.txt > cache-policy-why.out
assert_text cache-policy-why.out 'cache policy changed from declared to off'

off_state="$project/off state"
rm -f counted.txt
"$mk0_bin" -f V02file --state-dir "$off_state" \
    --cache off counted.txt > off-first.out
"$mk0_bin" -f V02file --state-dir "$off_state" \
    --cache off counted.txt > off-second.out
assert_event off-first.out run counted.txt
assert_event off-second.out run counted.txt

hermetic_state="$project/hermetic state"
expect_status 2 "$mk0_bin" -f V02file --state-dir "$hermetic_state" \
    --cache hermetic executable.sh > hermetic.out 2>&1
assert_text hermetic.out 'hermetic'

# Executable discovery uses the versioned protocol, and costly work requires approval.
mock_path="$project/fixtures/bin:${PATH:-/usr/bin:/bin}"
mock_state="$project/mock executor state"
mock_requests="$project/mock-requests.jsonl"
mock_mk0_state="$project/mock 0mk state"
rm -f external.txt "$mock_requests"
run_mock() {
    env "PATH=$mock_path" \
        'MK0_EXEC_MOCK_CACHE_POLICY=hermetic' \
        'MK0_EXEC_MOCK_COST_USD=0.25' \
        'MK0_EXEC_MOCK_REQUIRE_SUBMIT=1' \
        'MK0_EXEC_MOCK_WARNING=mock planning warning' \
        "MK0_EXEC_MOCK_STATE_DIR=$mock_state" \
        "MK0_EXEC_MOCK_REQUEST_LOG=$mock_requests" "$@"
}

expect_status 1 env "PATH=$mock_path" \
    'MK0_EXEC_MOCK_CACHE_POLICY=bogus' \
    "$mk0_bin" -f Executorfile --state-dir "$project/mock identity failure" \
        plan external.txt > mock-identity-failure.out 2>&1
assert_text mock-identity-failure.out 'invalid mock cache policy'

run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_mk0_state" \
    --cache hermetic plan external.txt > mock-plan.out 2> mock-plan.err
assert_event mock-plan.out run external.txt
assert_text mock-plan.out 'cost_usd=0.25'
assert_text mock-plan.err 'mock planning warning'
test ! -e external.txt

mock_deferred_state="$project/mock deferred state"
rm -f "$mock_requests"
run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_deferred_state" \
    --cache hermetic plan external-deferred.txt > mock-deferred.out 2> mock-deferred.err
assert_event mock-deferred.out deferred external-deferred.txt
assert_text mock-deferred.out 'waiting=prepared.txt'
test "$(grep -c '"operation":"plan"' "$mock_requests")" -eq 1 ||
    fail 'executor planned a downstream task before its input manifest was known'

expect_status 3 run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_mk0_state" \
    --cache hermetic external.txt > mock-unapproved.out 2>&1
assert_text mock-unapproved.out 'requires --submit'
if grep -Fq '"operation":"run"' "$mock_requests"; then
    fail 'unapproved remote work invoked the executor run operation'
fi

# An executor can recheck changed placement/cost at run time and request approval.
mock_recheck_state="$project/mock recheck state"
expect_status 3 env "PATH=$mock_path" \
    'MK0_EXEC_MOCK_CACHE_POLICY=hermetic' \
    'MK0_EXEC_MOCK_COST_USD=0' \
    'MK0_EXEC_MOCK_REQUIRE_SUBMIT=0' \
    'MK0_EXEC_MOCK_RUN_REQUIRE_SUBMIT=1' \
    "MK0_EXEC_MOCK_STATE_DIR=$mock_state" \
    "$mk0_bin" -f Executorfile --state-dir "$mock_recheck_state" \
        --cache hermetic external.txt > mock-recheck.out 2>&1
assert_text mock-recheck.out 'requires submit approval'

run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_mk0_state" \
    --cache hermetic --submit external.txt > mock-run.out 2> mock-run.err
test "$(cat external.txt)" = 'HELLO 0MK'
assert_event mock-run.out run external.txt
assert_event mock-run.out done external.txt

run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_mk0_state" \
    --cache hermetic --submit external-action > mock-action.out 2> mock-action.err
assert_event mock-action.out run external-action
assert_event mock-action.out done external-action
test ! -e external-action

run_mock "$mk0_bin" -f Executorfile --state-dir "$mock_mk0_state" \
    inspect external.txt > mock-inspect.out
assert_text mock-inspect.out 'handle=mock:'
assert_text mock-inspect.out 'metadata.executor=mock'
grep -Fq '"operation":"identity"' "$mock_requests" ||
    fail 'external executor identity was not requested'
grep -Fq '"operation":"plan"' "$mock_requests" ||
    fail 'external executor plan was not requested'
grep -Fq '"operation":"run"' "$mock_requests" ||
    fail 'external executor run was not requested after approval'
grep -Eq '"operation":"run".*"task":\{.*"attempt":"[^"]+"' "$mock_requests" ||
    fail 'external run request omitted its nonempty attempt identity'
grep -Fq '"operation":"inspect"' "$mock_requests" ||
    fail 'external durable handle was not inspected'
test "$(find "$mock_state" -type f | wc -l)" -eq 2 ||
    fail 'mock executor did not persist the artifact and action handles'

# Cancellation also covers executor identity discovery and does not orphan the adapter.
slow_pid_file="$project/slow-executor.pid"
slow_mk0_state="$project/slow executor state"
rm -f "$slow_pid_file"
env "PATH=$mock_path" "MK0_EXEC_SLOW_PID_FILE=$slow_pid_file" \
    "$mk0_bin" -f SlowExecutorfile --state-dir "$slow_mk0_state" \
        plan slow.txt > slow-executor.out 2>&1 &
runner_pid=$!
attempt=0
while test ! -s "$slow_pid_file"; do
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        fail '0mk exited before slow executor discovery started'
    fi
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || fail 'timed out waiting for slow executor discovery'
    sleep 1
done
IFS= read -r child_pid < "$slow_pid_file"
kill -TERM "$runner_pid"
set +e
wait "$runner_pid"
slow_status=$?
set -e
runner_pid=
test "$slow_status" -eq 143 || fail "slow executor SIGTERM exit was $slow_status, expected 143"
attempt=0
while kill -0 "$child_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || fail 'slow executor survived runner cancellation'
    sleep 1
done
child_pid=

# A forced rebuild under a reusable policy preserves evidence of nondeterminism.
nondeterministic_state="$project/nondeterministic state"
rm -f nondeterministic.txt
"$mk0_bin" -f V02file --state-dir "$nondeterministic_state" \
    --cache declared nondeterministic.txt
first_nondeterministic_output=$(cat nondeterministic.txt)
expect_status 1 "$mk0_bin" -f V02file --state-dir "$nondeterministic_state" \
    --cache declared --force nondeterministic.txt > nondeterministic.out 2>&1
assert_text nondeterministic.out 'nondetermin'
test "$(cat nondeterministic.txt)" = "$first_nondeterministic_output" ||
    fail 'nondeterministic execution replaced the previously published artifact'
nondeterministic_attempt=$(grep -rl '^STATUS=nondeterministic$' \
    "$nondeterministic_state/runs")
test -n "$nondeterministic_attempt" || fail 'nondeterministic attempt was not persisted'
nondeterministic_digest=$(sed -n 's/^OUTPUT_DIGEST=//p' "$nondeterministic_attempt")
test -n "$nondeterministic_digest" || fail 'nondeterministic output manifest was not persisted'
nondeterministic_object="$nondeterministic_state/objects/$(printf '%s' "$nondeterministic_digest" | cut -c1-2)/$(printf '%s' "$nondeterministic_digest" | cut -c3-)"
test -f "$nondeterministic_object" || fail 'nondeterministic output bytes were not preserved'
"$mk0_bin" -f V02file --state-dir "$nondeterministic_state" cache gc >/dev/null
test -f "$nondeterministic_object" || fail 'cache gc discarded nondeterminism evidence'

# Reuse-off tasks are intentionally volatile: each successful result is published.
volatile_state="$project/volatile state"
rm -f nondeterministic.txt
"$mk0_bin" -f V02file --state-dir "$volatile_state" --cache off nondeterministic.txt
first_volatile_output=$(cat nondeterministic.txt)
"$mk0_bin" -f V02file --state-dir "$volatile_state" --cache off nondeterministic.txt
test "$(cat nondeterministic.txt)" != "$first_volatile_output" ||
    fail 'cache off did not publish the later volatile result'

# -C changes the project root before resolving the 0mkfile and state.
cd "$tmp"
"$mk0_bin" -C "$project" -f V02file --state-dir "$state" why executable.sh > chdir-why.out
assert_text chdir-why.out 'executable.sh'
cd "$project"

# Independent ready tasks overlap when -j permits it.
parallel_state="$project/parallel state"
barrier_dir="$project/parallel barrier"
mkdir -p "$barrier_dir"
rm -f parallel-a.txt parallel-b.txt \
    "$barrier_dir/parallel-a.started" "$barrier_dir/parallel-b.started"
MK0_TEST_BARRIER_DIR="$barrier_dir" \
    "$mk0_bin" -f Parallelfile --state-dir "$parallel_state" -j 2 all
test "$(cat parallel-a.txt)" = parallel
test "$(cat parallel-b.txt)" = parallel
test -e "$barrier_dir/parallel-a.started"
test -e "$barrier_dir/parallel-b.started"
expect_status 2 "$mk0_bin" -f Parallelfile -j 0 all > invalid-jobs.out 2>&1
expect_status 2 "$mk0_bin" -f Parallelfile -j 257 all > excessive-jobs.out 2>&1

# --keep-going runs an independent target after another target fails.
keep_state="$project/keep-going state"
rm -f bad.txt good.txt
expect_status 1 "$mk0_bin" -f KeepGoingfile --state-dir "$keep_state" \
    --keep-going bad.txt good.txt > keep-going.out 2>&1
cmp input.txt good.txt

# Cancellation reaches the complete recipe process group and cleans its workspace.
signal_state="$project/signal state"
signal_dir="$project/signal observation"
mkdir -p "$signal_dir"
rm -f "$signal_dir/child.pid" "$signal_dir/signal.seen" signal.txt after-signal.txt
MK0_TEST_SIGNAL_DIR="$signal_dir" \
    "$mk0_bin" -f Signalfile --state-dir "$signal_state" --keep-going \
        signal.txt after-signal.txt > signal.out 2>&1 &
runner_pid=$!
attempt=0
while test ! -s "$signal_dir/child.pid"; do
    if ! kill -0 "$runner_pid" 2>/dev/null; then
        fail '0mk exited before the signal test recipe started'
    fi
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || fail 'timed out waiting for the signal test recipe'
    sleep 1
done
IFS= read -r child_pid < "$signal_dir/child.pid"
kill -TERM "$runner_pid"
set +e
wait "$runner_pid"
signal_status=$?
set -e
runner_pid=
test "$signal_status" -eq 143 || fail "SIGTERM exit was $signal_status, expected 143"

attempt=0
while kill -0 "$child_pid" 2>/dev/null; do
    attempt=$((attempt + 1))
    test "$attempt" -lt 10 || fail 'recipe child survived cancellation'
    sleep 1
done
child_pid=
test "$(cat "$signal_dir/signal.seen")" = TERM
test ! -e signal.txt
test ! -e after-signal.txt || fail '0mk launched more work after cancellation'
if test -d "$signal_state/work" &&
   find "$signal_state/work" -mindepth 1 -print -quit | grep -q .; then
    fail 'cancellation left a task workspace behind'
fi

if test "$source_state_was_absent" = true && test -e "$root/tests/.0mk"; then
    fail 'v0.2 tests wrote .0mk under tests/'
fi
printf '%s\n' '0mk v0.2 tests passed'
