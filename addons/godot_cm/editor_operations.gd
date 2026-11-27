@tool
extends RefCounted

const Probe = preload("res://addons/godot_cm/runtime_probe.gd")
var debugger

func register(service: RefCounted) -> void:
	var actions := {
		"editor.create_scene": ["create_scene", "Create and open a new .tscn without overwriting files. Args: path, root_type (Node3D by default), name."],
		"editor.open_scene": ["open_scene", "Open a saved .tscn. Refuses when any scene has unsaved changes. Args: path."],
		"editor.save_scene": ["save_scene", "Save the current scene. Args: confirm_save must be true to acknowledge the disk write."],
		"editor.play_scene": ["play_scene", "Run the saved current scene in a separate call after saving. Refuses unsaved changes."],
		"editor.stop_scene": ["stop_scene", "Stop the running scene. This is an external action, not an undoable edit."],
		"editor.reload_scene": ["reload_scene", "Reload the saved scene and reopen it. Args: path (defaults to the current scene). Refuses unsaved changes."],
	}
	for name in actions:
		service.register_plugin_method(name, self, {
			"method": actions[name][0], "effect": "external",
			"tier": "filesystem" if name in ["editor.create_scene", "editor.open_scene", "editor.save_scene", "editor.reload_scene"] else "",
			"description": actions[name][1],
		})
	service.register_plugin_method("editor.state", self, {"method": "state", "read_only": true})
	service.register_plugin_method("editor.runtime_samples", self, {
		"method": "runtime_samples", "read_only": true,
		"description": "Return bounded samples emitted by the optional RigidBody3D probe in a debug run.",
	})
	service.register_plugin_method("editor.watch_rigidbody", self, {
		"method": "watch_rigidbody", "capture": "capture_watch", "restore": "restore_watch",
		"description": "Add an undoable built-in runtime physics probe. Args: target (RigidBody3D handle). No arbitrary scripts are accepted.",
	})
	service.register_plugin_method("editor.attach_script", self, {
		"method": "attach_script", "capture": "capture_script", "restore": "restore_script",
		"tier": "filesystem",
		"description": "Undoably assign a non-@tool .gd script to a node. Args: node (Node handle), path (res://*.gd).",
	})

func _fail(message: String) -> Dictionary:
	return {"ok": false, "message": message}

func _scene_path(path: String) -> bool:
	return path.begins_with("res://") and path.ends_with(".tscn") and not path.contains("\\") and not path.substr(6).split("/").has("..") and path.simplify_path() == path

func _can_switch() -> bool:
	return EditorInterface.get_unsaved_scenes().is_empty() and not EditorInterface.is_playing_scene()

func create_scene(args: Dictionary) -> Dictionary:
	var path := String(args.get("path", ""))
	if not _scene_path(path):
		return _fail("A canonical res:// .tscn path is required")
	if not _can_switch():
		return _fail("Save your open scenes and stop playback before switching scenes")
	if FileAccess.file_exists(path):
		return _fail("Refusing to overwrite an existing scene")
	var root_type := String(args.get("root_type", "Node3D"))
	if root_type not in ["Node3D", "Node2D", "Node", "Control"]:
		return _fail("Unsupported root type")
	var error := DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(path.get_base_dir()))
	if error != OK:
		return _fail("Could not create the scene directory")
	var root: Node = ClassDB.instantiate(root_type)
	root.name = String(args.get("name", "MCPTest"))
	var packed := PackedScene.new()
	error = packed.pack(root)
	root.free()
	if error == OK:
		error = ResourceSaver.save(packed, path)
	if error != OK:
		return _fail("Could not save the new scene: %s" % error_string(error))
	EditorInterface.open_scene_from_path.call_deferred(path)
	return {"ok": true, "result": {"path": path, "opening": true}}

func open_scene(args: Dictionary) -> Dictionary:
	var path := String(args.get("path", ""))
	if not _scene_path(path) or not FileAccess.file_exists(path):
		return _fail("The .tscn scene must exist inside the project")
	if not _can_switch():
		return _fail("Save your open scenes and stop playback before switching scenes")
	EditorInterface.open_scene_from_path.call_deferred(path)
	return {"ok": true, "result": {"path": path, "opening": true}}

func save_scene(args: Dictionary) -> Dictionary:
	if not bool(args.get("confirm_save", false)):
		return _fail("Set confirm_save to true to acknowledge saving the current scene")
	var root := EditorInterface.get_edited_scene_root()
	if not root or root.scene_file_path.is_empty():
		return _fail("Open a scene with a saved path first")
	var error := EditorInterface.save_scene()
	if error != OK:
		return _fail(error_string(error))
	return {"ok": true, "result": {"path": root.scene_file_path}}

