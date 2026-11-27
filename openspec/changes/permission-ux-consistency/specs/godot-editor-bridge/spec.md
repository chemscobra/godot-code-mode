# Spec Delta

## ADDED Requirements

### Requirement: Actionable denial diagnostics
Permission failures SHALL include a `remediation` hint naming the way
forward: `godot.requestTier('<tier>')` for locked tiers with an allow/ask
policy, the trusted op for denials that have one, or "denied permanently"
otherwise.

#### Scenario: Locked tier suggests requestTier
- **WHEN** a call fails because the `filesystem` tier is locked and its
  policy is `ask` or `allow`
- **THEN** the diagnostic includes `remediation` pointing at
  `godot.requestTier('filesystem')`

#### Scenario: Script denial suggests trusted op
- **WHEN** a call is denied for touching `Script`/`GDScript` classes
- **THEN** the remediation names `godot.createScript`/`godot.attachScript`

### Requirement: Tier request deadline
A parked `requestTier`/`request_tier` operation SHALL wait for the user's
confirmation for a dedicated human-scale deadline (nominally 120s),
independent of the execute call's mode timeout.

#### Scenario: Read-mode request survives mode timeout
- **WHEN** `godot.requestTier('filesystem')` runs inside a `read`-mode
  execute call whose default deadline is short
- **THEN** the tier request keeps waiting up to the dedicated deadline
  instead of timing out at the mode default

## MODIFIED Requirements

### Requirement: Per-operation authorization
Every bridged operation SHALL be validated against the allowlist — class,
method, and property — including operations invoked on objects returned by
prior calls. Member authorization SHALL be inheritance-aware: a member
allowlisted on a class is permitted on its subclasses, and `"*"` on a class
covers members it inherits from base classes. Classes in
classes whose group policy is `write` SHALL be authorized for read-mode
operations and denied for write/untracked ones.

#### Scenario: Blocked API denied
- **WHEN** code attempts to reach OS, process, network, dynamic code loading, or arbitrary file access through any object
- **THEN** the operation is rejected and produces no side effects

#### Scenario: Authorization applies to returned objects
- **WHEN** code obtains a returned object and calls a non-allowlisted method on it
- **THEN** that call is rejected even though the object itself was legitimately obtained

#### Scenario: Inherited member permitted
- **WHEN** code sets a member declared on a base class (e.g. `albedo_color` on `StandardMaterial3D` via `BaseMaterial3D`) and that member is allowlisted on the base class
- **THEN** the operation is permitted without requiring a separate allowlist entry on the subclass

#### Scenario: Setter spelling equivalence
- **WHEN** code calls `set_material_override(m)` or sets `material_override`
  directly with a valid `StandardMaterial3D`
- **THEN** both spellings are authorized through the journaled `set` path

#### Scenario: Widened editor inspection
- **WHEN** code calls `get_open_scenes` or `get_resource_filesystem` on the
  `EditorInterface` singleton
- **THEN** the call is authorized; the returned object's own tier still gates
  its members

#### Scenario: Write-denied class readable
- **WHEN** code calls a const/read method on a class in a `write`-policy
  group (e.g. `ProjectSettings.get_setting`)
- **THEN** the read succeeds while a write-mode call on the same class is
  denied

### Requirement: Permission introspection
The bridge SHALL expose `godot.permissions(query?)`: with no query a
group/policy/denial summary — including each group's name, classes,
policy, protection and live unlock state; with `{class, member}` an
explanation `{allowed, reason, tier?, policy?, unlock?}`. It SHALL be
passive — it never grants permissions or executes code.

#### Scenario: Summary without query
- **WHEN** code calls `godot.permissions()`
- **THEN** it receives group names, policies, protection flags, unlock
  state, class examples, and the denied class/member lists

#### Scenario: Member explanation
- **WHEN** code queries a denied member (e.g. `EditorInterface` outside the
  restrict list, `Node.rpc`, `Object.call`)
- **THEN** the response names the cause: `restricted_class`,
  `member_denied`, `meta_method_denied`, `class_denied`,
  `write_denied_class`, `tier_locked`, or `member_not_indexed`
