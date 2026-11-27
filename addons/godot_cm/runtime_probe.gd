extends Node

@export var target_path: NodePath
@export var duration := 15.0
var _elapsed := 0.0
var _next_sample := 0.0

func _physics_process(delta: float) -> void:
	_elapsed += delta
	if _elapsed > duration:
		set_physics_process(false)
		return
	if _elapsed < _next_sample or not EngineDebugger.is_active():
		return
	_next_sample = _elapsed + 0.2
	var target := get_node_or_null(target_path) as RigidBody3D
	if not target:
		return
	var p := target.global_position
	var v := target.linear_velocity
	var w := target.angular_velocity
	EngineDebugger.send_message("godot_cm:rigidbody_sample", [String(target_path), {
		"time": _elapsed, "position": [p.x, p.y, p.z],
		"linear_velocity": [v.x, v.y, v.z], "angular_velocity": [w.x, w.y, w.z],
		"sleeping": target.sleeping,
	}])