func play_scene(_args: Dictionary) -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	if not root or root.scene_file_path.is_empty() or not _can_switch():
		return _fail("Open and save the scene, and stop existing playback first")
	EditorInterface.play_current_scene.call_deferred()
	return {"ok": true, "result": {"requested": true, "path": root.scene_file_path}}

func reload_scene(args: Dictionary) -> Dictionary:
	var path := String(args.get("path", ""))
	var root := EditorInterface.get_edited_scene_root()
	if path.is_empty() and root:
		path = root.scene_file_path
	if not _scene_path(path) or not FileAccess.file_exists(path):
		return _fail("The .tscn scene must exist inside the project")
	if not _can_switch():
		return _fail("Save your open scenes and stop playback before reloading")
	EditorInterface.reload_scene_from_path(path)
	EditorInterface.open_scene_from_path.call_deferred(path)
	return {"ok": true, "result": {"path": path, "reopening": true}}

func stop_scene(_args: Dictionary) -> Dictionary:
	EditorInterface.stop_playing_scene.call_deferred()
	return {"ok": true, "result": {"requested": true}}

func state(_args: Dictionary) -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	return {"ok": true, "result": {
		"scene": root.scene_file_path if root else "", "playing": EditorInterface.is_playing_scene(),
		"unsaved": Array(EditorInterface.get_unsaved_scenes()),
	}}

func runtime_samples(_args: Dictionary) -> Dictionary:
	var sessions := []
	if debugger:
		for id in debugger.samples:
			sessions.append({"session_id": id, "samples": debugger.samples[id].duplicate(true)})
	return {"ok": true, "result": sessions}

func _script_path(path: String) -> bool:
	return path.begins_with("res://") and path.ends_with(".gd") and not path.contains("\\") and not path.substr(6).split("/").has("..") and path.simplify_path() == path

func _valid_script_target(node, path: String) -> Dictionary:
	if node == null or not node is Node:
		return _fail("The target must be a Node handle")
	if not _script_path(path) or not FileAccess.file_exists(path):
		return _fail("A canonical res:// .gd path that exists is required")
	var script := ResourceLoader.load(path)
	if not script or not script is Script:
		return _fail("The resource is not a script")
	if (script as Script).is_tool():
		return _fail("Refusing @tool scripts: they run editor code and are not rollbackable")
	var base := (script as Script).get_instance_base_type()
	if node.get_class() != base and not ClassDB.is_parent_class(node.get_class(), base):
		return _fail("Script base type '%s' is incompatible with %s" % [base, node.get_class()])
	return {"ok": true, "node": node, "script": script}

func capture_script(args: Dictionary) -> Dictionary:
	var check := _valid_script_target(args.get("node"), String(args.get("path", "")))
	if not check.get("ok", false):
		return check
	return {"ok": true, "state": {"node": check.node, "script": (check.node as Node).get_script()}}

func attach_script(args: Dictionary) -> Dictionary:
	var check := _valid_script_target(args.get("node"), String(args.get("path", "")))
	if not check.get("ok", false):
		return check
	var node: Node = check.node
	node.set_script(check.script)
	EditorInterface.mark_scene_as_unsaved()
	return {"ok": true, "result": {"node": String(node.name), "path": check.script.resource_path}}

func restore_script(snapshot: Dictionary) -> bool:
	var node: Node = snapshot.get("node")
	if not is_instance_valid(node):
		return false
	node.set_script(snapshot.get("script"))
	EditorInterface.mark_scene_as_unsaved()
	return true

func capture_watch(args: Dictionary) -> Dictionary:
	var root := EditorInterface.get_edited_scene_root()
	var target := args.get("target") as RigidBody3D
	if not root or not target or not root.is_ancestor_of(target):
		return _fail("The target must be a RigidBody3D in the edited scene")
	if root.has_node("GodotCMPhysicsProbe"):
		return _fail("A probe is already present in this scene")
	return {"ok": true, "state": {"root": root}}

func watch_rigidbody(args: Dictionary) -> Dictionary:
	var validation := capture_watch(args)
	if not validation.get("ok", false):
		return validation
	var root: Node = validation.state.root
	var target: RigidBody3D = args.target
	var probe := Probe.new()
	probe.name = "GodotCMPhysicsProbe"
	root.add_child(probe)
	probe.owner = root
	probe.target_path = probe.get_path_to(target)
	EditorInterface.mark_scene_as_unsaved()
	return {"ok": true, "result": {"watching": String(probe.target_path)}}

func restore_watch(snapshot: Dictionary) -> bool:
	var root: Node = snapshot.get("root")
	if not is_instance_valid(root):
		return false
	var probe := root.get_node_or_null("GodotCMPhysicsProbe")
	if probe:
		root.remove_child(probe)
		probe.free()
	EditorInterface.mark_scene_as_unsaved()
	return true
