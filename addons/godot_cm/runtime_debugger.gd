@tool
extends EditorDebuggerPlugin

var samples: Dictionary = {}

func _has_capture(capture: String) -> bool:
	return capture == "godot_cm"

func _setup_session(session_id: int) -> void:
	get_session(session_id).started.connect(_clear.bind(session_id))

func _clear(session_id: int) -> void:
	samples[session_id] = []
	while samples.size() > 8:
		samples.erase(samples.keys()[0])

func _capture(message: String, data: Array, session_id: int) -> bool:
	if message != "godot_cm:rigidbody_sample" or data.size() != 2 or not data[1] is Dictionary:
		return false
	var history: Array = samples.get(session_id, [])
	var sample: Dictionary = data[1].duplicate(true)
	sample["target"] = String(data[0])
	history.append(sample)
	if history.size() > 200:
		history.pop_front()
	samples[session_id] = history
	return true
