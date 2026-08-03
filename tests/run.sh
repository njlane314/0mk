#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_tmp_base=${TMPDIR:-/tmp}
tmp=$(mktemp -d "$test_tmp_base/0mk-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

fail() {
    printf 'test failure: %s\n' "$*" >&2
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

assert_line() {
    grep -Fqx -- "$2" "$1" || fail "missing line in $1: $2"
}

assert_text() {
    grep -Fq -- "$2" "$1" || fail "missing text in $1: $2"
}

assert_event() {
    grep -Eq "^$2[[:space:]]+$3([[:space:]]|$)" "$1" ||
        fail "missing $2 event for $3 in $1"
}

sha256_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 "$1" | sed 's/^.*= //'
    else
        fail 'need sha256sum, shasum, or openssl for cache-corruption tests'
    fi
}

source_state_was_absent=false
if test ! -e "$root/tests/.0mk"; then
    source_state_was_absent=true
fi

test_cxx=${CXX:-c++}
"$test_cxx" ${CPPFLAGS:-} ${CXXFLAGS:-} \
    -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
    "$root/0mk.cpp" -o "$tmp/0mk"

project="$tmp/project with spaces"
mkdir -p "$project"
cp "$root/tests/0mkfile" "$root/tests/input.txt" \
    "$root/tests/Badfile" "$root/tests/Collisionfile" \
    "$root/tests/Cyclefile" "$root/tests/Failurefile" "$project/"
cp "$root"/tests/fixtures/*file "$project/"
mkdir -p "$project/fixtures"
cp -R "$root/tests/fixtures/bin" "$project/fixtures/"

cd "$project"

# CLI queries do not require a 0mkfile and do not create state.
../0mk --help > help.out
../0mk -h > short-help.out
cmp help.out short-help.out
assert_text help.out 'usage: 0mk'
assert_text help.out '--dry-run'
assert_text help.out '--force'
assert_text help.out '--why'
../0mk --version > version.out
grep -Eq '^0mk [0-9]+\.[0-9]+\.[0-9]+$' version.out ||
    fail 'version output is not a semantic version'
test ! -e .0mk

expect_status 2 ../0mk --definitely-not-an-option > unknown-option.out 2>&1
assert_text unknown-option.out 'Unknown option: --definitely-not-an-option'
expect_status 2 ../0mk -f > missing-file-option.out 2>&1
assert_text missing-file-option.out '-f requires a path'

# A dry run plans the complete graph without creating state or outputs.
../0mk -n all > initial-dry.out
test ! -e .0mk
test ! -e upper.txt
assert_line initial-dry.out \
    'deferred  report.txt   @shell  waiting=upper.txt  waiting for dependencies'
assert_line initial-dry.out \
    'deferred  joined.txt   @local  waiting=upper.txt  waiting for dependencies'

# Build, current checks, restoration, and always-run actions.
../0mk all
test "$(cat report.txt)" = 'HELLO 0MK!'
cmp input.txt copy.txt
test "$(wc -l < joined.txt)" -eq 2

../0mk --why all > current.out
assert_line current.out \
    'current   upper.txt   @shell  task identity and output match receipt  sha256:431d45a7281c'
assert_line current.out \
    'current   copy.txt   @local  task identity and output match receipt  sha256:34f031cced7f'

rm copy.txt
../0mk -n --why copy.txt > dry.out
test ! -e copy.txt
assert_line dry.out \
    'restored  copy.txt   @local  would restore from cache  sha256:34f031cced7f'
../0mk copy.txt
cmp input.txt copy.txt

../0mk check
../0mk check

# A malformed receipt is never trusted and is replaced after fresh execution.
copy_receipt=
copy_receipt_count=0
for candidate in .0mk/receipts/*.txt; do
    if grep -Fqx 'TARGET_HEX=636f70792e747874' "$candidate"; then
        copy_receipt=$candidate
        copy_receipt_count=$((copy_receipt_count + 1))
    fi
done
test "$copy_receipt_count" -eq 1 || fail 'expected exactly one receipt for copy.txt'
sed 's/^VERSION=.*/VERSION=invalid/' "$copy_receipt" > "$copy_receipt.corrupt"
mv "$copy_receipt.corrupt" "$copy_receipt"
rm copy.txt
../0mk --why copy.txt > corrupt-receipt.out
assert_line corrupt-receipt.out \
    'run       copy.txt   @local  executor=local  why=invalid receipt'
assert_event corrupt-receipt.out done copy.txt
assert_text corrupt-receipt.out 'sha256:34f031cced7f'
cmp input.txt copy.txt
if grep -Fqx 'VERSION=invalid' "$copy_receipt"; then
    fail 'fresh execution did not replace the corrupt receipt'
fi

printf '%s\n' 'hello changed 0mk' > input.txt
../0mk --why all > changed.out
assert_text changed.out \
    'run       upper.txt   @shell  executor=shell-compat  why=input.txt changed (previous sha256:'
assert_text changed.out \
    'run       copy.txt   @local  executor=local  why=input.txt changed (previous sha256:'
test "$(cat report.txt)" = 'HELLO CHANGED 0MK!'

# Returning to an earlier identity restores historical cached artifacts.
printf '%s\n' 'hello 0mk' > input.txt
../0mk --why all > restored.out
assert_line restored.out \
    'restored  upper.txt   @shell  restored from cache  sha256:431d45a7281c'
test "$(cat report.txt)" = 'HELLO 0MK!'
../0mk inspect copy.txt > reactivated-inspect.out
assert_text reactivated-inspect.out \
    'output=sha256:34f031cced7f4b9406cd86eba6b8e10c15dfe6ff32c5d99d0466f2820de77c75'

# A corrupt CAS object is detected and replaced by a fresh execution.
copy_hash=$(sha256_file copy.txt)
copy_object=".0mk/objects/$(printf '%s' "$copy_hash" | cut -c1-2)/$(printf '%s' "$copy_hash" | cut -c3-)"
chmod u+w "$copy_object"
printf '%s\n' corrupt > "$copy_object"
rm copy.txt
../0mk copy.txt
cmp input.txt copy.txt
test "$(sha256_file "$copy_object")" = "$copy_hash"

# Quoted rule paths and automatic variables remain individual argv values.
mkdir -p 'input dir'
printf '%s\n' alpha > 'input dir/source file.txt'
printf '%s\n' beta > 'input dir/second file.txt'
../0mk -f Pathsfile all
cmp 'input dir/source file.txt' 'output dir/report file.txt'
test "$(sed -n '1p' shell-vars.txt)" = 'input dir/source file.txt'
test "$(sed -n '2p' shell-vars.txt)" = 'input dir/source file.txt'
test "$(sed -n '3p' shell-vars.txt)" = 'input dir/second file.txt'
test "$(cat dollar.txt)" = '$HOME'

# Symlink artifacts are rejected without replacing a valid target.
printf '%s\n' original > symlink.txt
expect_status 1 ../0mk -f InvalidOutputsfile -B symlink.txt > symlink.out 2>&1
assert_text symlink.out "succeeded but did not create a file or tree artifact at \$@"
test "$(cat symlink.txt)" = original

# A directory is one logical tree artifact and can replace a file transactionally.
printf '%s\n' original > directory.txt
../0mk -f InvalidOutputsfile -B directory.txt
test -d directory.txt
rmdir directory.txt
../0mk -f InvalidOutputsfile directory.txt
test -d directory.txt

# A logical artifact can change kind even when the replaced tree is read-only.
../0mk -f TreeTargetfile kind-target
test -d kind-target
test "$(cat kind-target/value.txt)" = "$(cat input.txt)"
../0mk -f FileTargetfile kind-target
test -f kind-target
test "$(cat kind-target)" = "$(cat input.txt)"
../0mk -f TreeTargetfile kind-target
test -d kind-target
test "$(cat kind-target/value.txt)" = "$(cat input.txt)"
chmod -R u+rwx kind-target

# Target parents may not redirect publication through symlinks.
mkdir external-output
ln -s external-output linked
expect_status 2 ../0mk -f InvalidOutputsfile linked/output.txt > linked.out 2>&1
assert_text linked.out 'Artifact target parent may not be a symlink'
test ! -e external-output/output.txt

# Failed recipes cannot replace a valid target.
printf '%s\n' original > stable.txt
expect_status 1 ../0mk -f Failurefile stable.txt > failure.out 2>&1
test "$(cat stable.txt)" = original

# Parse and graph errors are usage failures with source-local diagnostics.
expect_status 2 ../0mk -f Cyclefile a > cycle.out 2>&1
expect_status 2 ../0mk -f Badfile > bad.out 2>&1
expect_status 2 ../0mk -f Collisionfile > collision.out 2>&1
expect_status 2 ../0mk -f Overlappingfile > overlapping.out 2>&1
expect_status 2 ../0mk -f InputOverlapfile > input-overlap.out 2>&1
expect_status 2 ../0mk -f ActionInputOverlapfile > action-input-overlap.out 2>&1
expect_status 2 ../0mk -f TargetInputOverlapfile > target-input-overlap.out 2>&1
assert_text cycle.out 'Dependency cycle: a -> b -> a'
assert_text bad.out 'Badfile:1'
assert_text collision.out "target path is not normalized; use 'a'"
assert_text overlapping.out 'artifact targets may not overlap: out and out/report.txt'
assert_text input-overlap.out 'artifact inputs may not overlap: data and data/value.txt'
assert_text action-input-overlap.out 'artifact inputs may not overlap: data and data/value.txt'
assert_text target-input-overlap.out 'artifact target and input may not overlap: data/result.txt and data'

if test "$source_state_was_absent" = true && test -e "$root/tests/.0mk"; then
    fail 'test run wrote .0mk under tests/'
fi
printf '%s\n' '0mk tests passed'
