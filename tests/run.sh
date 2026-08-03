#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-$root/tests}/rivet-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

c++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror \
    "$root/rivet.cpp" -o "$tmp/rivet"
cp "$root/tests/Rivetfile" "$root/tests/input.txt" "$tmp/"

cd "$tmp"
./rivet -n all > initial-dry.out
test ! -e .rivet
test ! -e upper.txt
grep -q '^report.txt <- @shell plan=deferred$' initial-dry.out
grep -q '^joined.txt <- @local plan=deferred$' initial-dry.out

./rivet all
test "$(cat report.txt)" = "HELLO RIVET!"
cmp input.txt copy.txt
test "$(wc -l < joined.txt)" -eq 2

./rivet --why all > current.out
grep -q '^upper.txt: current$' current.out
grep -q '^copy.txt: current$' current.out

rm copy.txt
./rivet -n --why copy.txt > dry.out
test ! -e copy.txt
grep -q '^copy.txt: would restore from cache$' dry.out
./rivet copy.txt
cmp input.txt copy.txt

./rivet check
./rivet check

printf '%s\n' 'hello changed rivet' > input.txt
./rivet all
test "$(cat report.txt)" = "HELLO CHANGED RIVET!"

# Returning to an earlier input identity restores historical cached artifacts.
printf '%s\n' 'hello rivet' > input.txt
./rivet --why all > restored.out
grep -q '^upper.txt: restored from cache$' restored.out
test "$(cat report.txt)" = "HELLO RIVET!"

# A corrupt CAS object is detected and replaced by a fresh execution.
copy_hash=$(sha256sum copy.txt | awk '{print $1}')
copy_object=".rivet/objects/$(printf '%s' "$copy_hash" | cut -c1-2)/$(printf '%s' "$copy_hash" | cut -c3-)"
printf '%s\n' corrupt > "$copy_object"
rm copy.txt
./rivet copy.txt
cmp input.txt copy.txt

# A failed recipe may write its private $@, but cannot replace a valid target.
cp "$root/tests/Failurefile" .
printf '%s\n' original > stable.txt
set +e
./rivet -f Failurefile stable.txt > failure.out 2>&1
failure_status=$?
set -e
test "$failure_status" -eq 1
test "$(cat stable.txt)" = original

set +e
./rivet -f "$root/tests/Cyclefile" a > cycle.out 2>&1
cycle_status=$?
./rivet -f "$root/tests/Badfile" > bad.out 2>&1
bad_status=$?
./rivet -f "$root/tests/Collisionfile" > collision.out 2>&1
collision_status=$?
set -e
test "$cycle_status" -eq 2
test "$bad_status" -eq 2
test "$collision_status" -eq 2
grep -q 'Dependency cycle: a -> b -> a' cycle.out
grep -q 'Badfile:1' bad.out
grep -q "target path is not normalized; use 'a'" collision.out

printf '%s\n' 'rivet tests passed'
