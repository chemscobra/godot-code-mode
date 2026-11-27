# Spec Delta

## ADDED Requirements

### Requirement: Extended journaled inverse set
The journaled set SHALL include transform mutators (inverted by restoring
`global_transform`), `show`/`hide` (via the `visible` property),
`add_to_group`/`remove_from_group` (inverse pair), `move_child` (prior
index), `add_sibling` (`remove_child` on the captured parent), and
`grab_focus`/`release_focus` (inverse pair). `set_x` calls SHALL route to
the journaled `set` path when the property is authorized.

#### Scenario: Transform mutator rolls back
- **WHEN** code calls `translate`/`rotate`/`scale_object_local` and the transaction later fails
- **THEN** `global_transform` is restored to the captured prior value

#### Scenario: Group and sibling calls invert
- **WHEN** `add_to_group` or `add_sibling` applies and the transaction fails
- **THEN** `remove_from_group` or `remove_child` on the captured parent restores the prior structure
