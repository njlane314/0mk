# rivet.h prototype

`rivet.h` is a compact C++17 prototype of a Unix-style, content-aware dependency
runner. It is inspired by Make's target/dependency surface but deliberately uses
a smaller language and different semantics.

```text
target <- dependencies... [@profile]
    recipe using $@, $<, and $^

alias <- dependencies...

action! <- dependencies... [@profile]
    always-run recipe
```

Normal rules produce exactly one regular file. The recipe writes `$@` into a
private workspace; Rivet hashes it, stores it in `.rivet/objects`, publishes it
atomically, and writes a receipt keyed by the complete task identity. Returning
inputs from A to B to A can therefore restore the earlier A result without
re-executing it. A workspace lock serialises mutating Rivet processes. Cache
keys include the recipe, profile identity, dependency names, and dependency
contents rather than modification times.

## Build

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic rivet.cpp -o rivet
./tests/run.sh
```

The top-level `Rivetfile` is the domain-shaped example; `tests/Rivetfile` is a
small runnable graph built from ordinary Unix utilities.

## Use

```sh
./rivet                   # build the first target
./rivet report.pdf
./rivet -n report.pdf     # dry-run
./rivet --why report.pdf  # explain cache decisions
./rivet -B report.pdf     # force execution
```

The default `@local` profile tokenises and executes an argument vector directly
without invoking a shell. Use the explicit `@shell` profile for pipelines,
redirection, or other shell syntax. In a shell recipe, automatic variables must
appear unquoted; Rivet inserts their shell-quoted values.

The example `@large` profile is registered as another local argv executor in
`rivet.cpp`. A real application can replace it with a callback that invokes
`cloud.h`, SSH, a container runtime, Slurm, a hardware test rig, or another
execution environment.

## Embed

The header also exposes the engine directly:

```cpp
rivet::engine graph;
graph.profile("large", my_remote_executor());

rivet::run_options options;
options.file = "Rivetfile";
options.targets = {"report.pdf"};
return graph.run(std::move(options));
```

An executor receives a stable task key, raw and expanded recipe forms, typed
inputs (`logical_name`, `local_path`, `digest`, availability), a private
workspace, and the required output path. During a dry-run, planning is deferred
when an upstream artifact does not exist yet. This keeps the graph language
independent of Burst, SSH, Slurm, containers, CI, or any other backend.

## Deliberate prototype limits

- POSIX-first, single-process, sequential scheduling;
- one output file and one recipe line per rule;
- regular-file inputs and outputs;
- content-only identity (Unix mode bits are not cached as artifact data);
- direct Unix process execution for `@local`, plus an explicit `@shell` profile;
- completed-task recovery only, not attachment to in-flight remote jobs;
- no cache garbage collector yet;
- no GNU Make variables, implicit rules, patterns, or directory artifacts.

Actions may be requested as roots but cannot be dependencies: their side
effects deliberately do not participate in artifact identity.

The executor interface and graph/cache semantics are the reusable part; the
shell executor is merely the zero-dependency demonstration backend.
