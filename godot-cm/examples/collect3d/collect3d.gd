extends Node3D
## Orb Run — the idiomatic pickup loop in 3D: CharacterBody3D player moved
## with move_and_slide() in _physics_process, orbs as Area3D triggers
## emitting body_entered, HUD on a CanvasLayer.

const SPEED := 6.0

var score := 0
var rng := RandomNumberGenerator.new()
var time := 0.0

@onready var player: CharacterBody3D = $Player
@onready var hud: Label = $HudLayer/Hud

func _ready() -> void:
	_update_hud()

func _physics_process(delta: float) -> void:
	time += delta
	var dir := _move_dir()
	player.velocity = Vector3(dir.x, 0, dir.y) * SPEED
	player.move_and_slide()
	for c in get_children():
		if c.name.begins_with("Orb_"):
			var skin := c.get_node_or_null("Skin")
			if skin:
				skin.rotate_y(delta * 2.0)

## Arrow keys + WASD — Input.get_vector covers ui_* arrows; WASD reads
## physical keys so no project InputMap setup is needed.
func _move_dir() -> Vector2:
	var d := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	if Input.is_key_pressed(KEY_A): d.x -= 1.0
	if Input.is_key_pressed(KEY_D): d.x += 1.0
	if Input.is_key_pressed(KEY_W): d.y -= 1.0
	if Input.is_key_pressed(KEY_S): d.y += 1.0
	return d.normalized() if d.length() > 1.0 else d

func _on_orb_body_entered(body: Node3D, orb_name: StringName) -> void:
	if body != player:
		return
	var orb := get_node_or_null(NodePath(orb_name)) as Area3D
	if orb == null:
		return
	score += 1
	orb.position = Vector3(rng.randf_range(-8, 8), 0.5, rng.randf_range(-8, 8))
	_update_hud()

func _update_hud() -> void:
	hud.text = "Orbs: %d" % score
