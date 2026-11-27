# Changelog

Release notes for the 0.3.x series. Earlier releases are available through the
repository's Git tags. Versions below refer to the addon and MCP server, not
the MCP protocol or Godot engine version.

## 0.4.0

### Unified permission groups and a discoverable dock

- `allowlist.json` gains a unified `groups` schema: every sensitive surface is a named class set with one policy — `deny` (blocked entirely), `write` (reads only), `ask` (prompt via `godot.requestTier`), `allow` (silent grant), `off` (dormant). `protected` groups (execution, process, interop) can never become prompt-unlockable, including via the dock.
- Legacy keys (`tiers`, `tier_policy`, `unlocked_tiers`, `auto_grant_tiers`, `deny_classes`, `deny_write_classes`, `deny_groups`) still load; old policy files keep working unchanged.
- Audited deny list: `ProjectSettings`/`EditorSettings`/`TextServer*` move to `write` groups (reads allowed); `Mutex`/`Semaphore` dropped; `Crypto*` → `crypto` group and capture devices → `capture` group, both `ask` (promptable instead of impossible).
- Every `PERMISSION` diagnostic carries a `remediation` string: `call godot.requestTier('<group>')`, `use godot.createScript / godot.attachScript` for Script-family denials, or `"denied permanently by policy (group '<name>')"`.
- `godot.requestTier()` gets a dedicated ~120s deadline independent of execute mode — a read-mode request no longer dies on the 5s default while the dialog is open.
- Dock rebuild: the permissions section shows the whole model — policy file row (path, "Open file", reload status), one row per group with a state combo and live lock indicator, a read-only "other restrictions" row, and the auto-approve checkbox (now scoped to `ask` groups).
- Dock combo edits persist to `allowlist.json` via `save_group_policy` (writes only `groups.<name>.policy`; class lists stay file-authoritative); `policy_reloaded` signal rebuilds the panel so file edits appear instantly. Hot-reload applies file-wins semantics only to entries whose policy actually changed — unrelated edits no longer wipe session grants. Consecutive dock saves merge against the on-disk file, and `tier_policy` is re-seeded eagerly on reload so the panel never paints stale states.
- Group rows show the policy file's `_why` rationale as a `ⓘ` tooltip; each row has a `↺` reset-to-default button fed by the shipped `allowlist.default.json`, plus a global "Reset defaults" that restores only policies (class lists and custom groups are kept).
- "Allow all" trust mode behind a confirmation dialog flips every non-protected group to `allow`; an explicit "sandbox OFF" checkbox extends it to protected groups (arbitrary code execution — logged as `SANDBOX OFF`).
- The manual test harness is now pytest: `tools/bridge_harness.py` holds the fixture/Client/stage functions, `tools/test_bridge/` has ordered functional tests (`pytest tools/test_bridge`, `-k` for granular selection); `tools/test_generic_bridge.py` remains the entry point for the pytest run and the legacy inventory/benchmark/catalog-lifecycle modes.
- Linux integration suite and both native builds pass; Windows was cross-built, not run.

## 0.3.3

### Reduced authoring friction

- `allowlist.json` hot-reloads while the editor runs: edits apply within ~100ms via the bridge pump, session tier grants survive, and a malformed mid-save file keeps the prior policy. Fixes policy edits silently requiring a plugin restart.
- `godot.createScript(path, source, {overwrite?})` writes non-`@tool` `.gd` files through a trusted, undoable `editor.create_script` operation (rollback deletes created files, restores overwritten bytes). `Script`/`GDScript` stay denied.
- `api.helpers()` returns signatures, modes, tiers, descriptions and examples for every JS helper (`build`, `scene.*`, `pluginRead/Call/Action`, `attachScript`, `createScript`, `values.*`, `batch`, `requestTier`, `permissions`, `checkpoint`) without reading files on disk.
- Returned `Array`/`Dictionary` values unwrap to native JS arrays/objects inside `execute`; `get_children()` and friends are directly iterable. Packed, typed and reserved-key collections keep their tagged form.
- `initialize` results include `sessionId` in the body for HTTP clients that ignore headers; the `MCP-Session-Id` header is unchanged. The `execute` tool description now documents that external actions need their own call.
- `effects` collapses to `{counts, total}` unless `verbose: true` — large `godot.build` results no longer emit ~100 per-op entries.
- `godot.permissions()` serializes correctly instead of failing with `INVALID_VALUE_ENCODING`.
- Linux integration and both native builds pass; Windows was cross-built, not run.
- Repo hygiene: `bin/*.so|.dll` binaries are no longer tracked — they ship via `dist/` release zips and are rebuilt locally, so fresh clones must build (or install a zip) instead of getting binaries from git.

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
