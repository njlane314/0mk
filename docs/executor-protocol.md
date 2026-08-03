# External executor protocol (`0mk-exec-v1`)

This document defines the Unix process boundary used by external execution
profiles. It is deliberately a small, synchronous request/response protocol:
the runner owns graph construction and artifact storage, while an executor owns
placement and command execution.

The words **MUST**, **MUST NOT**, **SHOULD**, and **MAY** are normative.

## Discovery and transport

A profile named `NAME` is resolved as the exact executable
`0mk-exec-NAME` through `PATH`. The protocol defines these operations:

```text
0mk-exec-NAME identity
0mk-exec-NAME plan
0mk-exec-NAME run
0mk-exec-NAME cancel
0mk-exec-NAME inspect
```

The current runner uses `identity`, `plan`, `run`, and `inspect`. `cancel` is
available to executor-side tooling that owns a durable handle.

Profile names cannot contain `/`. Implementations should restrict them to
letters, digits, `.`, `_`, and `-`. The executable receives no protocol
arguments after the subcommand.

Each invocation has one exchange:

1. The runner writes exactly one UTF-8 JSON object to standard input and closes
   it.
2. The executor performs the requested operation.
3. The executor writes exactly one UTF-8 JSON object followed by `\n` to
   standard output and exits.

Standard output is reserved for the response. Human logs and child-process
output belong on standard error. An executor MUST NOT print banners, progress
messages, or multiple JSON values to standard output.

Every request and response contains:

```json
{"protocol":"0mk-exec-v1"}
```

Requests also contain `operation`; it MUST equal the argv subcommand. The
duplication detects dispatch mistakes rather than selecting the operation.

Writers emit canonical JSON: UTF-8 without a BOM, no insignificant whitespace,
object keys sorted lexicographically, and no duplicate keys. Cache-relevant
integers that may exceed JSON's exactly representable range are decimal
strings. Receivers MUST reject duplicate keys and malformed required fields.
They MUST ignore unknown object members so v1 can gain additive diagnostics,
but MUST reject unknown enum values and command kinds. New required semantics
need a new protocol version.

The examples below are pretty-printed for readability. Bare hashes on the wire
are exactly 64 lowercase hexadecimal SHA-256 digits. User interfaces may render
the same value as `sha256:<hash>`.

## Common task object

`plan` and `run` carry the same task object. A complete example is:

```json
{
  "operation": "plan",
  "profile": "burst",
  "protocol": "0mk-exec-v1",
  "submit": false,
  "task": {
    "attempt": "",
    "cache_policy": "hermetic",
    "commands": [
      {
        "argv": [
          "./solve",
          "--input",
          "case.tar.zst",
          "--output",
          "/work/8d89/output/result.json"
        ],
        "kind": "argv",
        "raw": "./solve --input $< --output $@"
      }
    ],
    "environment": [
      {
        "digest": "0e897f7e35a58a3b9bda19f299f03e573e5d67a1a6ac0a7f6137bd7168f1f3f9",
        "name": "OMP_NUM_THREADS"
      }
    ],
    "inputs": [
      {
        "artifact": {
          "digest": "207a55933f1cb8f9dc807f988ee6fa4781e8149d1d76f14d8a747713cfac2bd9",
          "kind": "file",
          "mode": "420",
          "size": "184205"
        },
        "available": true,
        "name": "case.tar.zst",
        "path": "/work/8d89/root/case.tar.zst",
        "provisional": false
      }
    ],
    "key": "8d89eea6284e12d4b0b4a6ee39280e2ca50601c32d226fdce5bfc27339c733d4",
    "output": "/work/8d89/output/result.json",
    "source_root": "/checkout",
    "target": "result.json",
    "workspace": "/work/8d89",
    "working_directory": "/work/8d89/root"
  }
}
```

The fields are:

