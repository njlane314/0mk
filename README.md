# 0mk

[![Build](https://github.com/njlane314/0mk/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/njlane314/0mk/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C)

`0mk` is the small, auditable, content-addressed runner for file-producing
technical pipelines.

It keeps the useful shape of Make while giving coarse tasks stronger semantics:
inputs are immutable snapshots, one rule produces one file or directory tree,
profiles choose where work runs, and receipts record exactly why an artifact is
current. There is no daemon, hosted service, or provider SDK; commands reach a
shell only when requested explicitly.

## INSTALL

Requires a C++17 compiler, CMake, and a POSIX system.

From the source checkout:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
cmake --install build --prefix ~/.local
~/.local/bin/0mk --version
```

This installs `0mk` in `~/.local/bin`; ensure that directory is on `PATH`.
It also installs `0mk.h`, the CMake package, and the protocol specifications.

## USE

The checked-in `0mkfile` is a complete local pipeline:

```text
all <- out/report.txt

out/readme-lines.txt <- README.md
    sh -c 'wc -l < "$1" | tr -d " " > "$2"' sh $< $@

out/report.txt <- out/readme-lines.txt
    sh -c 'printf "0mk repository report\nREADME lines: " > "$2"; cat "$1" >> "$2"' sh $< $@
```

Build and inspect it:

```sh
0mk plan out/report.txt
0mk out/report.txt
0mk why out/report.txt
0mk inspect out/report.txt
```

With a profile backed by an executor such as `0mk-exec-large`:

```text
result.json <- case.tar.zst @large
    ./solve --input $< --output $@
```

```sh
0mk plan result.json
0mk --submit result.json
```

`plan` requests are specified as read-only, and executors must honor that
contract. If an executor reports cost-bearing or externally submitted work,
execution requires explicit `--submit` approval.

## RULES

```text
target <- dependencies... [@profile]
    command using $@, $<, and $^

alias <- dependencies...

action! <- dependencies... [@profile]
    always-run command
```

`$@` is the private output path, `$<` is the first artifact input, and `$^`
expands to every artifact input. New recipes are parsed as argument vectors, not
passed to a shell. Invoke `sh -c` explicitly for redirection, pipelines, or
expansion. The deprecated `@shell` profile remains only for older graphs.

A rule produces one logical file or tree artifact. An alias groups dependencies.
An action always runs and cannot be a dependency; it is intended for absolute
or external side effects because relative files in its private workspace are
discarded.

## EXECUTORS

`@local` runs commands directly. Any other profile is discovered through
`PATH`: `@large` resolves to `0mk-exec-large`. Embedding applications can
register a C++ executor callback instead.

External executors use the same task manifests and artifact semantics as local
execution. The runner uses a versioned JSON protocol for environment identity,
read-only planning, execution, and inspection; the protocol also defines durable
handles and cancellation for executor-side tooling. If an executor marks work
as requiring submission, `0mk` refuses it without `--submit`, and the protocol
requires the executor to recheck that approval. See
[`docs/executor-protocol.md`](docs/executor-protocol.md).

## FILES

```text
project/
├── 0mkfile                    authored graph
├── source and output files
└── .0mk/                      local state
    ├── objects/ and trees/    content-addressed artifacts
    ├── receipts/ and targets/ provenance and current heads
    └── runs/                  per-invocation evidence
```

Before execution, declared inputs and repository-relative tools are imported
into the store and materialized read-only. Modes are preserved. `off` always
runs, `declared` trusts declared inputs and the executor fingerprint, and
`hermetic` requires the executor to attest to a complete environment identity.
Under a reusable policy, two outputs for one task key are reported as
nondeterminism rather than silently replacing evidence.

Use `--state-dir PATH` to relocate state, and `0mk cache verify|du|gc` to inspect
it. The exact task identity, receipt fields, flat tree format, and retention
rules are specified in [`docs/state-format.md`](docs/state-format.md).

## EMBED

```cpp
#include <0mk.h>

int main() {
    mk0::engine engine;
    mk0::run_options options;
    options.targets = {"all"};
    return engine.run(std::move(options));
}
```

CMake consumers use `find_package(0mk CONFIG REQUIRED)` and link `0mk::0mk`.
The C++ namespace is `mk0` because an identifier cannot begin with a digit.

## CHECKS

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

CI runs the complete suite with GCC and Clang on Linux and Apple Clang on macOS.

## LICENSE

No license has been selected yet. Redistribution or reuse requires an explicit
license choice.
