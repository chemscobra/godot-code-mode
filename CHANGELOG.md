# Changelog

Release notes for the 0.3.x series. Earlier releases are available through the
repository's Git tags. Versions below refer to the addon and MCP server, not
the MCP protocol or Godot engine version.

## 0.3.2

### Compact responses and helper operations

- `execute` responses are compact by default: empty `diagnostics`/`changes`/`effects` are omitted and writes are reported as grouped counts (`{"Node3D.set": 24}`) with `status: committed|rolled_back`. `verbose: true` restores the ordered per-op log; external effects remain visible in both modes.
- `godot.build({type, name, props, children})` constructs whole subtrees in one transaction with automatic owner assignment. `godot.scene.new/reload/tree/run/stop` cover scene creation, reload, inspection and playback.
- `godot.attachScript(node, 'res://x.gd')` assigns scripts through a trusted, undoable editor operation; `Script` and meta-methods stay denied.
- `godot.permissions({class, member})` reports `allowed`, `reason` and `unlock` for every gate (denylist, tiers, restrict, meta-methods).

### Permission-layer unblocks

- `const`-flagged native methods count as reads in `read` mode instead of falling back to name-prefix heuristics.
- Journaled inverses added for transform mutators, `show`/`hide`, `add_to_group`/`remove_from_group`, `move_child`, `add_sibling` and focus calls.
- `EditorInterface` allowlist widened (`get_open_scenes`, `get_resource_filesystem`, `get_editor_paths`, `get_script_editor`, `get_selected_paths`); returned classes stay protected by their own tiers.
- `set_x(v)` routes through the journaled `set` path so both spellings behave identically (fixes the `material_override` mismatch).
- Packed arrays encode as tagged values with `values` preserving round-trip types; `StringName`/`NodePath` keep explicit tags.

## 0.3.1

### Performance

- Reuse native/script descriptors and immutable catalog snapshots rather than
  performing project-wide rediscovery per stable native batch element.
- Invalidate/reconcile metadata for script replacement/reload, filesystem and
  native-extension changes. Check current authorization and reference validity
  independently of cached descriptors.
- Construct value pages incrementally without re-encoding accumulated prefixes,
  preserving exact UTF-8 limits, order, continuation and retention rollback.
- Reuse already decoded passive worker results where transport-equivalent;
  retain the existing JSON fallback for incompatible representations.

### Measurement and tests

- Add opt-in `GODOT_CM_PROFILE=1` aggregate cost instrumentation with no new MCP
  tools/response fields or private argument/result logging.
- Add isolated baseline extraction, binary hashes, raw samples, warmups, median,
  p95, alternating comparisons, bounded profile reading and nineteen harness
  tests. Keep failed/inconclusive samples visible.
- Verify script and native-extension reloads, request-pinned snapshots and
  catalog/cache lifetimes over repeated invalidation cycles.
- Document a 21,600-sample primary comparison against 0.3.0. Minimal-topology
  medians change from 263.4 to 6.8 ms for 100-read batches, 1294.6 to 20.9 ms
  for 500-read batches, and 14.0 to 6.7 ms for 256-element pages. These are
  workload-specific results, not universal performance guarantees.

### Trade-offs and compatibility

- Cold direct startup regresses from 704.5 to 798 ms median in the ten-start
  series. Conservative catalog reconciliation can slow unknown-effect/script
  calls. Small pages, idle CPU and single operations have no general speedup
  claim; worker improvements are modest and some p95 results are mixed.
- Preserve permission policy, type validation, quotas, diagnostics, checkpoints,
  rollback and execution isolation. Batches remain ordered single main-thread
  dispatches; native calls cannot be forcibly interrupted.
- Rebuild Linux and Windows x86-64 editor binaries. Linux integration, TypeScript
  and strict OpenSpec validation pass. Windows was cross-built, not run.
- See [benchmark evidence and reproduction commands](docs/benchmarks.md).
  The matrices measured the optimized candidate before its version-label bump;
  the 0.3.1 rebuild has separate hashes and release verification there.

## 0.3.0

### Data reliability

- Cover all 39 concrete Godot Variant types with documented transport schemas,
  strict contextual conversion, explicit tags and validated opaque references.
- Accept named mathematical components for declared destinations, including
  vectors, rotations, transforms and colors. Preserve objects as dictionaries
  for Variant/untyped destinations; RGB accepts omitted alpha as 1.
- Preserve int64/BigInt precision, special scalar floats, StringName/NodePath
  identity and typed collection restrictions. Reject invalid components before
  assignment or invocation rather than reporting false success.
- Decode JavaScript data passively, rejecting accessors, active formatters,
  proxies, cycles and other unsupported values without running their hooks.
- Support bounded inline collections, retained-value inspection/pagination,
  explicit release and service-generation invalidation with quotas.

### Diagnostics and checkpoints

- Preserve original error codes and available operation, member, input-path and
  JavaScript source context within bounded valid responses.
- Add explicit synchronous `checkpoint(name, value)` snapshots, retained across
  later failures and kept separate from final results and untracked effects.
- Reserve worker-local `Error.prepareStackTrace` and `Error.stackTraceLimit`
  hooks so diagnostic creation does not execute user formatters.
- Update MCP help and generated TypeScript declarations; extend isolated tests
  for permissions, constructors, batches, Undo/Redo, rollback and quotas.

### Known limitations

- The initial five-sample historical comparison showed a batch-read regression
  against 0.2.1, motivating the measured optimizations in 0.3.1. That historical
  API comparison is not equivalent for every case; see the benchmark caveats.
- Checkpoints do not recover unreturned JavaScript locals or guarantee rollback
  of untracked effects. A dispatched native/plugin call is not proof of domain
  success, and cancellation cannot preempt a running native method.
- Linux integration and both native builds passed; Windows runtime remained
  unverified.