| Field | Contract |
| --- | --- |
| `attempt` | Per-run idempotency ID. It is empty only for side-effect-free top-level planning and is otherwise a unique bare SHA-256 value. |
| `key` | Cache identity, as a bare SHA-256 digest. |
| `target` | Normalized logical name of the one artifact produced by the rule. |
| `cache_policy` | `off`, `declared`, or `hermetic`; see below. |
| `commands` | Ordered commands. They run sequentially and stop at the first failure. |
| `environment` | Cache-relevant environment names and SHA-256 digests of their effective values. Values are not carried in the JSON request; they come from the executor's controlled launch environment or backend configuration. Entries are sorted by name. |
| `inputs` | Ordered logical inputs. Their order is significant. |
| `output` | Absolute host path at which a successful run must materialize the logical output; empty for an action with no artifact. |
| `workspace` | Absolute private attempt directory. Scratch files belong here. |
| `source_root` | Absolute root containing repository-owned tools and declared source material. |
| `working_directory` | Absolute directory in which commands begin. |

An input's `name` is its stable logical name; `path` is its local read-only path
materialized from the CAS snapshot. 0mk defers planning until upstream
manifests are known. The `available` and `provisional` fields are reserved for
other planners; `run` MUST reject an unavailable or provisional input.

An artifact has `kind` (`file` or `tree`), a content `digest`, byte `size`, and
Unix permission `mode`. `size` and `mode` are unsigned base-10 strings; for
example, mode `420` is octal `0644`, and `493` is octal `0755`. A tree digest
identifies its canonical content-addressed manifest and its size is the sum of regular-file
payload bytes.

A command is one of:

```json
{"argv":["tool","argument"],"kind":"argv","raw":"tool argument"}
{"command":"tool | filter > output","kind":"shell","raw":"tool | filter > $@"}
```

`argv` does not invoke a shell. `shell` means `/bin/sh -c COMMAND`; profiles do
not change parsing. New files should prefer an explicit argv command such as
`["sh","-c","...","sh",...]` when shell evaluation is intentional. `raw` is
diagnostic source text and MUST NOT be executed or used instead of the expanded
command.

The runner derives `key` from a canonical cache-key manifest under its task ABI.
The projection includes the target, ordered expanded commands, ordered logical
input manifests, declared environment digests, requested cache policy, and the
executor identity. `attempt` is deliberately excluded. Physical paths, `submit`, planning estimates,
and `available`/`provisional` are excluded. The v1 task ABI includes `raw` as
recipe provenance as well as the expanded command, although only the expanded
command is executed. An executor MUST treat the supplied key as opaque, stable,
and suitable as the provider's cache identity.

`attempt` is empty when `plan` is invoked for a side-effect-free top-level plan.
The planning phase of an actual run and its following `run` request carry the
same unique, nonempty bare SHA-256 value. The executor uses it to make
submission retry-safe. For `off`, the idempotency scope is
`(profile, task.key, task.attempt)`: retrying that tuple reattaches, while a
later run has a new attempt and executes again. Reusable policies may
additionally deduplicate completed work by `(profile, task.key)`.

## `identity`

`identity` is read-only and returns the cache-relevant execution fingerprint
for the profile.

Request:

```json
{
  "operation": "identity",
  "profile": "burst",
  "protocol": "0mk-exec-v1"
}
```

Response:

```json
{
  "cache_policy": "hermetic",
  "identity": "6ad200a64f44ce42f76e0d456d386178b210fcf1cdbb9c283f9fbf724e219e56",
  "protocol": "0mk-exec-v1"
}
```

`identity` MUST be stable for an equivalent execution environment and MUST
change whenever a cache-relevant part changes. Depending on the profile, that
normally includes the adapter implementation and configuration, executable and
shared-library closure, operating system or machine image, container digest,
toolchain, and sandbox policy. Mutable image tags, host names, and a label such
as `latest` are not adequate fingerprints.

`cache_policy` is the strongest guarantee the executor supports. The runner
MUST NOT request a stronger policy. Executors MUST reject a `plan` or `run`
whose task policy is stronger than their advertised guarantee.

## `plan`

`plan` is read-only. It MUST NOT allocate a billable machine, submit a job,
reserve a scarce resource, or execute a task command.

The request is the common request shown above with `operation:"plan"`.
`submit` records whether the user has given explicit approval, but it does not
authorize side effects during planning.

Response:

