# 0mk v0.2 state format

This document describes the on-disk state written by the current `0mk.h`.
It is an implementation specification, not a compatibility promise. The source
of truth is [`0mk.h`](../0mk.h), with task ABI `0mk-task-v2`.

All digests below are SHA-256 encoded as 64 lowercase hexadecimal characters.
Integers in records and manifests are decimal unless stated otherwise. Record
files contain one `KEY=VALUE` field per line; strings that may contain arbitrary
bytes are hexadecimal-encoded in fields ending in `_HEX`.

## Task identity

0mk frames each value as `<decimal byte length>:<value>;` and hashes the
concatenated frames. A task key contains, in this exact order:

1. `0mk-task-v2`, the target name, requested cache policy, profile name,
   executor cache identity, executor invocation fingerprint, and command-list
   digest;
2. each declared environment name and its value digest, in lexical name order;
3. each graph dependency, in rule order: its name, built identity, and artifact
   manifest identity, or `no-artifact` for an alias;
4. the literal `materialized-inputs`, followed by each snapshotted artifact
   input in order: its logical name and artifact manifest identity.

Artifact inputs comprise artifact-producing dependencies followed by the first
occurrence of each repository-relative executable found in an argument-vector
recipe. Such executables use a `tool:<path>` logical name. An artifact manifest
identity frames and hashes its kind (`file` or `tree`), content digest, decimal
size, and decimal mode.

The command-list digest starts with `0mk-command-list-v1`. For every recipe
line it includes the command kind and unexpanded source text, then every
expanded argument for an argument-vector command or the expanded command for a
compatibility-shell command. An environment value digest distinguishes a
missing variable from a present value; only names selected with `--env` are
included.

The task key does not directly include output content, the run ID, executor
plan or result metadata, a remote handle, or a cost estimate. An embedded
executor supplies the invocation fingerprint as an opaque callback result, so
it may deliberately make other pre-run invocation facts (including `--submit`)
identity-bearing.

## Artifact store

A file artifact records the SHA-256 of its bytes, byte count, and the low 12
mode bits (`st_mode & 07777`). Its bytes are stored at:

```text
objects/<first two digest characters>/<remaining 62 characters>
```

Stored blobs are regular, non-symlink files with mode `0444`; validation checks
that mode, rehashes their bytes, and compares their byte count with the
referring artifact or tree entry.

A tree artifact is a single, flat inventory of every descendant, sorted by its
generic relative path. It is not a recursively hashed Merkle tree. The root
directory mode is carried by the outer artifact manifest, and its size is the
sum of descendant regular-file sizes. Its serialized inventory is stored at
`trees/<digest>.txt`, mode `0444`, where the digest hashes these exact bytes:

```text
0MK_TREE_V1
F <path-hex> <mode> <size> <blob-digest>
D <path-hex> <mode> 0 -
```

Each `F` or `D` line ends with `\n`; an empty tree contains only the header.
Paths are nonempty, normalized, relative, unique, and contain no `..`
component. Symlinks and non-file/non-directory entries are rejected. Artifact
availability validation rehashes the inventory and checks every referenced
file blob.

## State root and layout

The state root defaults to `.0mk` beside the selected 0mkfile. A relative
`--state-dir` is resolved from that project root. 0mk rejects a symlink state
root, a state root that is the project directory or its ancestor, and an
unmarked nonempty directory.

The writer creates a regular, non-symlink file named `.0mk-state` containing:

```text
0MK_STATE_V1
```

The reader requires that as the sole line; it also accepts the same line
without a final newline.

The current namespaces are:

```text
.0mk-state                           format marker
objects/aa/bb...                     file blobs
trees/<tree-digest>.txt              flat tree inventories
receipts/<task-key>.txt              successful task receipts
targets/<sha256(target)>.txt         current per-target head
runs/<run-id>/<sha256(target)>.txt   per-invocation evidence
work/<task-key>/root/                private execution workspace
locks/                               object, tree, task, and target locks
cache.lock                           repository-wide cache lock
heads/                               reserved and currently unused
```

Namespaces are created lazily. Existing namespace paths and `cache.lock` are
rejected if their file type is unsafe.

## Receipts and heads

A receipt is `VERSION=2` and contains:

