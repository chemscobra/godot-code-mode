# Spec Delta

## ADDED Requirements

### Requirement: Native collection results
Inside `execute`, returned `Array` values SHALL arrive as real JS arrays
and returned `Dictionary` values with all-string keys SHALL arrive as
plain JS objects. Packed collections, typed dictionaries, dictionaries
with non-string or reserved keys, and opaque value references SHALL keep
their tagged `{$t:...}` form.

#### Scenario: get_children is iterable
- **WHEN** a bridged call returns `Array` (typed or untyped, e.g.
  `get_children()`)
- **THEN** the awaited result is a JS array usable with `.map`/`.length`
- **AND** it can be passed back as a call argument without manual tagging

#### Scenario: Typed dictionary or non-string keys keep tag
- **WHEN** a bridged call returns a dictionary whose keys are not plain
  strings (or collide with reserved envelopes)
- **THEN** the `{$t:"Dictionary",entries:[...]}` form is preserved so key
  type information is not lost
