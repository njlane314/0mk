# Scientific pipeline shape

This graph illustrates a mesh → simulation → analysis → publication workflow.
It is intentionally not the repository's runnable quick start: the inputs and
domain programs belong to the application using 0mk.

To run it, provide:

- `geometry.mesh` and `parameters.toml` as source inputs;
- `pack INPUT... --output OUTPUT`, which creates `case.tar.zst`;
- `solve --input INPUT --output OUTPUT`, which creates `result.json`;
- `report INPUT --output OUTPUT`, which creates `report.pdf`;
- `publish INPUT`, an optional side-effecting action;
- a `large` profile, either as `0mk-exec-large` on `PATH` or a callback
  registered by an embedding application.

The profile is expected to describe placement and policy. It does not change
how recipe arguments are parsed, and expensive execution still requires the
caller's explicit submission approval.
