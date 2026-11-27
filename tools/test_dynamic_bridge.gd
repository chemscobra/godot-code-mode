@tool
extends SceneTree

class MetadataProbe:
	extends RefCounted
	var calls := 0
	func _to_string() -> String:
		calls += 1
		return "metadata probe"

var probe := MetadataProbe.new()
const EditorOperations = preload("res://addons/godot_cm/editor_operations.gd")
const RuntimeDebugger = preload("res://addons/godot_cm/runtime_debugger.gd")
var editor_operations
var terrain_operations
var external_counter := 0
var terrain_root: Node3D
var service: RefCounted
var counter := 0
var catalog_updates := 0
var catalog_ticks := 0
var partial_class := false

func _initialize() -> void:
	_start.call_deferred()

func _start() -> void:
	service = McpService.new()
	if not service.start(int(OS.get_environment("GODOT_CM_TEST_PORT"))):
		quit(1)
		return
	editor_operations = EditorOperations.new()
	editor_operations.register(service)
	if ClassDB.class_exists("Terrain3D"):
		terrain_operations = load("res://addons/godot_cm_terrain3d_adapter/operations.gd").new()
		terrain_operations.register(service)
		terrain_root = Node3D.new()
		root.add_child(terrain_root)
		service.register_plugin_method("test.scene_root", self, {"method": "_scene_root", "read_only": true})
	service.set_plugin_discovery(_discover)
	assert(service.register_plugin_method("test.external", self, {"method": "_external", "effect": "external"}))
	assert(not service.register_plugin_method("test.bad_effect", self, {"method": "_external", "effect": "unknown"}))
	assert(not service.register_plugin_method("test.false_read", self, {"method": "_external", "effect": "external", "read_only": true}))
	assert(service.register_plugin_method("test.external_count", self, {"method": "_external_count", "read_only": true}))
	service.tier_request.connect(func(tier: String, _classes: Array) -> void:
		service.respond_tier_request(tier, true)
	)
	assert(service.register_plugin_method("test.read", self, {
		"method": "_read", "read_only": true,
	}))
	var foreign := Node.new()
	assert(not service.register_plugin_method("test.read", foreign, {"method": "get_name", "read_only": true}))
	foreign.free()
	assert(service.register_plugin_method("test.increment", self, {
		"method": "_increment", "capture": "_capture", "restore": "_restore",
		"description": "Increment the counter by an amount.",
		"arguments": [{"name": "amount", "type": "int"}], "return": "int",
		"input_schema": {"type": "object", "properties": {"amount": {"type": "integer"}}},
	}))
	assert(not service.register_plugin_method("test.unsafe", self, {"method": "_increment"}))
	assert(service.register_plugin_method("test.gated", self, {
		"method": "_read", "read_only": true, "tier": "filesystem",
	}))
	assert(service.register_plugin_method("test.catalog_control", self, {
		"method": "_catalog_control", "read_only": true,
	}))
	print("DYNAMIC_BRIDGE_READY")

func _discover() -> Array:
	var classes := [{"class": "DynamicTestProvider", "base": "SceneTree", "path": get_script().resource_path,
		"methods": get_script().get_script_method_list(),
		"properties": get_script().get_script_property_list()}]
	if partial_class:
		classes.append({"class": "UnloadedProvider", "base": "Node", "path": "res://unloaded.gd"})
	return classes

func _catalog_control(args: Dictionary) -> Dictionary:
	var name := String(args.get("name", "test.late"))
	match String(args.get("action", "")):
		"register":
			service.register_plugin_method(name, self, {"method": "_read", "read_only": true})
		"unregister":
			service.unregister_plugin_method(name)
		"partial":
			partial_class = bool(args.get("enabled", true))
			service.refresh_catalog()
		"stress":
			catalog_updates = int(args.get("count", 20))
			_catalog_tick()
		"unsafe_metadata":
			service.register_plugin_method("test.metadata", self, {
				"method": "_read", "read_only": true,
				"input_schema": {"type": "object", "default": probe},
			})
		"metadata_status":
			return {"ok": true, "result": probe.calls}
		"expire":
			var provider := Node.new()
			service.register_plugin_method("test.ephemeral", provider, {"method": "get_name", "read_only": true})
			provider.free()
	return {"ok": true, "result": catalog_ticks}

func _catalog_tick() -> void:
	if catalog_updates <= 0:
		return
	catalog_updates -= 1
	catalog_ticks += 1
	if catalog_updates % 2 == 0:
		service.register_plugin_method("test.toggling", self, {"method": "_read", "read_only": true})
	else:
		service.unregister_plugin_method("test.toggling")
	create_timer(0.02).timeout.connect(_catalog_tick)

func _read(_args: Dictionary) -> Dictionary:
	return {"ok": true, "result": counter}

func _capture(_args: Dictionary) -> Dictionary:
	return {"ok": true, "state": counter}

func _restore(state: int) -> bool:
	counter = state
	return true

func _increment(args: Dictionary) -> Dictionary:
	counter += int(args.get("amount", 1))
	return {"ok": not args.get("fail", false), "result": counter}

func _scene_root(_args: Dictionary) -> Dictionary:
	return {"ok": true, "result": terrain_root}

func _external(args: Dictionary) -> Dictionary:
	external_counter += int(args.get("amount", 1))
	return {"ok": not args.get("fail", false), "result": external_counter}

func _external_count(_args: Dictionary) -> Dictionary:
	return {"ok": true, "result": external_counter}

func _finalize() -> void:
	if service:
		service.stop()