```json
{
  "estimated_cost_usd": 0.74,
  "facts": {
    "cpus": "32",
    "executor": "burst",
    "memory": "128GiB",
    "rate": "0.74USD/h"
  },
  "protocol": "0mk-exec-v1",
  "requires_submit": true,
  "warnings": []
}
```

`facts` is a string-to-string map intended for stable CLI and JSON event output.
`warnings` is an ordered array of human-readable strings.
`estimated_cost_usd` is a non-negative JSON number giving the executor's best
total estimate in US dollars. The field is omitted when the executor cannot
estimate. It is advisory, not a billing guarantee. Currency, rate, region, and
estimate assumptions belong in additive facts or warnings.

`requires_submit:true` means `run` would cause remote, billable, scarce,
irreversible, or otherwise explicitly approved work. The runner MUST refuse to
run it unless the user supplied `--submit`, and the executor MUST independently
make the same check. A plan cannot grant its own approval. If placement or cost
changes between `plan` and `run`, the executor must recompute the decision and
may still refuse execution.

## `run`

The request is the common request with `operation:"run"`. `submit:true` means
the user explicitly approved submission for this invocation. It is not an
authentication credential and does not waive provider-side budget or policy
limits.

A successful protocol response is:

```json
{
  "exit_code": 0,
  "facts": {
    "exit_code": "0",
    "provider_job_id": "job-01J8J2TZG4N7DM5CZQ",
    "state": "completed"
  },
  "handle": "burst:job-01J8J2TZG4N7DM5CZQ",
  "metadata": {
    "image": "ghcr.io/acme/solver@sha256:8bbd...",
    "machine": "m6i.4xlarge",
    "provider_job_id": "job-01J8J2TZG4N7DM5CZQ",
    "region": "eu-west-2"
  },
  "protocol": "0mk-exec-v1"
}
```

`run` is synchronous in v1. The executor process MUST remain attached until the
attempt reaches a terminal state. `exit_code` is the task command's exit status,
not the executor process status. On `exit_code:0`, the complete artifact MUST be
present at `task.output` as a regular file or directory tree before the response is written.
The executor MUST NOT report zero for a missing, partial, or symlink-substituted
output. For an action, the empty output path has no artifact requirement.

Child output goes to the executor's standard error. Commands run in order and
the first nonzero command status becomes `exit_code`; later commands do not run.
Signal termination uses the usual `128 + signal` convention when it can be
represented. The runner alone imports and publishes a successful artifact into
the CAS.

`metadata` is a string-to-string provenance map and is persisted in the receipt.
Executors should include provider job ID, immutable image, machine type, region,
and actual cost when available. Metadata is descriptive and never contributes
retroactively to the task key.

Repeated `run` requests with the same profile, task key, and attempt MUST be
idempotent: the executor reattaches to or reports the existing attempt instead
of submitting a second job. Under `declared` and `hermetic`, an executor MAY
also return completed work for the same task key from an earlier attempt. Under
`off`, it MUST NOT do so.

## Durable handles, `inspect`, and `cancel`

`handle` is an opaque, executor-owned durable attempt identifier. It MUST remain
valid across executor processes and SHOULD remain inspectable at least as long
as the corresponding receipt. It is provenance, not part of the cache key. An
executor that has no durable backend returns the empty string.

Callers MUST store and pass a handle verbatim. They must not infer provider,
region, or job ID from its text.

Inspect request:

```json
{
  "handle": "burst:job-01J8J2TZG4N7DM5CZQ",
  "operation": "inspect",
  "profile": "burst",
  "protocol": "0mk-exec-v1"
}
```

Example response:

```json
{
  "exit_code": 0,
  "handle": "burst:job-01J8J2TZG4N7DM5CZQ",
  "metadata": {
    "provider_job_id": "job-01J8J2TZG4N7DM5CZQ"
  },
  "protocol": "0mk-exec-v1",
  "state": "completed"
}
```

`state` is `queued`, `running`, `completed`, `failed`, `cancelled`, or `unknown`.
`exit_code` is present only when known. `facts` is the normative flattened
string-to-string projection for CLI display. It MUST include `state`, MUST
include `exit_code` when known, and SHOULD include useful provider provenance.
The structured fields remain authoritative for machine processing. `inspect`
is read-only and idempotent.

Cancel request:

