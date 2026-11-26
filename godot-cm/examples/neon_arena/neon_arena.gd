extends Node3D
## Neon Arena — collect all 12 energy cells while dodging patrolling
## sentries. Idiomatic pattern: CharacterBody3D player, cells/sentries as
## Area3D triggers emitting body_entered, walls/floor as StaticBody3D so
## move_and_slide gives real collision (no manual clamp on the player).
## Sentries keep scripted patrol movement (they're Areas, not bodies).

const SPEED := 7.0
const INVULN_TIME := 1.5
const CELLS_TOTAL := 12

var lives := 3
var score := 0
var invuln := 0.0
var rng := RandomNumberGenerator.new()
var sentries: Array[Area3D] = []
var sentry_dirs := {}      # sentry -> Vector3 patrol dir
var time := 0.0

@onready var player: CharacterBody3D = $Player
@onready var hud: Label = $HudLayer/Hud

func _ready() -> void:
	for c in get_children():
		if c.name.begins_with("Sentry_"):
			sentries.append(c)
			sentry_dirs[c] = Vector3(rng.randf_range(-1, 1), 0,
					rng.randf_range(-1, 1)).normalized() * 3.0
	_update_hud()

func _physics_process(delta: float) -> void:
	time += delta
	invuln = max(0.0, invuln - delta)

	var dir := _move_dir()
	player.velocity = Vector3(dir.x, 0, dir.y) * SPEED
	player.move_and_slide()

	for s in sentries:
		s.position += sentry_dirs[s] * delta
		var skin := s.get_node_or_null("Skin")
		if skin:
			skin.rotate_y(delta * 4.0)
		# Bounce off arena bounds — sentries are Areas, not physics bodies.
		if abs(s.position.x) > 13 or abs(s.position.z) > 13:
			sentry_dirs[s] = -sentry_dirs[s]
			s.position.x = clampf(s.position.x, -13, 13)
			s.position.z = clampf(s.position.z, -13, 13)

## Arrow keys + WASD — Input.get_vector covers ui_* arrows; WASD reads
## physical keys so no project InputMap setup is needed.
func _move_dir() -> Vector2:
	var d := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	if Input.is_key_pressed(KEY_A): d.x -= 1.0
	if Input.is_key_pressed(KEY_D): d.x += 1.0
	if Input.is_key_pressed(KEY_W): d.y -= 1.0
	if Input.is_key_pressed(KEY_S): d.y += 1.0
	return d.normalized() if d.length() > 1.0 else d

func _on_cell_body_entered(body: Node3D, cell_name: StringName) -> void:
	if body != player:
		return
	var cell := get_node_or_null(NodePath(cell_name)) as Area3D
	if cell == null or not cell.visible:
		return
	score += 1
	cell.visible = false
	cell.set_deferred("monitoring", false) # signal-safe: off for next frame
	_update_hud("YOU WIN — cells: %d" % score if score >= CELLS_TOTAL else "")

func _on_sentry_body_entered(body: Node3D, _sentry_name: StringName) -> void:
	if body != player or invuln > 0.0:
		return
	_hit()

func _hit() -> void:
	lives -= 1
	invuln = INVULN_TIME
	player.position = Vector3(0, 0.5, 0)
	_update_hud("HIT! " if lives > 0 else "GAME OVER ")

func _update_hud(prefix := "") -> void:
	hud.text = "%sLives: %d | Cells: %d" % [prefix, lives, score]