| Field | Meaning |
| --- | --- |
| `TASK_KEY` | Task identity; also the receipt filename. |
| `TARGET_HEX`, `PROFILE_HEX` | Target and selected profile. |
| `EXECUTOR_IDENTITY` | Cache-relevant invocation fingerprint. |
| `COMMAND_HEX`, `COMMAND_DIGEST` | Display command and identity-bearing command-list digest. |
| `ENVIRONMENT_DIGEST` | Digest of the ordered declared-environment pairs. |
| `CACHE_POLICY` | `off`, `declared`, or `hermetic`. |
| `ELAPSED_MS`, `EXIT_CODE` | Executor elapsed time and exit status. |
| `HANDLE_HEX` | Optional opaque executor handle. |
| `OUTPUT_PRESENT` | `0` or `1`, followed when present by `OUTPUT_*`. |
| `INPUT_COUNT` | Number of ordered `INPUT_<n>_*` entries. |
| `METADATA_COUNT` | Number of `METADATA_<n>_KEY_HEX`/`VALUE_HEX` pairs. |

Every present manifest uses `<prefix>KIND`, `DIGEST`, `SIZE`, and `MODE`.
Each input has `NAME_HEX`, `IDENTITY`, `ARTIFACT_PRESENT`, and, when present,
an `ARTIFACT_*` manifest. Receipt inputs are graph dependencies followed by
captured `tool:` inputs. Executor result metadata is retained; plan facts are
added with a `plan.` prefix, except the transient `why` fact.

Receipts are written only after successful execution. A target head is a
`VERSION=1` record containing `TARGET_HEX` and `TASK_KEY`. It is replaced after
a successful artifact or action, and also when a cached artifact is confirmed
current or restored. A head is accepted only when its target, filename hash,
and referenced receipt agree.

## Run records and nondeterminism

Every non-dry build invocation gets an opaque run ID. A recorded task outcome
uses a per-target `VERSION=1` run record containing:

```text
TARGET_HEX, TASK_KEY, STATUS, MESSAGE_HEX,
ELAPSED_MS, EXIT_CODE, HANDLE_HEX, OUTPUT_PRESENT,
optional OUTPUT_*, METADATA_COUNT, and indexed METADATA_* pairs
```

Current statuses written by v0.2 are `current`, `restored`, `failed`, `done`,
and `nondeterministic`. Run records are evidence, not resumable attempt state;
they do not contain the receipt's command, inputs, environment, or executor
identity.

Under `declared` or `hermetic`, when execution for an existing task key produces
a different full output manifest (kind, digest, size, or mode), 0mk imports
the candidate artifact, writes a `nondeterministic` run record, and fails the
task. That record contains the candidate `OUTPUT_*`, the old and new content
digests as `previous_output` and `new_output` metadata, and the fixed diagnostic
message. 0mk does not publish the candidate or replace the earlier receipt or
target head. A different mode can trigger this check even when the two content
digests match.

## Cache policies

- `off` disables reuse, but still snapshots inputs, computes a task key, and
  writes successful receipts. Each successful attempt is publishable, so 0mk
  does not compare repeat executions for nondeterminism under this policy.
- `declared` permits reuse based on declared inputs and environment plus the
  executor identities. The local executor fingerprints resolved command
  executables, but this policy does not claim that shared libraries, the host,
  undeclared variables, time, or network access are captured.
- `hermetic` is accepted only from an executor whose advertised guarantee is
  at least `hermetic`. 0mk records that claim and identity; it does not itself
  create or verify a hermetic sandbox.

The requested policy is part of the task key. Actions are never reused under
any policy.

## Retention and compatibility boundaries

`0mk cache gc` treats every valid historical receipt as a root, including its
output and input artifacts. It also roots every output recorded in the run
history, which retains a nondeterministic candidate. GC removes only
unreferenced blobs and tree inventories, then clears `work/`; it does not expire
receipts, heads, run records, or locks. Invalid receipt/run records or unsafe
cache entries make GC fail rather than guess.

The marker, tree, receipt, head, run-record, task-ABI, and command-list versions
are independent. v0.2 performs no state migration. An unsupported state marker
is refused; incompatible receipts cease to be reusable, and incompatible tree
inventories are corrupt. `cache verify` reports invalid records or unavailable
artifacts, while GC refuses invalid history that could otherwise lose a root.

Record replacement and blob creation use temporary files followed by rename,
but the format makes no cross-platform crash-durability guarantee. Tree
materialization stages a complete directory and swaps paths with a backup; it
is not promised as an atomic tree publication to concurrent readers. This
state is a local cache and provenance format, not a shared coordination
database or durable orchestration log.