```json
{
  "handle": "burst:job-01J8J2TZG4N7DM5CZQ",
  "operation": "cancel",
  "profile": "burst",
  "protocol": "0mk-exec-v1"
}
```

Example response:

```json
{
  "cancelled": true,
  "handle": "burst:job-01J8J2TZG4N7DM5CZQ",
  "protocol": "0mk-exec-v1",
  "state": "cancelled"
}
```

Cancellation is idempotent. Cancelling an already terminal attempt returns its
terminal state with `cancelled:false`; an unknown handle returns
`state:"unknown"`. A successful cancellation means the executor requested and
confirmed cancellation as far as its provider permits. It does not imply that a
partially written workspace output is publishable.

Because `run` has one terminal response, v1 does not return an intermediate
"submitted" response. Backends persist the `(profile, task.key, task.attempt)`
mapping before submission and use it to recover a handle after a runner restart.

## Cache contract

The policies are ordered `off < declared < hermetic`:

- `off`: do not reuse completed results. Receipts and provenance may still be
  recorded.
- `declared`: the command, snapshotted inputs, declared environment digests, and
  executor identity are trusted as the complete cache inputs. Undeclared host
  state is possible, so this is the normal local-execution policy.
- `hermetic`: the executor guarantees that the task cannot observe undeclared
  inputs and that its identity completely describes the execution environment.
  A digest-pinned container with isolated mounts is a typical implementation.

Inputs MUST be imported into immutable snapshots before `run`. Executors MUST
not substitute the live project path for an input snapshot. They SHOULD expose
snapshots read-only. `artifact.mode` records the original artifact mode; a
read-only materialization may clear its write bits while preserving all other
bits. The output path is the only artifact publication channel.

Under `declared` or `hermetic`, if the same task key completes with two different
output manifests, the runner reports nondeterminism and preserves both pieces
of evidence; it MUST NOT silently replace the earlier cache object. `off` tasks
publish each successful attempt without that comparison. An executor must not
add random attempt IDs or timestamps to a reusable cache identity.

## Failures and exit rules

A normal executor process exits `0` after writing a schema-valid response. This
is true even when a `run` response contains a nonzero task `exit_code`.

An executor process uses these statuses only for protocol or adapter failures:

| Status | Meaning |
| --- | --- |
| `64` | malformed request, unsupported operation/value, or argv/JSON operation mismatch |
| `69` | configured backend is unavailable |
| `70` | internal executor failure |
| `75` | temporary adapter/provider failure; retry may succeed |
| `77` | explicit submission approval is required |

When possible, a nonzero process also writes one response of this form before
exiting:

```json
{
  "error": {
    "code": "approval_required",
    "message": "profile burst requires submit approval",
    "retryable": false
  },
  "protocol": "0mk-exec-v1"
}
```

The runner treats these statuses as adapter outcomes, not task exit statuses.
Status `77` becomes 0mk's stable approval-required status `3`; the others are
task failures. A process killed by a signal may be unable to produce JSON.

## Security requirements

External executors are trusted code with the same host privileges as the runner
unless separately sandboxed. Implementations and callers should therefore:

- resolve only the exact `0mk-exec-NAME` through a controlled `PATH`; never
  derive a shell command from the profile name;
- treat all JSON strings, paths, handles, metadata, and provider output as
  untrusted data and apply size/depth limits;
- invoke `argv` commands without a shell and pass handles as arguments, never by
  interpolation;
- reject output paths outside the declared workspace and reject symlink outputs;
- stage inputs from their immutable snapshot paths, validate their kind, reject
  symlinks, verify content when the available manifest permits it, and prevent
  task commands from modifying them;
- keep credentials out of manifests, identity responses, metadata, logs, and
  handles; `environment[].digest` is an identity, not a secret transport;
- store durable handle mappings and provider credentials with restrictive
  permissions;
- make `plan`, `identity`, and `inspect` free of submission side effects; and
- enforce provider budgets and `submit` again inside `run`, even if the caller
  previously planned the task.

The `hermetic` label is a correctness claim, not a security boundary. It should
be advertised only when the executor actually confines filesystem, network,
clock, randomness, and environment access to its declared contract.
