extends Node2D
## Coin Rush — the idiomatic pickup loop: CharacterBody2D player moved
## with move_and_slide() in _physics_process, coins as Area2D triggers
## emitting body_entered, HUD on a CanvasLayer.

const SPEED := 240.0

var score := 0
var rng := RandomNumberGenerator.new()

@onready var player: CharacterBody2D = $Player
@onready var hud: Label = $HudLayer/Hud

func _ready() -> void:
	_update_hud()

func _physics_process(_delta: float) -> void:
	player.velocity = _move_dir() * SPEED
	player.move_and_slide()

## Arrow keys + WASD — Input.get_vector covers ui_* arrows; WASD reads
## physical keys so no project InputMap setup is needed.
func _move_dir() -> Vector2:
	var d := Input.get_vector("ui_left", "ui_right", "ui_up", "ui_down")
	if Input.is_key_pressed(KEY_A): d.x -= 1.0
	if Input.is_key_pressed(KEY_D): d.x += 1.0
	if Input.is_key_pressed(KEY_W): d.y -= 1.0
	if Input.is_key_pressed(KEY_S): d.y += 1.0
	return d.normalized() if d.length() > 1.0 else d

func _on_coin_body_entered(body: Node2D, coin_name: StringName) -> void:
	if body != player:
		return
	var coin := get_node_or_null(NodePath(coin_name)) as Area2D
	if coin == null:
		return
	score += 1
	coin.position = Vector2(rng.randf_range(-400, 400), rng.randf_range(-300, 300))
	_update_hud()

func _update_hud() -> void:
	hud.text = "Coins: %d" % score
