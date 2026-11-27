import argparse
import configparser
import contextlib
import hashlib
import json
import io
import math
import platform
import statistics
import tarfile
import os
import re
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request
from types import SimpleNamespace


VARIANT_FIXTURES = [
    ("Nil", "NIL", "scalar", None),
    ("bool", "BOOL", "scalar", True),
    ("int", "INT", "scalar", 42),
    ("float", "FLOAT", "scalar", 1.25),
    ("String", "STRING", "scalar", "sample"),
    ("Vector2", "VECTOR2", "named", {"x": 1, "y": 2}),
    ("Vector2i", "VECTOR2I", "named", {"x": 1, "y": 2}),
    ("Rect2", "RECT2", "named", {"position": {"x": 1, "y": 2}, "size": {"x": 3, "y": 4}}),
    ("Rect2i", "RECT2I", "named", {"position": {"x": 1, "y": 2}, "size": {"x": 3, "y": 4}}),
    ("Vector3", "VECTOR3", "named", {"x": 1, "y": 2, "z": 3}),
    ("Vector3i", "VECTOR3I", "named", {"x": 1, "y": 2, "z": 3}),
    ("Transform2D", "TRANSFORM2D", "named", {"x": {"x": 1, "y": 0}, "y": {"x": 0, "y": 1}, "origin": {"x": 2, "y": 3}}),
    ("Vector4", "VECTOR4", "named", {"x": 1, "y": 2, "z": 3, "w": 4}),
    ("Vector4i", "VECTOR4I", "named", {"x": 1, "y": 2, "z": 3, "w": 4}),
    ("Plane", "PLANE", "named", {"normal": {"x": 0, "y": 1, "z": 0}, "d": 2}),
    ("Quaternion", "QUATERNION", "named", {"x": 0, "y": 0, "z": 0, "w": 1}),
    ("AABB", "AABB", "named", {"position": {"x": 1, "y": 2, "z": 3}, "size": {"x": 4, "y": 5, "z": 6}}),
    ("Basis", "BASIS", "named", {"x": {"x": 1, "y": 0, "z": 0}, "y": {"x": 0, "y": 1, "z": 0}, "z": {"x": 0, "y": 0, "z": 1}}),
    ("Transform3D", "TRANSFORM3D", "named", {"basis": {"x": {"x": 1, "y": 0, "z": 0}, "y": {"x": 0, "y": 1, "z": 0}, "z": {"x": 0, "y": 0, "z": 1}}, "origin": {"x": 2, "y": 3, "z": 4}}),
    ("Projection", "PROJECTION", "named", {"x": {"x": 1, "y": 0, "z": 0, "w": 0}, "y": {"x": 0, "y": 1, "z": 0, "w": 0}, "z": {"x": 0, "y": 0, "z": 1, "w": 0}, "w": {"x": 0, "y": 0, "z": 0, "w": 1}}),
    ("Color", "COLOR", "named", {"r": 2, "g": 0.5, "b": 0, "a": 1}),
    ("StringName", "STRING_NAME", "string_like", "example"),
    ("NodePath", "NODE_PATH", "string_like", "Parent/Child"),
    ("RID", "RID", "opaque", None),
    ("Object", "OBJECT", "object", None),
    ("Callable", "CALLABLE", "opaque", None),
    ("Signal", "SIGNAL", "opaque", None),
    ("Dictionary", "DICTIONARY", "collection", {"sample": 1}),
    ("Array", "ARRAY", "collection", [1, 2]),
    ("PackedByteArray", "PACKED_BYTE_ARRAY", "packed", [0, 128, 255]),
    ("PackedInt32Array", "PACKED_INT32_ARRAY", "packed", [-2147483648, 2147483647]),
    ("PackedInt64Array", "PACKED_INT64_ARRAY", "packed", [1, {"$t": "int", "value": "9223372036854775807"}]),
    ("PackedFloat32Array", "PACKED_FLOAT32_ARRAY", "packed", [1.25, -2.5]),
    ("PackedFloat64Array", "PACKED_FLOAT64_ARRAY", "packed", [1.25, -2.5]),
    ("PackedStringArray", "PACKED_STRING_ARRAY", "packed", ["one", "two"]),
    ("PackedVector2Array", "PACKED_VECTOR2_ARRAY", "packed", [{"x": 1, "y": 2}]),
    ("PackedVector3Array", "PACKED_VECTOR3_ARRAY", "packed", [{"x": 1, "y": 2, "z": 3}]),
    ("PackedColorArray", "PACKED_COLOR_ARRAY", "packed", [{"r": 2, "g": 0.5, "b": 0, "a": 1}]),
    ("PackedVector4Array", "PACKED_VECTOR4_ARRAY", "packed", [{"x": 1, "y": 2, "z": 3, "w": 4}]),
]


def verify_variant_inventory(repository):
    api = json.loads((repository / "api/extension_api.json").read_text())
    enumeration = next(item for item in api["global_enums"] if item["name"] == "Variant.Type")
    expected = {item["name"]: item["value"] for item in enumeration["values"] if item["name"] != "TYPE_MAX"}
    actual = {"TYPE_" + enum: index for index, (_, enum, _, _) in enumerate(VARIANT_FIXTURES)}
    assert len(VARIANT_FIXTURES) == len(actual) == 39
    assert actual == expected, {"fixtures": actual, "api": expected}
    header = (repository / "godot-cpp/include/godot_cpp/variant/variant.hpp").read_text()
    enum_body = header.split("enum Type {", 1)[1].split("VARIANT_MAX", 1)[0]
    native = re.findall(r"^\s*([A-Z][A-Z0-9_]*)\s*,", enum_body, re.MULTILINE)
    assert native == [fixture[1] for fixture in VARIANT_FIXTURES], native
    simulated_new_type = dict(expected, TYPE_FUTURE_VARIANT=39)
    assert actual != simulated_new_type
    assert {category for _, _, category, _ in VARIANT_FIXTURES} == {"scalar", "named", "string_like", "opaque", "object", "collection", "packed"}
    print("Variant fixture inventory: 39 types; API/header coverage and future-type detection passed", flush=True)


BOOTSTRAP = '''@tool
extends EditorPlugin

const Widget = preload("res://generic_widget.gd")
const TypedConstructor = preload("res://typed_constructor.gd")
const EditorOps = preload("res://addons/godot_cm/editor_operations.gd")
var service: RefCounted
var editor_ops: RefCounted
var frame_capture_enabled := OS.get_environment("GENERIC_TEST_FRAMES") == "1"
var frame_sampling := false
var frame_done := false
var frame_previous := 0
var frame_drawn_start := 0
var frame_omitted := 0
var frame_intervals := PackedFloat64Array()
var frame_work := PackedFloat64Array()
var frame_work_start := 0

func _enter_tree() -> void:
    if frame_capture_enabled:
        get_tree().process_frame.connect(_frame_begin)
    call_deferred("_start")

func _frame_begin() -> void:
    if frame_sampling and not frame_done:
        frame_work_start = Time.get_ticks_usec()

func _frame_end() -> void:
    if frame_sampling and not frame_done and frame_work_start > 0 and frame_work.size() < 100000:
        frame_work.append(float(Time.get_ticks_usec() - frame_work_start) / 1000.0)

func _start() -> void:
    await _probe_activation()
    service = ClassDB.instantiate("McpService")
    var started := Time.get_ticks_msec()
    assert(service.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
    Widget.test_service = weakref(service)
    if frame_capture_enabled:
        get_tree().process_frame.connect(_frame_end)
    if not OS.get_environment("GENERIC_TEST_DTS").is_empty():
        assert(DirAccess.copy_absolute(ProjectSettings.globalize_path("user://godot_cm-api-surface.d.ts"), OS.get_environment("GENERIC_TEST_DTS")) == OK)
    FileAccess.open("res://startup_ms", FileAccess.WRITE).store_string(str(Time.get_ticks_msec() - started))
    var occupied = ClassDB.instantiate("McpService")
    started = Time.get_ticks_msec()
    assert(not occupied.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
    FileAccess.open("res://busy_ms", FileAccess.WRITE).store_string(str(Time.get_ticks_msec() - started))
    occupied = null
    service.tier_request.connect(_permission)
    service.set_tier_policy("untracked", "deny" if OS.get_environment("GENERIC_TEST_DENY") == "1" else "ask")
    service.set_tier_policy("filesystem", "ask")
    editor_ops = EditorOps.new()
    editor_ops.register(service)
    EditorInterface.open_scene_from_path("res://blank.tscn")
    await get_tree().process_frame
    await get_tree().process_frame
    assert(EditorInterface.get_edited_scene_root() != null)
    FileAccess.open("res://ready", FileAccess.WRITE).store_string("ready")

func _process(_delta: float) -> void:
    if frame_capture_enabled and not frame_done:
        var now := Time.get_ticks_usec()
        if not frame_sampling and FileAccess.file_exists("res://frames_start"):
            frame_sampling = true
            frame_previous = now
            frame_drawn_start = Engine.get_frames_drawn()
            FileAccess.open("res://frames_ready", FileAccess.WRITE).store_string("ready")
        elif frame_sampling:
            EditorInterface.get_base_control().queue_redraw()
            if frame_intervals.size() < 100000:
                frame_intervals.append(float(now - frame_previous) / 1000.0)
            else:
                frame_omitted += 1
            frame_previous = now
            if FileAccess.file_exists("res://frames_stop"):
                var data := {"intervals_ms": Array(frame_intervals), "continuous_process_frame_ms": Array(frame_work), "omitted": frame_omitted, "frames_drawn": Engine.get_frames_drawn() - frame_drawn_start}
                FileAccess.open("res://frames.json", FileAccess.WRITE).store_string(JSON.stringify(data))
                frame_done = true
    for action in ["undo", "redo"]:
        if FileAccess.file_exists("res://" + action + "_request") and not FileAccess.file_exists("res://" + action + "_ready"):
            var manager := get_undo_redo()
            var history := manager.get_object_history_id(EditorInterface.get_edited_scene_root())
            manager.get_history_undo_redo(history).call(action)
            FileAccess.open("res://" + action + "_ready", FileAccess.WRITE).store_string("ready")
    if FileAccess.file_exists("res://destroy_rollback"):
        var root := EditorInterface.get_edited_scene_root()
        var victim := root.get_node_or_null("RollbackVictim") if root != null else null
        if victim is Node3D and victim.position.x == 777:
            victim.free()
    if service == null or not FileAccess.file_exists("res://restart_request") or FileAccess.file_exists("res://restart_ready"):
        return
    assert(_catalog_connections() == [1, 1, 1, 1, 1])
    service.stop()
    assert(_catalog_connections() == [0, 0, 0, 0, 0])
    service = ClassDB.instantiate("McpService")
    assert(service.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
    Widget.test_service = weakref(service)
    service.tier_request.connect(_permission)
    service.set_tier_policy("untracked", "ask")
    service.set_tier_policy("filesystem", "ask")
    editor_ops.register(service)
    assert(_catalog_connections() == [1, 1, 1, 1, 1])
    FileAccess.open("res://restart_ready", FileAccess.WRITE).store_string("ready")

func _catalog_connections() -> Array:
    var counts := []
    var filesystem := EditorInterface.get_resource_filesystem()
    for pair in [[GDExtensionManager, "extensions_reloaded"], [GDExtensionManager, "extension_loaded"], [GDExtensionManager, "extension_unloading"], [filesystem, "filesystem_changed"], [filesystem, "script_classes_updated"]]:
        var count := 0
        for connection in pair[0].get_signal_connection_list(pair[1]):
            var owner: Object = connection["callable"].get_object()
            if owner != null and owner.get_class() == "BridgePump":
                count += 1
        counts.append(count)
    return counts

func _has_status(text: String) -> bool:
    for label in EditorInterface.get_base_control().find_children("*", "Label", true, false):
        if label.text == text:
            return true
    return false

func _probe_activation() -> void:
    var started := Time.get_ticks_msec()
    EditorInterface.set_plugin_enabled("godot_cm", true)
    var activation_ms := Time.get_ticks_msec() - started
    assert(EditorInterface.is_plugin_enabled("godot_cm"))
    assert(_has_status("Starting MCP server..."))
    EditorInterface.set_plugin_enabled("godot_cm", false)
    await get_tree().process_frame
    await get_tree().process_frame
    await get_tree().process_frame
    assert(not _has_status("MCP server running"))
    var probe := TCPServer.new()
    assert(probe.listen(int(OS.get_environment("GENERIC_TEST_PORT")), "127.0.0.1") == OK)
    probe.stop()
    started = Time.get_ticks_msec()
    EditorInterface.set_plugin_enabled("godot_cm", true)
    activation_ms = maxi(activation_ms, Time.get_ticks_msec() - started)
    assert(_has_status("Starting MCP server..."))
    var deadline := Time.get_ticks_msec() + 15000
    while not _has_status("MCP server running") and Time.get_ticks_msec() < deadline:
        await get_tree().process_frame
    assert(_has_status("MCP server running"))
    var ready_ms := Time.get_ticks_msec() - started
    EditorInterface.set_plugin_enabled("godot_cm", false)
    FileAccess.open("res://activation_ms", FileAccess.WRITE).store_string(JSON.stringify({"checkbox": activation_ms, "ready": ready_ms}))

func _permission(tier: String, _classes: Array) -> void:
    service.respond_tier_request(tier, true)

func _exit_tree() -> void:
    if frame_capture_enabled:
        for callback in [_frame_begin, _frame_end]:
            if get_tree().process_frame.is_connected(callback):
                get_tree().process_frame.disconnect(callback)
    if service:
        service.stop()
        service = null
'''

WIDGET = '''@tool
class_name GenericMcpWidget
extends Node3D

static var constructions := 0
static var test_service: WeakRef
var counter := 0
var typed_points: Array[Vector3] = []
var typed_lookup: Dictionary[int, Vector3] = {}
var metadata_guard := false

func _get_property_list() -> Array[Dictionary]:
    if metadata_guard:
        FileAccess.open("res://unexpected_property_probe", FileAccess.WRITE).store_string("called")
    return []

func typed_echo(value: Transform3D, points: Array[Vector3], weight: float = 1.5) -> Transform3D:
    return value

func untyped_echo(value):
    return value

func _init(initial: int = 0) -> void:
    constructions += 1
    counter = initial

static func static_identity(value: int) -> int:
    return value

func constructor_count() -> int:
    return constructions

func increment(amount: int) -> int:
    counter += amount
    return counter

func get_mutating() -> int:
    counter += 1
    return counter

func peek() -> int:
    return counter

func delay_mutate(milliseconds: int) -> int:
    OS.delay_msec(milliseconds)
    counter += 1
    return counter
'''

for _type_name, _enum_name, _, _ in VARIANT_FIXTURES:
    _declared_type = "Variant" if _type_name == "Nil" else _type_name
    WIDGET += f'\nfunc echo_{_enum_name.lower()}(value: {_declared_type}) -> {_declared_type}:\n    return value\n'

WIDGET += '''
func native_fixture(type_name: String) -> Variant:
    match type_name:
        "RID": return RID()
        "Callable": return Callable(self, "peek")
        "Signal": return renamed
        "Object": return self
    return null

func nonfinite_vector() -> Vector3:
    return Vector3(NAN, INF, -INF)

static func static_vector(value: Vector3) -> Vector3:
    return value

func projection_column(value: Projection) -> Vector4:
    return value * Vector4(0, 1, 0, 0)

func known_projection() -> Projection:
    return Projection(Vector4(1, 2, 3, 4), Vector4(5, 6, 7, 8), Vector4(9, 10, 11, 12), Vector4(13, 14, 15, 16))

func basis_point(value: Basis) -> Vector3:
    return value * Vector3(1, 2, 3)

func transform_point(value: Transform3D) -> Vector3:
    return value * Vector3(1, 2, 3)

func transform_2d_point(value: Transform2D) -> Vector2:
    return value * Vector2(1, 1)

func euler_basis(value: Vector3, order: int) -> Basis:
    return Basis.from_euler(value, order)

func large_strings(count: int) -> PackedStringArray:
    var result := PackedStringArray()
    result.resize(count)
    result.fill("entry")
    return result

func large_bytes(count: int) -> PackedByteArray:
    var result := PackedByteArray()
    result.resize(count)
    return result

func large_item() -> Array:
    return ["x".repeat(20000)]

func page_rejection() -> Array:
    var result := []
    result.resize(300)
    result[0] = "p".repeat(15815)
    result[1] = "f".repeat(200)
    result[2] = "x".repeat(20000)
    return result

func page_retention_pair() -> Array:
    var result := []
    result.resize(300)
    result[0] = "x".repeat(20000)
    result[1] = "y".repeat(20000)
    return result

func native_cycle() -> Array:
    var result: Array = []
    result.append(result)
    return result

func clear_cycle(value: Array) -> void:
    value.clear()

func echo_typed_array(value: Array[Vector3]) -> Array[Vector3]:
    return value

func echo_typed_dictionary(value: Dictionary[int, Vector3]) -> Dictionary[int, Vector3]:
    return value

func echo_widget_array(value: Array[GenericMcpWidget]) -> Array[GenericMcpWidget]:
    return value
'''

LONG_EFFECT = "budget_effect_" + "x" * 480
WIDGET += f'\nfunc {LONG_EFFECT}() -> void:\n    pass\n'
WIDGET += '''
func default_vector(value: Vector3 = Vector3(1,2,3)) -> Vector3:
    return value

func default_callable(value: Callable = Callable()) -> bool:
    return value.is_valid()

func page_strings(value: PackedStringArray, offset: int, limit: int) -> Array:
    return Array(value.slice(offset, offset + limit))

func script_identity(resource: Script) -> int:
    return resource.get_instance_id()

func replace_script_source(resource: Script, source: String) -> void:
    resource.source_code = source

func reload_script(resource: Script) -> int:
    return resource.reload(true)

func reload_native(path: String) -> int:
    return GDExtensionManager.reload_extension(path)

func destroy_node(node: Node) -> void:
    node.free()

func set_test_policy(tier: String, policy: String) -> void:
    test_service.get_ref().set_tier_policy(tier, policy)

func save_group_policy(group: String, state: String) -> bool:
    return test_service.get_ref().save_group_policy(group, state)

func reset_group_policies() -> bool:
    return test_service.get_ref().reset_group_policies()

func allow_all_groups(include_protected: bool = false) -> bool:
    return test_service.get_ref().allow_all_groups(include_protected)

func scan_scripts() -> void:
    EditorInterface.get_resource_filesystem().scan()

func update_script(path: String) -> void:
    EditorInterface.get_resource_filesystem().update_file(path)

func scan_busy() -> bool:
    return EditorInterface.get_resource_filesystem().is_scanning()

func registered_script(path: String) -> bool:
    for entry in ProjectSettings.get_global_class_list():
        if entry.path == path:
            return true
    return false

func unicode_strings(width: int, count: int) -> PackedStringArray:
    var token := "é漢" + String.chr(10) + String.chr(9) + String.chr(34) + String.chr(92)
    var values := PackedStringArray()
    for index in range(count):
        values.append(token.repeat(width))
    return values

func unicode_dictionary(width: int, count: int) -> Dictionary:
    var token := "é漢" + String.chr(10) + String.chr(9) + String.chr(34) + String.chr(92)
    var values := {}
    for index in range(count):
        values[str(index) + token] = token.repeat(width)
    return values

func page_size_oracle(values: Variant, offset: int, limit: int) -> Dictionary:
    var items := []
    var dictionary := values is Dictionary
    var keys: Array = values.keys() if dictionary else []
    var page := {"type":type_string(typeof(values)), "variant_type":typeof(values), "offset":offset, "total":values.size()}
    page["entries" if dictionary else "items"] = items
    for index in range(offset, mini(values.size(), offset + limit)):
        var item: Variant = {"key":keys[index], "value":values[keys[index]]} if dictionary else values[index]
        assert(JSON.stringify(item).to_utf8_buffer().size() <= 16384 - 512)
        items.append(item)
        if JSON.stringify(page).to_utf8_buffer().size() > 16384 - 256:
            items.pop_back()
            break
    var count := items.size()
    page["count"] = count
    page["next_offset"] = offset + count if offset + count < values.size() else null
    page["truncated"] = offset + count < values.size()
    return {"count":count, "bytes":JSON.stringify(page).to_utf8_buffer().size(), "next_offset":page["next_offset"]}

func benchmark_sleep(milliseconds: int) -> int:
    OS.delay_msec(milliseconds)
    return milliseconds

func large_dictionary(count: int) -> Dictionary:
    var result := {}
    for i in count:
        result["key_" + str(i)] = i
    return result
'''

TYPED_CONSTRUCTOR = '''@tool
class_name GenericTypedConstructor
extends RefCounted
var received: Vector3
func _init(value: Vector3) -> void:
    received = value
    FileAccess.open("res://typed_constructor_called", FileAccess.WRITE).store_string("called")
'''

UNCACHED = '''@tool
class_name UncachedGenericWidget
extends Node3D

static func _static_init() -> void:
    FileAccess.open("res://unexpected_load", FileAccess.WRITE).store_string("loaded")

func _init() -> void:
    FileAccess.open("res://unexpected_construction", FileAccess.WRITE).store_string("constructed")
'''


class Client:
    def __init__(self, port):
        self.port = port
        self.url = f"http://127.0.0.1:{port}/mcp"
        self.session = None
        self.sequence = 0
        self.profile_label = "setup"
        self.profile_labels = []
        self.initialized = self.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "generic-tests", "version": "1"}})

    def rpc(self, method, params):
        self.sequence += 1
        headers = {"Content-Type": "application/json"}
        if self.session:
            headers["MCP-Session-Id"] = self.session
        body = json.dumps({"jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params}).encode()
        request = urllib.request.Request(self.url, body, headers)
        with urllib.request.urlopen(request, timeout=90) as response:
            self.session = response.headers.get("MCP-Session-Id", self.session)
            result = json.load(response)
        assert "error" not in result, result
        return result["result"]

    def tool(self, name, code, mode="read", timeout=None):
        arguments = {"code": code}
        if name == "execute":
            arguments["mode"] = mode
            arguments["verbose"] = True
        if timeout is not None:
            arguments["timeoutMs"] = timeout
        self.profile_labels.append(self.profile_label)
        result = self.rpc("tools/call", {"name": name, "arguments": arguments})
        if "structuredContent" in result:
            return result["structuredContent"]
        return json.loads(result["content"][0]["text"])

    def execute(self, code, mode="read", timeout=None):
        return self.tool("execute", code, mode, timeout)

    def value(self, code, mode="read"):
        result = self.execute(code, mode)
        assert result["ok"], result
        return result["result"]

    def search(self, code):
        result = self.tool("search", code)
        assert result["ok"], result
        return result["result"]


def literal(value):
    return json.dumps(value)


def exercise_passive_data(client, node):
    handle = literal(node)
    invalid = [
        '({get x(){touched++;return 1;}})',
        '({toJSON(){touched++;return {x:1};}})',
        'new Proxy({x:1},{ownKeys(){touched++;return ["x"];},get(){touched++;return 1;}})',
        '({get __godotHandle(){touched++;return "h1";}})',
        '({value:undefined})',
        '({value:()=>1})',
        '({value:Symbol("x")})',
        '({[Symbol("key")]:1})',
        '(()=>{const a={};a.self=a;return a;})()',
        '(()=>{const a=[];a[1]=2;return a;})()',
        'Object.defineProperty([1],"0",{get(){touched++;return 1;}})',
        'Object.defineProperty({},"hidden",{get(){touched++;return 1;}})',
        'Object.create({get x(){touched++;return 1;}})',
        '({value:Promise.resolve(1)})',
        'new Date()',
        '"á\\u0000ß"',
        '"\\ud800"',
    ]
    for expression in invalid:
        result = client.value(f'''
let touched=0;
const value={expression};
try {{ await godot.call({handle}, "set_meta", "passive_probe", value); return {{accepted:true,touched}}; }}
catch(error) {{ return {{accepted:false,touched,code:error.code}}; }}
''', "untracked")
        assert not result["accepted"] and result["touched"] == 0, (expression, result)
        assert result["code"] in {"INVALID_VALUE_ENCODING", "VALUE_DEPTH_LIMIT", "UNAWAITED_VALUE"}, result
    assert client.value(f'''
let touched=0;
const operations=Object.defineProperty([{{op:"get",target:{handle},prop:"position"}}],"0",{{get(){{touched++;return {{op:"get"}};}}}});
try {{ await godot.batch(operations); return false; }} catch(error) {{ return touched===0 && error.code==="INVALID_VALUE_ENCODING"; }}
''', "untracked")
    assert client.value(f'''
let touched=0;
try {{ await godot.call({handle},{{toString(){{touched++;return "peek";}}}}); return false; }}
catch(error) {{ return touched===0 && error.code==="INVALID_VALUE_ENCODING"; }}
''', "untracked")
    for expression in invalid:
        result = client.execute(f'let touched=0; return {expression};')
        assert not result["ok"], (expression, result)
        assert result["diagnostics"][0]["code"] in {"INVALID_VALUE_ENCODING", "UNAWAITED_VALUE"}, result
    round_trip = client.value('return {text:"á𐍈ß",items:[null,true,0,"ok"]};')
    assert round_trip == {"text": "á𐍈ß", "items": [None, True, 0, "ok"]}, round_trip
    assert client.value('return Object.assign(Object.create(null),{x:1});') == {"x": 1}
    assert client.value('return;') is None


def without_math_tags(value):
    named = {name for name, _, category, _ in VARIANT_FIXTURES if category == "named"}
    if isinstance(value, list):
        return [without_math_tags(item) for item in value]
    if isinstance(value, dict):
        return {key: without_math_tags(item) for key, item in value.items() if key != "$t" or value.get("$t") not in named}
    return value


def exercise_variant_matrix(client, widget):
    for name, enum, category, sample in VARIANT_FIXTURES:
        if category in {"opaque", "object"}:
            sample = client.value(f'return await godot.call({widget}, "native_fixture", {literal(name)});', "untracked")
        output = client.value(f'return await godot.call({widget}, "echo_{enum.lower()}", {literal(sample)});', "untracked")
        if category == "opaque":
            assert output.get("__godotValue") and output["type"] == name, (name, output)
            continue
        if category == "object":
            assert output["__godotHandle"] == sample["__godotHandle"], (name, output)
            continue
        expected = {"$t": name, "value": sample} if category == "string_like" else {"$t": name, "values": sample} if category == "packed" else sample
        assert without_math_tags(output) == expected, (name, output, expected)
        if category == "named":
            assert output["$t"] == name, output
            repeated = client.value(f'return await godot.call({widget}, "echo_{enum.lower()}", {literal(output)});', "untracked")
            assert repeated == output, (name, repeated, output)
            for bad in [list(sample.values()), {key: item for key, item in sample.items() if key != next(iter(sample))}, dict(sample, misspelled=1)]:
                failure = client.execute(f'return await godot.call({widget}, "echo_{enum.lower()}", {literal(bad)});', "untracked")
                assert not failure["ok"] and not failure["effects"], (name, bad, failure)
    for name in ("Vector2i", "Vector3i", "Vector4i"):
        enum, sample = next((enum, sample) for actual, enum, _, sample in VARIANT_FIXTURES if actual == name)
        for bad_value in (1.5, 2147483648, -2147483649, True, "1"):
            bad = dict(sample, x=bad_value)
            assert not client.execute(f'return await godot.call({widget}, "echo_{enum.lower()}", {literal(bad)});', "untracked")["ok"]
    for name in ("int", "bool", "String", "float"):
        bad_values = ["1", True, 1.5, 9007199254740992] if name == "int" else [0, "true", None] if name == "bool" else [1, {}, None] if name == "String" else ["1.5", True, None]
        for value in bad_values:
            assert not client.execute(f'return await godot.call({widget}, "echo_{name.lower()}", {literal(value)});', "untracked")["ok"], (name, value)
    for expression, expected in [
        ("9223372036854775807n", "9223372036854775807"),
        ("-9223372036854775808n", "-9223372036854775808"),
        ('({$t:"int",value:"9223372036854775807"})', "9223372036854775807"),
    ]:
        output = client.value(f'return await godot.call({widget}, "echo_int", {expression});', "untracked")
        assert output == {"$t": "int", "value": expected}, output
    for expression in ['9223372036854775808n', '-9223372036854775809n', '({$t:"int",value:"01"})', '({$t:"int",value:"-0"})', '({$t:"int",value:"1e3"})']:
        failure = client.execute(f'return await godot.call({widget}, "echo_int", {expression});', "untracked")
        assert not failure["ok"] and not failure["effects"], failure
    for expression, name in [("NaN", "NaN"), ("Infinity", "+Infinity"), ("-Infinity", "-Infinity"), ("-0", "-0")]:
        output = client.value(f'return await godot.call({widget}, "echo_float", {expression});', "untracked")
        assert output == {"$t": "float", "value": name}, output
        assert client.value(f'return await godot.call({widget}, "echo_float", {literal(output)});', "untracked") == output
        assert client.value(f'return {expression};') == output
    for name in ("StringName", "NodePath"):
        source = "not/a/real/node" if name == "NodePath" else "named_value"
        tag = {"$t": name, "value": source}
        assert client.value(f'return await godot.call({widget},"untyped_echo",{literal(tag)});', "untracked") == tag
        enum = "string_name" if name == "StringName" else "node_path"
        assert client.value(f'return await godot.call({widget},"echo_{enum}",{literal(tag)});', "untracked") == tag
    for method in ("echo_dictionary", "untyped_echo"):
        value = {"x": 1, "y": 2, "z": 3}
        assert client.value(f'return await godot.call({widget}, "{method}", {literal(value)});', "untracked") == value
    assert client.value(f'return await godot.call({widget}, "echo_color", {{r:2,g:0.5,b:0}});', "untracked") == {"$t": "Color", "r": 2, "g": 0.5, "b": 0, "a": 1}
    assert client.value(f'return await godot.call({widget}, "echo_color", {{r:2,g:0.5,b:0,a:0.25}});', "untracked")["a"] == 0.25
    for bad in ['{r:1,b:1}', '{r:1,g:0,b:0,a:"1"}', '{r:1e40,g:0,b:0}', '{r:NaN,g:0,b:0}']:
        assert not client.execute(f'return await godot.call({widget}, "echo_color", {bad});', "untracked")["ok"]
    assert not client.execute(f'return await godot.call({widget}, "echo_quaternion", {{$t:"Vector4",x:0,y:0,z:0,w:1}});', "untracked")["ok"]
    signed = {"x": {"x": 2, "y": 1, "z": 0}, "y": {"x": 0, "y": -3, "z": 0}, "z": {"x": 0, "y": 0, "z": 4}}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "echo_basis", {literal(signed)});', "untracked")) == signed
    nonunit = {"x": 1, "y": 2, "z": 3, "w": 4}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "echo_quaternion", {literal(nonunit)});', "untracked")) == nonunit
    plane = {"normal": {"x": 0, "y": 3, "z": 0}, "d": -2}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "echo_plane", {literal(plane)});', "untracked")) == plane
    for method, value in [("echo_rect2", {"position": {"x": 1, "y": 2}, "size": {"x": -3, "y": -4}}), ("echo_aabb", {"position": {"x": 1, "y": 2, "z": 3}, "size": {"x": -1, "y": -2, "z": -3}})]:
        assert without_math_tags(client.value(f'return await godot.call({widget}, "{method}", {literal(value)});', "untracked")) == value
    projection = {axis: dict(zip(("x", "y", "z", "w"), range(index * 4 + 1, index * 4 + 5))) for index, axis in enumerate(("x", "y", "z", "w"))}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "known_projection");', "untracked")) == projection
    assert without_math_tags(client.value(f'return await godot.call({widget}, "projection_column", {literal(projection)});', "untracked")) == projection["y"]
    assert without_math_tags(client.value(f'return await godot.call({widget}, "basis_point", {literal(signed)});', "untracked")) == {"x": 2, "y": -5, "z": 12}
    transform = {"basis": signed, "origin": {"x": 2, "y": 3, "z": 4}}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "transform_point", {literal(transform)});', "untracked")) == {"x": 4, "y": -2, "z": 16}
    transform_2d = {"x": {"x": 2, "y": 1}, "y": {"x": -1, "y": 3}, "origin": {"x": 4, "y": 5}}
    assert without_math_tags(client.value(f'return await godot.call({widget}, "transform_2d_point", {literal(transform_2d)});', "untracked")) == {"x": 5, "y": 9}
    nested_invalid = [
        ("rect2", {"position": {"x": 0, "y": "bad"}, "size": {"x": 1, "y": 2}}, "args[0].position.y"),
        ("rect2i", {"position": {"x": 0, "y": 0}, "size": {"x": 1.5, "y": 2}}, "args[0].size.x"),
        ("aabb", {"position": {"x": 0, "y": 0, "z": 0}, "size": {"x": 1, "y": 2, "z": False}}, "args[0].size.z"),
        ("plane", {"normal": {"x": 0, "y": "bad", "z": 0}, "d": 1}, "args[0].normal.y"),
        ("transform3d", {"basis": {"x": {"x": 1, "y": 0, "z": 0}, "y": {"x": 0, "y": 1, "z": 0}, "z": {"x": 0, "y": "bad", "z": 1}}, "origin": {"x": 0, "y": 0, "z": 0}}, "args[0].basis.z.y"),
    ]
    for method, invalid, path in nested_invalid:
        failed = client.execute(f'return await godot.call({widget}, "echo_{method}", {literal(invalid)});', "untracked")
        diagnostic = failed["diagnostics"][0]
        assert not failed["ok"] and not failed["effects"], failed
        assert diagnostic["code"] == "TYPE_MISMATCH" and diagnostic["path"] == path, failed
        assert diagnostic["member"] == "echo_" + method and diagnostic["argument"] == 0, failed
        assert "bad" not in literal(diagnostic), diagnostic
    value = client.value(f'return await godot.call({widget}, "nonfinite_vector");', "untracked")
    assert value["type"] == "Vector3" and value.get("__godotValue"), value
    repeated = client.value(f'return await godot.call({widget}, "echo_vector3", {literal(value)});', "untracked")
    assert repeated["type"] == "Vector3" and repeated.get("__godotValue"), repeated
    print("Variant codec: all 39 rows round-tripped; numeric and named-structure rejection cases passed", flush=True)


def exercise_integration_contracts(client, project, node, widget):
    types = client.search('return api.types();')
    assert [item["type"] for item in types] == [item[0] for item in VARIANT_FIXTURES], types
    assert all("contextual_format" in item and "output_format" in item for item in types)
    for name in ("RID", "Callable", "Signal"):
        info = client.search(f'return api.describeType("{name}");')
        assert info["opaque_only"] and info["explicit_format"] is None, info
    assert client.search('return api.describeType("NoSuchBuiltin")==null;')
    tools = client.rpc("tools/list", {})["tools"]
    assert {tool["name"] for tool in tools} == {"search", "execute"}, tools
    text = " ".join(tool["description"] for tool in tools)
    assert all(term in text for term in ("api.types", "api.describeType", "checkpoint", "UNAWAITED_VALUE", "16 KiB")), text
    defaults = client.search('const methods=api.describe("GenericMcpWidget").methods; return [methods.find(m=>m.name==="default_vector").arguments[0],methods.find(m=>m.name==="default_callable").arguments[0]];')
    assert defaults[0]["default"] == {"$t": "Vector3", "x": 1, "y": 2, "z": 3} and defaults[0]["default_status"] == "encoded", defaults
    assert defaults[1]["default_status"] == "unavailable", defaults
    assert not (project / "typed_constructor_called").exists()
    assert client.search('return api.describe("GenericTypedConstructor")!=null;')
    failure = client.execute('return await godot.create("GenericTypedConstructor",{x:1,y:"bad",z:3});', "untracked")
    assert not failure["ok"] and not failure["effects"] and not (project / "typed_constructor_called").exists(), failure
    constructed = client.value('return await godot.create("GenericTypedConstructor",{x:1,y:2,z:3});', "untracked")
    assert client.value(f'return await godot.get({literal(constructed)},"received");', "untracked") == {"$t": "Vector3", "x": 1, "y": 2, "z": 3}
    assert client.value('return await godot.call("GenericMcpWidget","static_vector",{x:1,y:2,z:3});', "untracked") == {"$t": "Vector3", "x": 1, "y": 2, "z": 3}
    assert not client.execute('return await godot.call("GenericMcpWidget","static_vector",{x:1,y:"bad",z:3});', "untracked")["ok"]
    original = client.value(f'return await godot.get({literal(node)},"position");')
    moved = {"$t": "Vector3", "x": 21, "y": 22, "z": 23}
    client.value(f'await godot.set({literal(node)},"position",{literal(moved)});', "write")
    for action, expected in (("undo", original), ("redo", moved)):
        (project / (action + "_request")).write_text(action)
        deadline = time.monotonic() + 15
        while not (project / (action + "_ready")).exists():
            assert time.monotonic() < deadline, action
            time.sleep(0.02)
        assert client.value(f'return await godot.get({literal(node)},"position");') == expected
    victim = client.value('const root=await godot.editor.getEditedScene();const n=await godot.create("Node3D");await godot.set(n,"name","RollbackVictim");await godot.call(root,"add_child",n);return n;', "write")
    (project / "destroy_rollback").write_text("destroy owned fixture node after mutation")
    failure = client.execute(f'await godot.set({literal(victim)},"position",{{x:777,y:1,z:2}});for(let i=0;i<50;i++) await godot.get({literal(victim)},"position");throw new Error("rollback probe");', "write")
    assert not failure["ok"] and any(d["code"] == "ROLLBACK_PARTIAL" for d in failure["diagnostics"]), failure
    locked = client.execute(f'await godot.set({literal(node)},"position",{{x:1,y:2,z:3}});', "write")
    assert locked["diagnostics"][0]["code"] == "TX_LOCKED", locked
    assert client.value('return await godot.unlockWrites();', "write")["was_locked"]
    client.value(f'await godot.set({literal(node)},"position",{literal(original)});', "write")
    deleted = client.execute('const node=await godot.create("Node3D");checkpoint("node",node);await godot.call(node,"queue_free");throw new Error("after deletion");', "untracked")
    assert deleted["partial"] and len(deleted["checkpoints"]) == 1, deleted
    stale = client.execute(f'return await godot.get({literal(deleted["checkpoints"][0]["value"])},"position");')
    assert stale["diagnostics"][0]["code"] == "INVALID_HANDLE", stale
    exact = 65536 - len(json.dumps([{"name": "b", "value": ""}], separators=(",", ":")))
    assert client.execute(f'checkpoint("b","x".repeat({exact}));')["ok"]
    failed = client.execute(f'checkpoint("b","x".repeat({exact + 1}));')
    assert failed["diagnostics"][0]["code"] == "CHECKPOINT_LIMIT" and not failed["checkpoints"], failed
    envelope = client.execute(f'checkpoint("padding","p".repeat(64000));await godot.batch(Array.from({{length:450}},()=>({{op:"call",target:{widget},method:{literal(LONG_EFFECT)},args:[]}})));throw new Error("primary_budget_error");', "untracked")
    assert not envelope["ok"] and envelope["diagnostics"][0]["message"] == "primary_budget_error", envelope
    assert envelope["truncated"] and envelope["effects_omitted"] > 0 and len(envelope["effects"]) + envelope["effects_omitted"] == 450, envelope
    assert len(json.dumps(envelope, separators=(",", ":"), ensure_ascii=False).encode()) <= 262144


def exercise_reference_restart(client, project):
    node = client.value('return await godot.create("GenericMcpWidget");', "untracked")
    reference = client.value(f'return await godot.call({literal(node)},"large_strings",300);', "untracked")
    (project / "restart_request").write_text("restart")
    deadline = time.monotonic() + 30
    while not (project / "restart_ready").exists():
        assert time.monotonic() < deadline, "Fixture service restart timed out"
        time.sleep(0.05)
    fresh = Client(client.port)
    assert fresh.execute(f'return await godot.get({literal(node)},"position");')["diagnostics"][0]["code"] == "INVALID_HANDLE"
    assert fresh.execute(f'return await godot.values.inspect({literal(reference)});')["diagnostics"][0]["code"] == "INVALID_VALUE_REFERENCE"
    assert fresh.value('return await godot.requestTier("untracked");')["granted"]
    new_node = fresh.value('return await godot.create("GenericMcpWidget");', "untracked")
    new_reference = fresh.value(f'return await godot.call({literal(new_node)},"large_strings",300);', "untracked")
    assert node["__godotHandle"] != new_node["__godotHandle"] and reference["__godotValue"] != new_reference["__godotValue"]
    assert fresh.value(f'return await godot.values.inspect({literal(new_reference)});')["size"] == 300
    assert fresh.execute(f'return await godot.get({literal(node)},"position");')["diagnostics"][0]["code"] == "INVALID_HANDLE"


def exercise_reference_quotas(client, widget):
    pair = literal(client.value(f'return await godot.call({widget},"page_retention_pair");', "untracked"))
    base = client.value(f'return await godot.call({widget},"large_strings",257);', "untracked")
    ref = literal(base)
    before = client.value(f'return await godot.values.inspect({ref});')
    failure = client.execute(f'return await godot.call({widget},"large_bytes",{9 << 20});', "untracked")
    assert failure["diagnostics"][0]["code"] == "VALUE_REFERENCE_LIMIT", failure
    assert client.value(f'return await godot.values.inspect({ref});')["retained_references"] == before["retained_references"]
    large = []
    for _ in range(9):
        result = client.execute(f'return await godot.call({widget},"large_bytes",{1 << 20});', "untracked")
        if not result["ok"]:
            assert result["diagnostics"][0]["code"] == "VALUE_REFERENCE_LIMIT", result
            break
        large.append(result["result"])
    else:
        raise AssertionError("Aggregate retained-memory quota was not enforced")
    assert large
    client.value(f'return await Promise.all({literal(large)}.map(ref=>godot.values.release(ref)));', "untracked")
    metadata = client.value(f'return await godot.values.inspect({ref});')
    retained = []
    remaining = metadata["reference_limit"] - metadata["retained_references"]
    while remaining:
        count = min(450, remaining)
        rows = client.value(f'return await godot.batch(Array.from({{length:{count}}},()=>({{op:"call",target:{widget},method:"large_strings",args:[257]}})));', "untracked")
        assert all(row["ok"] for row in rows), rows[-1]
        retained.extend(row["result"] for row in rows)
        remaining -= count
    failure = client.execute(f'return await godot.call({widget},"large_strings",257);', "untracked")
    assert failure["diagnostics"][0]["code"] == "VALUE_REFERENCE_LIMIT", failure
    assert client.value(f'return await godot.values.inspect({ref});')["size"] == 257
    first = literal(retained.pop())
    client.value(f'await godot.values.release({first});', "untracked")
    assert client.execute(f'return await godot.values.inspect({first});')["diagnostics"][0]["code"] == "INVALID_VALUE_REFERENCE"
    checkpoint = client.value(f'return await godot.values.inspect({ref});')["retained_references"]
    failed_page = client.execute(f'return await godot.values.read({pair},{{offset:0,limit:2}});')
    assert not failed_page["ok"] and failed_page["diagnostics"][0]["code"] == "VALUE_REFERENCE_LIMIT", failed_page
    assert client.value(f'return await godot.values.inspect({ref});')["retained_references"] == checkpoint
    nested = client.value(f'return await godot.values.read({pair},{{offset:0,limit:1}});')["items"][0]
    assert client.value(f'return await godot.values.inspect({literal(nested)});')["size"] == 20000
    assert client.value(f'return await godot.values.inspect({ref});')["size"] == 257
    client.value(f'return await godot.values.release({literal(nested)});', "untracked")
    retained.append(client.value(f'return await godot.call({widget},"large_strings",257);', "untracked"))
    for start in range(0, len(retained), 450):
        client.value(f'return await Promise.all({literal(retained[start:start + 450])}.map(ref=>godot.values.release(ref)));', "untracked")
    assert client.value(f'return await godot.values.inspect({ref});')["size"] == 257


def exercise_checkpoints(client, node, widget):
    result = client.execute('const value={items:[1]}; checkpoint("same",value); value.items.push(2); checkpoint("same",value); throw new Error("later");')
    assert not result["ok"] and not result["partial"] and result["result"] is None, result
    assert result["checkpoints"] == [{"name": "same", "value": {"items": [1]}}, {"name": "same", "value": {"items": [1, 2]}}], result
    assert client.execute('return 1;')["checkpoints"] == []
    for name in ('""', '"x".repeat(129)', '"é".repeat(65)', '42'):
        failure = client.execute(f'checkpoint("kept",1); checkpoint({name},2);')
        assert failure["diagnostics"][0]["code"] == "CHECKPOINT_LIMIT" and len(failure["checkpoints"]) == 1, failure
    assert client.execute('checkpoint("é".repeat(64),1);')["ok"]
    assert client.execute('for(let i=0;i<32;i++) checkpoint("x",i);')["ok"]
    failure = client.execute('for(let i=0;i<33;i++) checkpoint("x",i);')
    assert failure["diagnostics"][0]["code"] == "CHECKPOINT_LIMIT" and len(failure["checkpoints"]) == 32, failure
    failure = client.execute('checkpoint("kept",1); checkpoint("big","x".repeat(65536));')
    assert failure["diagnostics"][0]["code"] == "CHECKPOINT_LIMIT" and len(failure["checkpoints"]) == 1, failure
    assert client.execute('checkpoint("data","x".repeat(64000));')["ok"]
    assert client.execute('let value=1; for(let i=0;i<32;i++) value={value}; checkpoint("depth",value);')["ok"]
    failure = client.execute('checkpoint("kept",1); let value=1; for(let i=0;i<33;i++) value={value}; checkpoint("depth",value);')
    assert failure["diagnostics"][0]["code"] == "CHECKPOINT_LIMIT" and len(failure["checkpoints"]) == 1, failure
    for expression in ('Object.defineProperty({},"x",{enumerable:true,get(){touched++;return 1;}})', '(()=>{const x={};x.self=x;return x;})()', '({pending:Promise.resolve(1)})', '({fn(){touched++;}})', '({x:undefined})'):
        result = client.execute(f'let touched=0; checkpoint("kept",1); try {{checkpoint("bad",{expression});}} catch(error) {{return {{touched,code:error.code}};}}')
        assert result["ok"] and result["result"]["touched"] == 0 and len(result["checkpoints"]) == 1, result
        assert result["result"]["code"] in {"INVALID_VALUE_ENCODING", "UNAWAITED_VALUE"}, result
    result = client.execute(f'checkpoint("node",{literal(node)});')
    assert result["checkpoints"][0]["value"] == node and result["result"] is None, result
    original = client.value(f'return await godot.get({literal(node)},"position");')
    failed = client.execute(f'await godot.set({literal(node)},"position",{{x:8,y:9,z:10}}); checkpoint("temporary",await godot.get({literal(node)},"position")); await godot.set({literal(node)},"position",{{x:1,y:"bad",z:3}});', "write")
    assert not failed["ok"] and not failed["partial"] and failed["checkpoints"][0]["value"]["x"] == 8, failed
    assert client.value(f'return await godot.get({literal(node)},"position");') == original
    adopted = client.execute(f'return godot.call({literal(node)},"get_class");')
    assert adopted["ok"] and adopted["result"] == "Node3D", adopted
    missing = client.execute(f'return {{pending:godot.call({literal(node)},"set_meta","promise_effect",true)}};', "untracked")
    assert missing["diagnostics"][0]["code"] == "UNAWAITED_VALUE" and missing["partial"] and len(missing["effects"]) == 1, missing
    assert client.value(f'return await godot.call({literal(node)},"get_meta","promise_effect");') is True
    assert client.execute('await Promise.resolve();')["result"] is None
    assert not client.execute('return {fn(){}};')["ok"]
    oversized = client.execute('return "x".repeat(262144);')
    assert not oversized["ok"] and oversized["diagnostics"][0]["code"] == "OUTPUT_LIMIT", oversized


def exercise_collections(client, node, widget):
    vector = {"x": 1, "y": 2, "z": 3}
    array = client.value(f'return await godot.call({widget},"echo_typed_array",[{literal(vector)}]);', "untracked")
    assert isinstance(array, list) and without_math_tags(array) == [vector], array
    empty = client.value(f'return await godot.call({widget},"echo_typed_array",[]);', "untracked")
    assert empty == [], empty
    assert client.value(f'return await godot.call({widget},"untyped_echo",{literal(empty)});', "untracked") == empty
    typed_dictionary = {"$t": "Dictionary", "entries": [{"key": 1, "value": vector}]}
    dictionary = client.value(f'return await godot.call({widget},"echo_typed_dictionary",{literal(typed_dictionary)});', "untracked")
    assert dictionary["key_type"]["type"] == "int" and dictionary["value_type"]["type"] == "Vector3", dictionary
    assert without_math_tags(dictionary["entries"]) == [{"key": 1, "value": vector}], dictionary
    assert client.value(f'return await godot.call({widget},"untyped_echo",{literal(dictionary)});', "untracked") == dictionary
    escaped = {"$t": "Dictionary", "entries": [{"key": "$t", "value": "literal"}, {"key": "__godotHandle", "value": "not-a-handle"}, {"key": "__godotValue", "value": "not-a-reference"}, {"key": "__proto__", "value": {"safe": True}}]}
    assert client.value(f'return await godot.call({widget},"echo_dictionary",{literal(escaped)});', "untracked") == escaped
    for method, value in [
        ("echo_typed_array", {"$t": "Array", "element_type": {"type": "Vector2"}, "values": []}),
        ("echo_typed_array", [{"x": 1, "y": "wrong", "z": 3}]),
        ("echo_typed_dictionary", {"1": vector}),
        ("echo_dictionary", {"$t": "Dictionary", "entries": [{"key": 1, "value": "a"}, {"key": {"$t": "int", "value": "1"}, "value": "b"}]}),
        ("echo_dictionary", {"$t": "Dictionary", "key_type": {"type": "StringName"}, "entries": [{"key": "a", "value": 1}, {"key": {"$t": "StringName", "value": "a"}, "value": 2}]}),
    ]:
        failure = client.execute(f'return await godot.call({widget},"{method}",{literal(value)});', "untracked")
        assert not failure["ok"] and not failure["effects"], failure
    widget_array = client.value(f'return await godot.call({widget},"echo_widget_array",[{widget}]);', "untracked")
    assert isinstance(widget_array, list) and len(widget_array) == 1, widget_array
    replay = client.value(f'return await godot.call({widget},"echo_widget_array",{literal(widget_array)});', "untracked")
    assert replay == widget_array, replay
    assert not client.execute(f'return await godot.call({widget},"echo_widget_array",[{literal(node)}]);', "untracked")["ok"]
    packed_invalid = {"PackedByteArray": [0, 256], "PackedInt32Array": [1, 2147483648], "PackedInt64Array": [1, 9007199254740992], "PackedFloat32Array": [1, 1e40], "PackedFloat64Array": [1, "wrong"], "PackedStringArray": ["ok", 1], "PackedVector2Array": [{"x": 1, "y": 2}, {"x": 1, "y": "wrong"}], "PackedVector3Array": [vector, {"x": 1, "y": 2}], "PackedColorArray": [{"r": 1, "g": 0, "b": 0}, {"r": 1, "g": "wrong", "b": 0}], "PackedVector4Array": [{"x": 1, "y": 2, "z": 3, "w": 4}, {"x": 1, "y": 2, "z": 3}]}
    for name, values in packed_invalid.items():
        enum = next(enum.lower() for actual, enum, _, _ in VARIANT_FIXTURES if actual == name)
        failure = client.execute(f'return await godot.call({widget},"echo_{enum}",{literal(values)});', "untracked")
        assert not failure["ok"] and not failure["effects"], (name, failure)
        assert "[1]" in failure["diagnostics"][0]["path"], failure
    for name in ("PackedFloat32Array", "PackedFloat64Array"):
        enum = next(enum.lower() for actual, enum, _, _ in VARIANT_FIXTURES if actual == name)
        result = client.value(f'return await godot.call({widget},"echo_{enum}",[NaN,Infinity,-Infinity,-0]);', "untracked")
        assert result["values"] == [{"$t": "float", "value": item} for item in ("NaN", "+Infinity", "-Infinity", "-0")], result
    result = client.value(f'return await godot.call({widget},"echo_packed_int64_array",[-9223372036854775808n,9223372036854775807n]);', "untracked")
    assert result["values"] == [{"$t": "int", "value": item} for item in ("-9223372036854775808", "9223372036854775807")], result


def exercise_value_references(client, node, widget):
    reference = client.value(f'return await godot.call({widget},"large_strings",300);', "untracked")
    assert reference.get("__godotValue") and reference["type"] == "PackedStringArray", reference
    ref = literal(reference)
    metadata = client.value(f'return await godot.values.inspect({ref});')
    assert metadata["type"] == "PackedStringArray" and metadata["size"] == 300 and metadata["readable"], metadata
    forged = dict(reference, type="RID")
    assert client.value(f'return await godot.values.inspect({literal(forged)});')["type"] == "PackedStringArray"
    page = client.value(f'return await godot.values.read({ref},{{offset:2,limit:5}});')
    assert page["items"] == ["entry"] * 5 and page["count"] == 5 and page["next_offset"] == 7, page
    assert len(literal(page).encode()) <= 16384, page
    assert client.value(f'return await godot.values.read({ref},{{offset:300,limit:1}});')["count"] == 0
    for options in [{"offset": -1}, {"offset": 301}, {"limit": 0}, {"limit": 257}, {"offset": 0.5}, {"unknown": 1}]:
        assert not client.execute(f'return await godot.values.read({ref},{literal(options)});')["ok"], options
    alias = client.value(f'return await godot.call({widget},"echo_packed_string_array",{ref});', "untracked")
    assert alias.get("__godotValue"), alias
    for script in [f'return await godot.get({ref},"size");', f'return await godot.values.inspect({literal(node)});']:
        failure = client.execute(script)
        assert failure["diagnostics"][0]["code"] == "REFERENCE_KIND_MISMATCH", failure
    for mode in ("read", "write"):
        assert not client.execute(f'return await godot.values.release({ref});', mode)["ok"]
    assert client.value(f'return await godot.values.release({ref});', "untracked")["released"]
    assert client.execute(f'return await godot.values.inspect({ref});')["diagnostics"][0]["code"] == "INVALID_VALUE_REFERENCE"
    assert client.value(f'return await godot.values.inspect({literal(alias)});')["size"] == 300
    outer = client.value(f'return await godot.call({widget},"large_item");', "untracked")
    inner = client.value(f'return await godot.values.read({literal(outer)},{{offset:0,limit:1}});')["items"][0]
    assert inner.get("__godotValue") and inner["type"] == "String", inner
    assert client.value(f'return await godot.values.inspect({literal(inner)});')["size"] == 20000
    assert client.value(f'return await godot.values.read({literal(inner)},{{offset:0,limit:3}});')["items"] == ["x"] * 3
    cycle = client.value(f'return await godot.call({widget},"native_cycle");', "untracked")
    assert cycle.get("__godotValue") and cycle["type"] == "Array", cycle
    nested = client.value(f'return await godot.values.read({literal(cycle)},{{offset:0,limit:1}});')["items"][0]
    assert nested.get("__godotValue") and nested["type"] == "Array", nested
    client.value(f'await godot.call({widget},"clear_cycle",{literal(cycle)});', "untracked")
    assert client.value(f'return await godot.values.inspect({literal(nested)});')["size"] == 0
    for name in ("RID", "Callable", "Signal"):
        opaque = client.value(f'return await godot.call({widget},"native_fixture",{literal(name)});', "untracked")
        assert not client.value(f'return await godot.values.inspect({literal(opaque)});')["readable"]
        failure = client.execute(f'return await godot.values.read({literal(opaque)});')
        assert failure["diagnostics"][0]["code"] == "UNSUPPORTED_VALUE_OPERATION", failure
        assert client.value(f'return await godot.call({widget},"echo_{name.lower()}",{literal(opaque)});', "untracked")["type"] == name
    for name, fake in [("RID", 123), ("Callable", {"$t": "Callable", "target": node, "method": "peek"}), ("Signal", {"$t": "Signal", "name": "renamed"})]:
        failure = client.execute(f'return await godot.call({widget},"echo_{name.lower()}",{literal(fake)});', "untracked")
        assert not failure["ok"] and not failure["effects"], failure
    nonfinite = client.value(f'return await godot.call({widget},"nonfinite_vector");', "untracked")
    fields = client.value(f'return await godot.values.read({literal(nonfinite)});')["fields"]
    assert fields == {"x": {"$t": "float", "value": "NaN"}, "y": {"$t": "float", "value": "+Infinity"}, "z": {"$t": "float", "value": "-Infinity"}}, fields
    assert client.value(f'return await godot.get({literal(node)},"position");')["$t"] == "Vector3"


def exercise_page_boundaries(client, widget):
    for width in (1, 9, 11, 1200):
        reference = literal(client.value(f'return await godot.call({widget},"unicode_strings",{width},300);', "untracked"))
        expected = 'é漢\n\t"\\' * width
        for limit in (16, 64, 256):
            for offset in (0, 299, 300):
                oracle = client.value(f'return await godot.call({widget},"page_size_oracle",{reference},{offset},{limit});', "untracked")
                page = client.value(f'return await godot.values.read({reference},{{offset:{offset},limit:{limit}}});')
                assert page["count"] == oracle["count"] and page["next_offset"] == oracle["next_offset"], (width, offset, limit, oracle, page["count"])
                assert page["items"] == [expected] * int(page["count"]), (width, offset, limit)
                assert page["truncated"] == (offset + page["count"] < 300) and oracle["bytes"] <= 16384, oracle
        assert client.value(f'return await godot.values.release({reference});', "untracked")["released"]
    token = 'é漢\n\t"\\'
    for width in (1, 9, 31):
        reference = literal(client.value(f'return await godot.call({widget},"unicode_dictionary",{width},300);', "untracked"))
        for limit in (16, 64, 256):
            for offset in (0, 150, 299, 300):
                oracle = client.value(f'return await godot.call({widget},"page_size_oracle",{reference},{offset},{limit});', "untracked")
                page = client.value(f'return await godot.values.read({reference},{{offset:{offset},limit:{limit}}});')
                assert page["count"] == oracle["count"] and page["next_offset"] == oracle["next_offset"] and oracle["bytes"] <= 16384, (width, offset, limit, oracle)
                assert page["entries"] == [{"key":str(index)+token,"value":token*width} for index in range(offset, offset+int(page["count"]))], (width, offset, limit)
        client.value(f'return await godot.values.release({reference});', "untracked")
    reference = literal(client.value(f'return await godot.call({widget},"page_rejection");', "untracked"))
    before = client.value(f'return await godot.values.inspect({reference});')
    page = client.value(f'return await godot.values.read({reference},{{offset:0,limit:3}});')
    assert page["count"] == 2 and page["next_offset"] == 2, page["count"]
    assert client.value(f'return await godot.values.inspect({reference});')["retained_references"] == before["retained_references"]
    nested = client.value(f'return await godot.values.read({reference},{{offset:2,limit:1}});')["items"][0]
    assert client.value(f'return await godot.values.inspect({literal(nested)});')["size"] == 20000
    client.value(f'await godot.values.release({literal(nested)});await godot.values.release({reference});', "untracked")
    print("Page boundaries: native JSON oracle, UTF-8/escapes, 16/64/256 limits, continuation and tentative retention rollback passed", flush=True)


def exercise_rotation_contract(client, widget):
    result = client.value(f'''
const editor=await godot.getSingleton("EditorInterface");
const root=await godot.call(editor,"get_base_control");
const parent=await godot.create("Node3D");
const node=await godot.create("Node3D");
await godot.call(root,"add_child",parent);
await godot.call(parent,"add_child",node);
await godot.set(parent,"position",{{x:10,y:0,z:0}});
await godot.set(node,"position",{{x:1,y:2,z:3}});
const global=await godot.get(node,"global_position");
await godot.set(node,"global_position",{{x:20,y:2,z:3}});
const local=await godot.get(node,"position");
await godot.set(node,"rotation_degrees",{{x:0,y:90,z:0}});
const radians=await godot.get(node,"rotation");
await godot.set(node,"rotation",{{x:0,y:Math.PI/2,z:0}});
const degrees=await godot.get(node,"rotation_degrees");
const orders=[];
for(const order of [0,5]) {{
  await godot.set(node,"rotation_order",order);
  await godot.set(node,"rotation",{{x:0.25,y:0.5,z:0.75}});
  const basis=await godot.get(node,"basis");
  const expected=await godot.call({widget},"euler_basis",{{x:0.25,y:0.5,z:0.75}},order);
  orders.push({{order:await godot.get(node,"rotation_order"),same:JSON.stringify(basis)===JSON.stringify(expected)}});
}}
await godot.set(node,"quaternion",{{x:0,y:0,z:0,w:1}});
const quaternion=await godot.get(node,"quaternion");
await godot.set(node,"basis",{{x:{{x:1,y:0,z:0}},y:{{x:0,y:1,z:0}},z:{{x:0,y:0,z:1}}}});
await godot.set(node,"transform",{{basis:{{x:{{x:1,y:0,z:0}},y:{{x:0,y:1,z:0}},z:{{x:0,y:0,z:1}}}},origin:{{x:1,y:2,z:3}}}});
const transform=await godot.get(node,"transform");
await godot.call(parent,"queue_free");
return {{global,local,radians,degrees,orders,quaternion,transform}};
''', "untracked")
    assert without_math_tags(result["global"]) == {"x": 11, "y": 2, "z": 3}, result
    assert without_math_tags(result["local"]) == {"x": 10, "y": 2, "z": 3}, result
    assert abs(result["radians"]["y"] - 1.5707963267948966) < 0.00001, result
    assert abs(result["degrees"]["y"] - 90) < 0.00001, result
    assert result["orders"] == [{"order": 0, "same": True}, {"order": 5, "same": True}], result
    assert without_math_tags(result["quaternion"]) == {"x": 0, "y": 0, "z": 0, "w": 1}, result
    assert without_math_tags(result["transform"]["origin"]) == {"x": 1, "y": 2, "z": 3}, result


def exercise_diagnostics(client, node, widget):
    handle = literal(node)
    for code, script, mode in [
        ("INVALID_HANDLE", 'return await godot.get({__godotHandle:"h_unknown"},"position");', "read"),
        ("INVALID_VALUE_REFERENCE", f'return await godot.call({widget},"echo_vector3",{{__godotValue:"v_unknown",type:"Vector3"}});', "untracked"),
        ("PERMISSION", f'await godot.set({handle},"position",{{x:1,y:2,z:3}});', "read"),
        ("TYPE_MISMATCH", f'return await godot.call({widget},"echo_vector3",{{x:1,y:"secret_argument",z:3}});', "untracked"),
    ]:
        result = client.execute(script, mode)
        assert not result["ok"] and result["diagnostics"][0]["code"] == code, result
        assert "secret_argument" not in literal(result["diagnostics"]), result
    mutated = client.execute(f'''
try {{ await godot.call({widget},"echo_vector3",{{x:1,y:"wrong",z:3}}); }}
catch(error) {{ error.code="FAKE_CODE"; error.message="fake message"; throw error; }}
''', "untracked")
    assert mutated["diagnostics"][0]["code"] == "TYPE_MISMATCH", mutated
    assert mutated["diagnostics"][0]["message"] != "fake message", mutated
    batch = client.value(f'return await godot.batch([{{op:"set",target:{handle},prop:"position",value:{{x:2,y:3,z:4}}}},{{op:"set",target:{handle},prop:"position",value:{{x:1,y:"wrong",z:3}}}},{{op:"get",target:{handle},prop:"position"}}]);', "untracked")
    assert batch[0]["ok"] and not batch[1]["ok"] and batch[2]["ok"], batch
    assert batch[1]["error"]["code"] == "TYPE_MISMATCH" and batch[1]["error"]["batch_index"] == 1, batch
    assert batch[1]["error"]["path"] == "value.y", batch
    assert without_math_tags(batch[2]["result"]) == {"x": 2, "y": 3, "z": 4}, batch
    result = client.execute('throw {get message(){while(true){}},toString(){while(true){}}};', timeout=500)
    assert not result["ok"] and result["diagnostics"][0]["code"] == "JS_ERROR", result
    unavailable = client.execute('throw "plain failure";')
    assert "source" not in unavailable["diagnostics"][0], unavailable
    assert client.value('return ["prepareStackTrace","stackTraceLimit"].every(name=>{const d=Object.getOwnPropertyDescriptor(Error,name);return !d.writable&&!d.configurable;}) && Error.stackTraceLimit===32 && Error.prepareStackTrace===undefined;')
    for script, line in [('throw new Error("first line");', 1), ('const x=1;\nconst y=2;\nconst z=x+y;\nthrow new Error("known line");', 4)]:
        failure = client.execute(script)
        source = failure["diagnostics"][0]["source"]
        assert source["line"] == line and "<execute>:" + str(line) in source["stack"], failure
    failure = client.execute(f'const x=1;\nconst y=2;\nconst z=x+y;\nawait godot.call({widget},"echo_vector3",{{x:1,y:"bad",z:3}});', "untracked")
    assert failure["diagnostics"][0]["source"]["line"] == 4, failure
    assert client.value(f'''
let touched=0;
Error.prepareStackTrace=()=>{{touched++;return "custom";}};
try {{ await godot.call({widget},"echo_vector3",{{x:1,y:"bad",z:3}}); }} catch(error) {{}}
try {{ await godot.call({widget},"set_meta","passive",{{get x(){{touched++;return 1;}}}}); }} catch(error) {{}}
return touched===0;
''', "untracked")
    assert client.value(f'''
let touched=0;
Error.stackTraceLimit={{valueOf(){{touched++;return 10;}}}};
try {{ await godot.call({widget},"echo_vector3",{{x:1,y:"bad",z:3}}); }} catch(error) {{}}
return touched===0;
''', "untracked")


def exercise_value_validation(client, node):
    handle = literal(node)
    original = client.value(f'return await godot.get({handle}, "position");', "untracked")
    invalid = [
        {"x": 1, "y": "two", "z": 3},
        {"x": 1, "y": 2},
        {"x": 1, "y": 2, "z": 3, "extra": 4},
        {"$t": "Vector3", "x": 1, "y": 2},
        {"$t": "FutureVector", "x": 1, "y": 2, "z": 3},
        [1, 2, 3],
    ]
    for value in invalid:
        result = client.execute(f'await godot.set({handle}, "position", {literal(value)});', "untracked")
        assert not result["ok"], (value, result)
        assert not result["effects"], result
        assert client.value(f'return await godot.get({handle}, "position");', "untracked") == original
    client.value(f'await godot.set({handle}, "position", {{x:1,y:2,z:3}});', "untracked")
    assert client.value(f'return await godot.get({handle}, "position");', "untracked") == {"$t": "Vector3", "x": 1, "y": 2, "z": 3}
    client.value(f'await godot.set({handle}, "position", {literal(original)});', "untracked")


def stage_discovery(client, project):
    config = configparser.ConfigParser()
    config.read(Path(__file__).resolve().parents[1] / "addons/godot_cm/plugin.cfg")
    assert client.initialized["serverInfo"]["version"] == config["plugin"]["version"].strip('"')
    assert client.search('return api.describe("EditorInterface").methods.some(m => m.name === "get_editor_viewport_3d") && api.describe("EditorInterface").methods.some(m => m.name === "stop_playing_scene");')
    schema = client.rpc("tools/list", {})
    execution = next(tool for tool in schema["tools"] if tool["name"] == "execute")
    discovery = next(tool for tool in schema["tools"] if tool["name"] == "search")
    assert "does not authorize" in discovery["description"]
    assert "Variant/untyped" in discovery["description"]
    declarations = (Path(__file__).resolve().parents[1] / "addons/godot_cm/api-surface.d.ts").read_text()
    assert "interface GodotTypeDescriptor" in declarations and "variant_type?: number | null" in declarations
    assert "untracked" in execution["inputSchema"]["properties"]["mode"]["enum"]
    assert client.search('return api.describe("ResourceLoader").methods.some(m => m.name === "load");')
    assert client.search('return api.describe("EditorInterface").methods.some(m => m.name === "play_current_scene");')
    assert client.search('return api.describe("GenericMcpWidget").kind === "script_class";')
    assert client.search('return api.describe("GenericMcpWidget").methods.some(m => m.name === "increment");')
    assert client.search('''
const widget = api.describe("GenericMcpWidget");
const echo = widget.methods.find(m => m.name === "typed_echo");
const untyped = widget.methods.find(m => m.name === "untyped_echo");
const points = widget.properties.find(p => p.name === "typed_points");
const lookup = widget.properties.find(p => p.name === "typed_lookup");
const position = api.describe("Node3D").properties.find(p => p.name === "position");
return echo.arguments[0].variant_type === 18 && echo.arguments[0].index === 0 &&
    untyped.arguments[0].type === "Variant" && untyped.mode === "untracked" &&
    echo.return.variant_type === 18 && echo.arguments[2].default === 1.5 &&
    points.variant_type === 28 && points.element_type.variant_type === 9 &&
    lookup.key_type.variant_type === 2 && lookup.value_type.variant_type === 9 &&
    position.variant_type === 9 && typeof position.usage === "number" && typeof position.hint === "number" &&
    typeof echo.is_vararg === "boolean";
''')
    assert client.search('return api.describe("UncachedGenericWidget").metadata_status === "class_only";')
    assert not (project / "unexpected_load").exists()
    assert not (project / "unexpected_construction").exists()
    assert not client.execute('return await godot.getSingleton("OS");', "untracked")["ok"]
    assert not client.execute('return await godot.getSingleton("ResourceLoader");')["ok"]


def stage_node_ops(client):
    node = client.value('return await godot.create("Node3D");', "write")
    handle = literal(node)
    assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 1);', "write")["ok"]
    for operation in [{"op": "create", "class": "Node3D", "write": False}, {"op": "set", "target": node, "prop": "name", "value": "Spoofed", "write": False}]:
        result = client.value('return await godot.batch(' + literal([operation]) + ');')
        assert not result[0]["ok"], result
    assert not client.execute(f'return await godot.connect({handle}, "renamed", {handle}, "call");', "write")["ok"]
    assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 1);', "untracked")["ok"]
    return node


def stage_tier_gate(client, denied, node):
    handle = literal(node)
    grant = client.value('return await godot.requestTier("untracked");')
    if denied:
        assert not grant["granted"]
        assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 2);', "untracked")["ok"]
        assert not client.value(f'return await godot.call({handle}, "has_meta", "generic_test");')
        return False
    assert grant["granted"]
    return True


def stage_tool_contract(client):
    tools = client.rpc("tools/list", {})
    desc = next(t["description"] for t in tools["tools"] if t["name"] == "execute")
    assert "own execute call" in desc, desc
    assert client.initialized.get("sessionId") == client.session, client.initialized


def stage_permissions(client, node):
    handle = literal(node)
    summary = client.value('return await godot.permissions();')
    groups = {g["name"]: g for g in summary["groups"]}
    assert groups["untracked"] and groups["execution"]["policy"] == "deny" and groups["execution"]["protected"], summary
    assert groups["config"]["policy"] == "write" and groups["capture"]["policy"] == "ask", summary
    assert "sandbox" in groups["execution"].get("why", ""), summary
    assert groups["capture"]["default"] == "ask" and groups["config"]["default"] == "write", summary
    # Unified groups: hard deny hints at the trusted script ops.
    denied = client.execute('return await godot.create("GDScript");', "untracked")
    assert not denied["ok"], denied
    rem = next((d.get("remediation", "") for d in denied.get("diagnostics", [])), "")
    assert "createScript" in rem, denied
    # Write-only groups: reads pass, writes stay denied with remediation.
    assert client.value('return await godot.call(await godot.getSingleton("ProjectSettings"), "get_setting", "application/config/name");') is not None
    write_denied = client.execute('return await godot.call(await godot.getSingleton("ProjectSettings"), "set_setting", "application/config/name", "probe");', "untracked")
    assert not write_denied["ok"], write_denied
    rem = next((d.get("remediation", "") for d in write_denied.get("diagnostics", [])), "")
    assert "writes denied" in rem, write_denied
    assert client.value('return await godot.requestTier("capture");')["granted"]
    helpers = client.search('return api.helpers().map(function(h){return h.name;});')
    for helper in ["godot.build", "godot.createScript", "godot.attachScript", "godot.pluginCall", "godot.scene.new", "checkpoint"]:
        assert helper in helpers, helpers
    children = client.value(f'return await godot.call({handle}, "get_children");', "untracked")
    assert isinstance(children, list), children
    raw = client.rpc("tools/call", {"name": "execute", "arguments": {"code": f'return await godot.call({handle}, "set_meta", "compact", 1);', "mode": "untracked"}})
    compact = json.loads(raw["content"][0]["text"])
    assert compact["ok"] and compact["effects"] == {"counts": {"call": 1}, "total": 1}, compact


def stage_policy_reload(client, project):
    # The addon directory is a symlink in non-isolated runs: preserve the
    # policy file and always restore it, or the working tree stays dirty.
    policy_file = project / "addons" / "godot_cm" / "allowlist.json"
    policy_original = policy_file.read_text()
    document = json.loads(policy_original)
    try:
        document["groups"]["filesystem"]["policy"] = "deny"
        policy_file.write_text(json.dumps(document))
        policy = ""
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            tiers = client.value('return await godot.permissions();')["groups"]
            policy = next((t["policy"] for t in tiers if t["name"] == "filesystem"), "")
            if policy == "deny":
                break
            time.sleep(0.2)
        assert policy == "deny", policy
        assert not client.value('return await godot.requestTier("filesystem");')["granted"]
        locked = client.execute('return await godot.create("FileAccess");', "untracked")
        assert not locked["ok"], locked
        rem = next((d.get("remediation", "") for d in locked.get("diagnostics", [])), "")
        assert "filesystem" in rem and "permanently" in rem, locked
        document["groups"]["filesystem"]["policy"] = "allow"
        policy_file.write_text(json.dumps(document))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if client.value('return await godot.requestTier("filesystem");')["granted"]:
                break
            time.sleep(0.2)
        assert client.value('return await godot.requestTier("filesystem");')["granted"]
    finally:
        policy_file.write_text(policy_original)
    # Wait for the restore reload: it relocks filesystem per the file's
    # "ask" policy, then re-grant through the auto-accepting dialog hook.
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        tiers = client.value('return await godot.permissions();')["groups"]
        if next((t["policy"] for t in tiers if t["name"] == "filesystem"), "") == "ask":
            break
        time.sleep(0.2)
    assert client.value('return await godot.requestTier("filesystem");')["granted"]


def stage_script_ops(client, project, node):
    handle = literal(node)
    source = "extends Node3D\nfunc created_via_mcp() -> int:\n    return 1\n"
    created = client.value(f'return await godot.createScript({literal("res://mcp_created.gd")}, {literal(source)});', "write")
    assert created["path"] == "res://mcp_created.gd" and (project / "mcp_created.gd").read_text() == source, created
    client.value(f'return await godot.attachScript({handle}, {literal("res://mcp_created.gd")});', "write")
    refused = client.execute(f'return await godot.createScript({literal("res://tool.gd")}, {literal("@tool\textends Node\n")});', "write")
    assert not refused["ok"], refused
    rolled = client.execute(f'await godot.createScript({literal("res://rollback.gd")}, {literal("extends Node\n")}); throw new Error("rollback");', "write")
    assert not rolled["ok"] and not (project / "rollback.gd").exists(), rolled


def stage_effects_rollback(client, project, node):
    handle = literal(node)
    assert client.value('''
const rf = api.describe("Image").constants.FORMAT_RF;
const image = await godot.call("Image", "create_from_data", 2, 2, false, rf, {$t:"PackedByteArray",values:[0,0,32,64,0,0,32,64,0,0,32,64,0,0,32,64]});
const bytes = await godot.call(image, "get_data");
if (bytes.$t !== "PackedByteArray" || bytes.values.length !== 16) throw new Error("Missing inline packed data");
const copy = await godot.call("Image", "create_from_data", 2, 2, false, rf, bytes);
const pixel = await godot.call(copy, "get_pixel", 0, 0);
return pixel.$t === "Color" && pixel.r === 2.5;
''', "untracked")
    result = client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 7); throw new Error("after effect");', "untracked")
    assert not result["ok"] and result["partial"] and not result["rollback_supported"], result
    assert len(result["effects"]) == 1 and result["effects"][0]["status"] == "completed", result
    assert client.value(f'return await godot.call({handle}, "get_meta", "generic_test");') == 7
    name = client.value(f'return await godot.get({handle}, "name");')
    result = client.execute(f'await godot.set({handle}, "name", "RolledBack"); throw new Error("rollback");', "write")
    assert not result["ok"] and result["rollback_supported"] and not result["effects"], result
    assert client.value(f'return await godot.get({handle}, "name");') == name
    root = client.value('return await godot.editor.getEditedScene();')
    client.value(f'await godot.call({literal(root)}, "add_child", {handle}); await godot.call({handle}, "set_owner", {literal(root)});', "write")
    result = client.execute(f'await godot.call({handle}, "set_owner", null); throw new Error("owner rollback");', "write")
    assert not result["ok"], result
    assert client.value(f'return await godot.get({handle}, "owner");')["__godotHandle"] == root["__godotHandle"]
    result = client.value(f'return await godot.batch([{{op:"call",target:{handle},method:"set_meta",args:["spoof",true],write:false}}]);')
    assert not result[0]["ok"], result
    assert not client.value(f'return await godot.call({handle}, "has_meta", "spoof");')
    result = client.value('return await godot.batch([{op:"tx_commit"}]);', "untracked")
    assert not result[0]["ok"], result
    assert not client.execute('return await godot.create("GenericMcpWidget");', "write")["ok"]


def stage_widget_setup(client):
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    assert client.value(f'return await godot.call({widget}, "constructor_count");', "untracked") == 1
    return widget


# Regression for the dock save race: two consecutive save_group_policy calls
# must compose — the second reads the file the first just wrote, not a stale
# in-memory policy document.
def stage_group_saves(client, project, widget):
    policy_file = project / "addons" / "godot_cm" / "allowlist.json"
    original = policy_file.read_text()
    try:
        assert client.value(f'return await godot.call({widget}, "save_group_policy", "capture", "deny");', "untracked")
        assert client.value(f'return await godot.call({widget}, "save_group_policy", "crypto", "deny");', "untracked")
        document = json.loads(policy_file.read_text())
        assert document["groups"]["capture"]["policy"] == "deny", document["groups"]["capture"]
        assert document["groups"]["crypto"]["policy"] == "deny", document["groups"]["crypto"]
        groups = {g["name"]: g for g in client.value('return await godot.permissions();')["groups"]}
        assert groups["capture"]["policy"] == "deny" and groups["crypto"]["policy"] == "deny", groups
        # Regression: a save must not reset untouched groups to the "ask"
        # fallback — the reload re-seeds tier_policy from the file eagerly.
        assert groups["config"]["policy"] == "write" and groups["network"]["policy"] == "allow", groups
        assert client.value(f'return await godot.call({widget}, "reset_group_policies");', "untracked")
        document = json.loads(policy_file.read_text())
        assert document["groups"]["capture"]["policy"] == "ask" and document["groups"]["crypto"]["policy"] == "ask", document["groups"]
        groups = {g["name"]: g for g in client.value('return await godot.permissions();')["groups"]}
        assert groups["capture"]["policy"] == "ask" and groups["crypto"]["policy"] == "ask", groups
        # Trust mode: every non-protected group goes "allow"; protected stays "deny".
        assert client.value(f'return await godot.call({widget}, "allow_all_groups");', "untracked")
        groups = {g["name"]: g for g in client.value('return await godot.permissions();')["groups"]}
        assert all(g["policy"] == "allow" for g in groups.values() if not g.get("protected")), groups
        assert groups["execution"]["policy"] == "deny" and groups["interop"]["policy"] == "deny", groups
        assert client.value(f'return await godot.call({widget}, "allow_all_groups", true);', "untracked")
        groups = {g["name"]: g for g in client.value('return await godot.permissions();')["groups"]}
        assert all(g["policy"] == "allow" for g in groups.values()), groups
        assert client.value(f'return await godot.call({widget}, "reset_group_policies");', "untracked")
    finally:
        policy_file.write_text(original)
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        groups = client.value('return await godot.permissions();')["groups"]
        if next((t["policy"] for t in groups if t["name"] == "capture"), "") == "ask":
            break
        time.sleep(0.2)
    # The storm flips untracked ask -> allow -> ask, so file-wins revokes the
    # session grant: re-grant for the stages that follow.
    assert client.value('return await godot.requestTier("untracked");')["granted"]


def stage_widget_contract(client, project, widget):
    client.value(f'await godot.set({widget}, "metadata_guard", true);', "untracked")
    for _ in range(3):
        assert client.search('return api.describe("GenericMcpWidget").properties.some(p => p.name === "typed_points");')
    assert not (project / "unexpected_property_probe").exists()
    assert not (project / "unexpected_load").exists()
    assert not (project / "unexpected_construction").exists()
    assert client.value(f'return await godot.call({widget}, "constructor_count");', "untracked") == 1
    assert client.value('return await godot.call("GenericMcpWidget", "static_identity", 17);', "untracked") == 17
    other = literal(client.value('return await godot.create("GenericMcpWidget", 11);', "untracked"))
    assert client.value(f'return await godot.get({other}, "counter");', "untracked") == 11
    assert not client.execute(f'return await godot.call({widget}, "get_mutating");')["ok"]
    assert client.value(f'return await godot.call({widget}, "peek");', "untracked") == 0
    assert client.value(f'return await godot.call({widget}, "increment", 3);', "untracked") == 3
    bad = client.execute(f'return await godot.call({widget}, "increment");', "untracked")
    assert not bad["ok"], bad
    assert client.value(f'return await godot.call({widget}, "peek");', "untracked") == 3
    delayed = client.execute(f'return await godot.call({widget}, "delay_mutate", 1200);', "untracked", timeout=500)
    assert not delayed["ok"] and delayed["partial"] and delayed["effects"], delayed
    assert delayed["diagnostics"][0]["code"] == "INTERRUPTED", delayed
    assert client.value(f'return await godot.call({widget}, "peek");', "untracked") == 4
    assert not client.execute(f'return await godot.call({widget}, "call", "increment", 5);', "untracked")["ok"]


def stage_loader(client):
    assert client.value('return await godot.requestTier("filesystem");')["granted"]
    loader = literal(client.value('return await godot.getSingleton("ResourceLoader");'))
    source = client.value(f'return await godot.call({loader}, "load", "res://anonymous.gd");', "untracked")
    assert source["script_class"] == "script:res://anonymous.gd", source
    assert client.search('return api.describe("script:res://anonymous.gd") != null;')
    anonymous = literal(client.value('return await godot.create("script:res://anonymous.gd", 23);', "untracked"))
    assert client.value(f'return await godot.get({anonymous}, "counter");', "untracked") == 23
    packed = literal(client.value(f'return await godot.call({loader}, "load", "res://blank.tscn");', "untracked"))
    instance = literal(client.value(f'return await godot.call({packed}, "instantiate");', "untracked"))
    assert client.value(f'return await godot.get({instance}, "name");') == {"$t": "StringName", "value": "GenericTestRoot"}
    return loader


def stage_tail(client, project, terrain):
    assert not client.execute('return 1;', "invalid")["ok"]
    assert bool(client.search('return api.describe("Terrain3D") != null;')) == terrain
    if terrain:
        assert client.search('''
const entry = api.describe("Terrain3DData");
const method = entry.methods.find(m => m.name === "import_images");
return entry.source === "extension" && method.arguments[0].variant_type === 28 &&
    method.arguments[0].element_type.class_name === "Image" && method.arguments[1].variant_type === 9;
''')
        created = client.execute('return await godot.create("Terrain3D");', "untracked")
        assert created["ok"], created
        terrain_exercise(client, project)


class BridgeSession:
    """Ordered contract stages over one live editor session.

    ensure(name) runs every pending stage in order up to and including the
    requested one, so any test can declare only its deepest dependency and
    still work under `pytest -k`. A denied run stops at the tier gate.
    """

    def __init__(self, client, project, denied=False, terrain=False):
        self.client = client
        self.project = project
        self.denied = denied
        self.terrain = terrain
        self.vars = {}
        self.done = set()
        self.blocked = False

    def ensure(self, stage):
        for name, run_stage in STAGES:
            if self.blocked:
                break
            if name in self.done:
                continue
            run_stage(self)
            self.done.add(name)
            if name == stage:
                break
        if stage not in self.done and not self.denied:
            raise AssertionError(f"session blocked before stage '{stage}'")


STAGES = [
    ("discovery", lambda s: stage_discovery(s.client, s.project)),
    ("node_ops", lambda s: s.vars.update(node=stage_node_ops(s.client))),
    ("tier_gate", lambda s: setattr(s, "blocked", not stage_tier_gate(s.client, s.denied, s.vars["node"]))),
    ("tool_contract", lambda s: stage_tool_contract(s.client)),
    ("permissions", lambda s: stage_permissions(s.client, s.vars["node"])),
    ("policy_reload", lambda s: stage_policy_reload(s.client, s.project)),
    ("script_ops", lambda s: stage_script_ops(s.client, s.project, s.vars["node"])),
    ("passive_data", lambda s: exercise_passive_data(s.client, s.vars["node"])),
    ("value_validation", lambda s: exercise_value_validation(s.client, s.vars["node"])),
    ("effects_rollback", lambda s: stage_effects_rollback(s.client, s.project, s.vars["node"])),
    ("widget_setup", lambda s: s.vars.update(widget=stage_widget_setup(s.client))),
    ("group_saves", lambda s: stage_group_saves(s.client, s.project, s.vars["widget"])),
    ("variant_matrix", lambda s: exercise_variant_matrix(s.client, s.vars["widget"])),
    ("diagnostics", lambda s: exercise_diagnostics(s.client, s.vars["node"], s.vars["widget"])),
    ("rotation_contract", lambda s: exercise_rotation_contract(s.client, s.vars["widget"])),
    ("collections", lambda s: exercise_collections(s.client, s.vars["node"], s.vars["widget"])),
    ("value_references", lambda s: exercise_value_references(s.client, s.vars["node"], s.vars["widget"])),
    ("page_boundaries", lambda s: exercise_page_boundaries(s.client, s.vars["widget"])),
    ("checkpoints", lambda s: exercise_checkpoints(s.client, s.vars["node"], s.vars["widget"])),
    ("reference_quotas", lambda s: exercise_reference_quotas(s.client, s.vars["widget"])),
    ("integration_contracts", lambda s: exercise_integration_contracts(s.client, s.project, s.vars["node"], s.vars["widget"])),
    ("widget_contract", lambda s: stage_widget_contract(s.client, s.project, s.vars["widget"])),
    ("loader", lambda s: s.vars.update(loader=stage_loader(s.client))),
    ("catalog_freshness", lambda s: exercise_catalog_freshness(s.client, s.vars["loader"], s.vars["widget"], s.project)),
    ("tail", lambda s: stage_tail(s.client, s.project, s.terrain)),
]


def exercise(client, project, denied=False, terrain=False):
    BridgeSession(client, project, denied, terrain).ensure(STAGES[-1][0])


def exercise_catalog_freshness(client, loader, widget, project):
    script = literal(client.value(f'return await godot.call({loader},"load","res://reload_target.gd");', "untracked"))
    node = literal(client.value('return await godot.create("script:res://reload_target.gd");', "untracked"))
    identity = client.value(f'return await godot.call({widget},"script_identity",{script});', "untracked")
    assert client.value(f'return await godot.call({node},"accept",{{x:1,y:2,z:3}});', "untracked") == "vector"
    source = '@tool\nextends Node3D\nfunc accept(value: Color) -> String:\n    return "color"\n'
    rows = client.value(f'''return await godot.batch([
        {{op:"call",target:{widget},method:"replace_script_source",args:[{script},{literal(source)}]}},
        {{op:"call",target:{widget},method:"reload_script",args:[{script}]}},

        {{op:"call",target:{node},method:"accept",args:[{{r:1,g:0,b:0}}]}}
    ]);''', "untracked")
    assert all(row["ok"] for row in rows), rows
    assert rows[1]["result"] == 0 and rows[2]["result"] == "color", rows
    assert client.value(f'return await godot.call({widget},"script_identity",{script});', "untracked") == identity
    assert client.search('return api.describe("script:res://reload_target.gd").methods.find(m=>m.name==="accept").arguments[0].variant_type === 20;')
    rejected = client.execute(f'return await godot.call({node},"accept",{{x:1,y:2,z:3}});', "untracked")
    assert not rejected["ok"] and rejected["diagnostics"][0]["code"] == "INVALID_VALUE_ENCODING" and rejected["diagnostics"][0]["path"] == "args[0].x", rejected
    replacement = literal(client.value(f'return await godot.call({loader},"load","res://reload_replacement.gd");', "untracked"))
    assert client.value(f'await godot.call({node},"set_script",{replacement});return await godot.get({node},"counter");', "untracked") == 7
    permissions = client.value(f'''return await godot.batch([
        {{op:"call",target:{widget},method:"set_test_policy",args:["filesystem","deny"]}},
        {{op:"call",target:{loader},method:"has_cached",args:["res://anonymous.gd"]}},
        {{op:"call",target:{widget},method:"set_test_policy",args:["filesystem","allow"]}},
        {{op:"call",target:{loader},method:"has_cached",args:["res://anonymous.gd"]}}
    ]);''', "untracked")
    assert [row["ok"] for row in permissions] == [True, False, True, True], permissions
    assert permissions[1]["error"]["code"] == "PERMISSION" and permissions[3]["result"], permissions
    mutable = client.execute('''const node=await godot.create("Node3D");const value={x:1,y:2,z:3};
        await godot.set(node,"position",value);value.y="wrong";
        try {await godot.set(node,"position",value);} catch(error) {return {code:error.code,position:await godot.get(node,"position")};}
        throw new Error("mutable value reused validation");''', "untracked")
    assert mutable["ok"] and mutable["result"]["code"] == "TYPE_MISMATCH" and mutable["result"]["position"]["y"] == 2, mutable
    victim = literal(client.value('return await godot.create("Node3D");', "untracked"))
    deleted = client.value(f'return await godot.batch([{{op:"call",target:{widget},method:"destroy_node",args:[{victim}]}},{{op:"get",target:{victim},prop:"position"}}]);', "untracked")
    assert deleted[0]["ok"] and not deleted[1]["ok"] and deleted[1]["error"]["code"] == "INVALID_HANDLE", deleted
    late = project / "late_added.gd"
    late.write_text('@tool\nclass_name LateAddedWidget\nextends Node3D\n')
    for expected in (True, False):
        if not expected:
            late.unlink()
        client.value(f'await godot.call({widget},"update_script","res://late_added.gd");await godot.call({widget},"scan_scripts");return true;', "untracked")
        limit = time.monotonic() + 15
        while client.value(f'return await godot.call({widget},"scan_busy");', "untracked") and time.monotonic() < limit:
            time.sleep(0.05)
        while time.monotonic() < limit:
            found = client.search('return api.describe("LateAddedWidget") != null;')
            if found == expected:
                break
            time.sleep(0.05)
        assert found == expected, {"late_class_expected": expected, "found": found, "registered": client.value(f'return await godot.call({widget},"registered_script","res://late_added.gd");', "untracked"), "metadata": client.search('const d=api.describe("LateAddedWidget");return d?{status:d.metadata_status,path:d.path}:null;')}
    print("Catalog freshness: same-identity reload, in-batch conversion, script replacement and filesystem add/remove passed", flush=True)


def terrain_exercise(client, project):
    print("Terrain3D collision constants:", client.search('return api.describe("Terrain3DCollision").constants;'))
    result = client.execute('''
const vec = (x,y,z) => ({$t:"Vector3",x,y,z});
const root = await godot.create("Node3D");
await godot.set(root, "name", "GenericTerrainPhysicsTest");
const edited = await godot.editor.getEditedScene();
await godot.call(edited, "add_child", root);
async function node(cls, name, parent = root) {
    const item = await godot.create(cls);
    await godot.set(item, "name", name);
    await godot.call(parent, "add_child", item);
    await godot.call(item, "set_owner", root);
    return item;
}
const terrain = await node("Terrain3D", "Terrain");
await godot.set(terrain, "region_size", 256);
if (await godot.call("DirAccess", "make_dir_recursive_absolute", "res://terrain_data") !== 0) throw new Error("mkdir failed");
await godot.set(terrain, "data_directory", "res://terrain_data");
const constants = api.describe("Terrain3DCollision").constants;
const full = Object.keys(constants).find(k => k.endsWith("FULL_GAME"));
if (!full) throw new Error("Missing full-game collision mode");
if (api.describe("Terrain3D").properties.some(p => p.name === "collision_mode")) {
    await godot.set(terrain, "collision_mode", constants[full]);
} else {
    const collision = await godot.call(terrain, "get_collision");
    await godot.call(collision, "set_mode", constants[full]);
}
const buffer = new ArrayBuffer(64 * 64 * 4);
const view = new DataView(buffer);
for (let z = 0; z < 64; z++) for (let x = 0; x < 64; x++) {
    const r = Math.hypot(x - 32, z - 32);
    view.setFloat32((z * 64 + x) * 4, r < 24 ? 4 * (1 + Math.cos(Math.PI * r / 24)) : 0, true);
}
const bytes = {$t:"PackedByteArray", values:Array.from(new Uint8Array(buffer))};
const image = await godot.call("Image", "create_from_data", 64, 64, false, api.describe("Image").constants.FORMAT_RF, bytes);
if (!image) throw new Error("Image allocation failed");
const data = await godot.call(terrain, "get_data");
await godot.call(data, "import_images", [image, null, null], vec(0,0,0), 0, 1);
const height = await godot.call(data, "get_height", vec(32,0,32));
if (!(height > 7)) throw new Error("Heightmap import failed: " + height);
await godot.call(data, "save_directory", "res://terrain_data");
const ball = await node("RigidBody3D", "Ball");
await godot.set(ball, "position", vec(36,8.3,32));
const material = await godot.create("PhysicsMaterial");
await godot.set(material, "friction", 1);
await godot.set(ball, "physics_material_override", material);
const shapeNode = await node("CollisionShape3D", "Collision", ball);
const shape = await godot.create("SphereShape3D");
await godot.set(shape, "radius", 0.75);
await godot.set(shapeNode, "shape", shape);
const meshNode = await node("MeshInstance3D", "Mesh", ball);
const mesh = await godot.create("SphereMesh");
await godot.set(mesh, "radius", 0.75);
await godot.set(mesh, "height", 1.5);
await godot.set(meshNode, "mesh", mesh);
const camera = await node("Camera3D", "Camera");
await godot.set(camera, "position", vec(75,45,85));
await godot.call(camera, "look_at", vec(36,2,32), vec(0,1,0));
await godot.set(camera, "current", true);
const light = await node("DirectionalLight3D", "Sun");
await godot.set(light, "rotation_degrees", vec(-55,-25,0));
const loader = await godot.getSingleton("ResourceLoader");
const observer = await godot.call(loader, "load", "res://physics_observer.gd");
await godot.call(root, "set_script", observer);
const packed = await godot.create("PackedScene");
if (await godot.call(packed, "pack", root) !== 0) throw new Error("pack failed");
const saver = await godot.getSingleton("ResourceSaver");
if (await godot.call(saver, "save", packed, "res://generic_terrain_test.tscn") !== 0) throw new Error("save failed");
await godot.call(edited, "remove_child", root);
const editor = await godot.getSingleton("EditorInterface");
await godot.call(editor, "play_custom_scene", "res://generic_terrain_test.tscn");
return {height};
''', "untracked", timeout=60000)
    assert result["ok"], result
    deadline = time.monotonic() + 45
    ready = False
    while time.monotonic() < deadline:
        ready = client.value('return await godot.call("FileAccess", "file_exists", "res://physics_samples.json");', "untracked")
        if ready:
            break
        time.sleep(0.2)
    editor = literal(client.value('return await godot.getSingleton("EditorInterface");'))
    client.value(f'await godot.call({editor}, "stop_playing_scene");', "untracked")
    assert ready, "No runtime physics samples"
    samples = json.loads(client.value('return await godot.call("FileAccess", "get_file_as_string", "res://physics_samples.json");', "untracked"))
    first, last = samples[0], samples[-1]
    distance = ((last["x"] - first["x"]) ** 2 + (last["z"] - first["z"]) ** 2) ** 0.5
    drop = first["y"] - last["y"]
    assert distance > 15 and drop > 5 and last["y"] > 0.5, (first, last)
    assert max(sample["spin"] for sample in samples) > 1, samples[-1]
    print(f"Terrain3D native MCP physics passed: {distance:.2f} m traveled, {drop:.2f} m descended, {len(samples)} samples")


def summarize_samples(records):
    durations = sorted(record["elapsed_ms"] for record in records if record["ok"])
    return {
        "samples": records,
        "median_ms": statistics.median(durations) if durations else None,
        "p95_ms": durations[math.ceil(len(durations) * 0.95) - 1] if durations else None,
        "failed": sum(not record["ok"] for record in records),
    }


def measure_benchmark(operation, warmup, samples, validate=lambda value: True, cleanup=None, clock=time.perf_counter):
    def sample():
        response = None
        started = clock()
        try:
            response = operation()
            record = {"ok": bool(response.get("ok")), "codes": [item.get("code", "UNKNOWN") for item in response.get("diagnostics", [])]}
        except Exception as error:
            record = {"ok": False, "codes": [type(error).__name__]}
        record["elapsed_ms"] = round((clock() - started) * 1000, 6)
        if response is not None:
            value = response.get("result")
            record["encoded_result_bytes"] = len(json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode())
            if record["ok"]:
                try:
                    if not validate(value):
                        record.update(ok=False, codes=["INVALID_RESULT"])
                except Exception as error:
                    record.update(ok=False, codes=["VALIDATION_" + type(error).__name__])
            if cleanup and response.get("ok"):
                try:
                    cleanup(value)
                except Exception as error:
                    record["ok"] = False
                    record["codes"].append("CLEANUP_" + type(error).__name__)
        return record
    warmups = [sample() for _ in range(warmup)]
    return dict(summarize_samples([sample() for _ in range(samples)]), warmups=warmups)


def summarize_reports(reports):
    groups = {}
    for report in reports:
        addon = report["addon"]
        key = (addon["source"], report["topology"], report["environment"]["profiling_requested"], tuple(sorted(addon["binary_sha256"].items())))
        group = groups.setdefault(key, {"source": key[0], "topology": key[1], "profiling": key[2], "binary_sha256": addon["binary_sha256"], "rounds": 0, "metrics": {}, "native": {}})
        group["rounds"] += 1
        group.setdefault("runs", []).append({name: report.get(name) for name in ("round", "activation", "idle", "main_thread_occupancy_ms", "main_thread_occupancy_scope")})
        group["runs"][-1]["round_metrics"] = {name: {key: metric.get(key) for key in ("median_ms", "p95_ms", "failed")} for name, metric in report["metrics"].items()}
        if "rendered_frames" in report:
            group["runs"][-1]["rendered_frames"] = {name: report["rendered_frames"].get(name) for name in ("median_ms", "p95_ms", "max_ms", "continuous_process_frame_max_ms", "continuous_process_frame_p95_ms", "frames_drawn", "scope")}
        for name, metric in report["metrics"].items():
            group["metrics"].setdefault(name, []).extend(metric["samples"])
        seen = {}
        for record in report.get("native_profile", {}).get("records", []):
            case = record.get("benchmark_case")
            if case not in report["metrics"]:
                continue
            seen[case] = seen.get(case, 0) + 1
            if seen[case] <= report.get("warmup", 0):
                continue
            values = group["native"].setdefault(case, {})
            for name, value in record["metrics"].items():
                values.setdefault(name, []).append(value)
    result = []
    for group in groups.values():
        for name, records in group["metrics"].items():
            summary = summarize_samples(records)
            summary.pop("samples")
            group["metrics"][name] = dict(summary, sample_count=len(records))
        group["native"] = {case: {name: statistics.median(values) for name, values in metrics.items()} for case, metrics in group["native"].items()}
        result.append(group)
    return result


def benchmark_schedule(baseline, rounds):
    return [(number, source) for number in range(rounds) for source in (([baseline, None] if number % 2 == 0 else [None, baseline]) if baseline else [None])]


def benchmark_legacy(client):
    assert client.value('return await godot.requestTier("untracked");')["granted"]
    node = literal(client.value('return await godot.create("Node3D");', "untracked"))
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    metrics = {}
    def sample(name, code, mode="read"):
        durations = []
        for _ in range(5):
            started = time.perf_counter()
            value = client.value(code, mode)
            durations.append(round((time.perf_counter() - started) * 1000, 3))
        metrics[name] = {"median_ms": statistics.median(durations), "samples_ms": durations, "encoded_bytes": len(json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode())}
        return value
    modern = client.value('return typeof godot.values === "object";')
    batch_target = node if modern else literal({"$handle": int(json.loads(node)["__godotHandle"][1:])})
    batch = sample("batch_100", f'return await godot.batch(Array.from({{length:100}},()=>({{op:"get",target:{batch_target},prop:"position"}})));')
    metrics["batch_100"]["target_format"] = "object handle" if modern else "legacy internal $handle (public nested handles are broken in 0.2.1)"
    assert len(batch) == 100 and all(row["ok"] for row in batch), batch[:3]
    small = sample("packed_128", f'return await godot.call({widget},"large_strings",128);', "untracked")
    metrics["packed_128"]["inline"] = "values" in small
    reference = sample("packed_10000", f'return await godot.call({widget},"large_strings",10000);', "untracked")
    assert "__godotValue" in reference, reference
    page = sample("script_page_256", f'return await godot.call({widget},"page_strings",{literal(reference)},0,256);', "untracked")
    assert page == ["entry"] * 256
    if client.value('return typeof godot.values === "object";'):
        page = sample("passive_page_256", f'return await godot.values.read({literal(reference)},{{offset:0,limit:256}});')
        assert page["items"] == ["entry"] * 256 and metrics["passive_page_256"]["encoded_bytes"] <= 16384
    else:
        metrics["passive_page_256"] = {"available": False}
    print("Benchmark: " + json.dumps(metrics, sort_keys=True), flush=True)
    return metrics


def verify_worker_semantics(client):
    assert client.value('return await godot.requestTier("untracked");')["granted"]
    node = client.value('return await godot.create("Node3D");', "untracked")
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    exercise_passive_data(client, node)
    exercise_checkpoints(client, node, widget)
    payload = '''const value={bounds:[-9223372036854775808n,9223372036854775807n],
        specials:[NaN,Infinity,-Infinity,-0],numbers:[-2147483648,2147483647,2147483648,0.1,1.2345678901234567,1e-200,1e200],
        unicode:"é漢\\n\\t\\\"\\\\",reserved:{$t:"literal",__godotHandle:"data",__godotValue:"data"}};
        checkpoint("payload",value);return value;'''
    result = client.execute(payload)
    assert result["ok"] and len(result["checkpoints"]) == 1, result
    assert result["result"]["bounds"] == [{"$t":"int","value":str(value)} for value in (-9223372036854775808,9223372036854775807)], result
    assert result["result"]["specials"] == [{"$t":"float","value":value} for value in ("NaN","+Infinity","-Infinity","-0")], result
    assert result["result"]["reserved"] == {"$t":"literal","__godotHandle":"data","__godotValue":"data"}, result
    fast = client.execute('const value={integers:[-2147483648,0,2147483647],unicode:"é漢",nested:{items:[true,null,"escaped\\\\"]},bounds:[-9223372036854775808n,9223372036854775807n]};checkpoint("fast",value);return value;')
    assert fast["ok"] and fast["result"]["integers"] == [-2147483648,0,2147483647], fast
    for code in ("return undefined;", "await Promise.resolve();"):
        void = client.execute(code)
        assert void["ok"] and void["result"] is None and not void["checkpoints"], void
    assert client.value('return typeof globalThis.value === "undefined";')
    canonical = {name: {key: record[key] for key in ("result","checkpoints","partial","truncated")} for name, record in (("floating_fallback",result),("passive_reuse",fast))}
    print("Worker semantic comparison payload verified", flush=True)
    return canonical


def benchmark(client, args):
    assert client.value('return await godot.requestTier("untracked");')["granted"]
    node = literal(client.value('return await godot.create("Node3D");', "untracked"))
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    reference = client.value(f'return await godot.call({widget},"large_strings",10000);', "untracked")
    dictionary = client.value(f'return await godot.call({widget},"large_dictionary",10000);', "untracked")
    assert "__godotValue" in reference and "__godotValue" in dictionary
    unicode = client.value(f'return await godot.call({widget},"unicode_strings",9,10000);', "untracked") if not args.benchmark_case or "unicode_page_256" in args.benchmark_case else None
    large = client.value(f'return await godot.call({widget},"page_rejection");', "untracked") if not args.benchmark_case or "large_page_3" in args.benchmark_case else None
    cases = {}
    for count in (1, 10, 100, 500):
        cases[f"batch_{count}"] = (f'return await godot.batch(Array.from({{length:{count}}},()=>({{op:"get",target:{node},prop:"position"}})));', "read", lambda value, count=count: len(value) == count and all(item["ok"] and item["result"]["$t"] == "Vector3" for item in value), False)
    cases["native_get"] = (f'return await godot.get({node},"position");', "read", lambda value: value["$t"] == "Vector3", False)
    cases["native_set"] = (f'await godot.set({node},"position",{{x:1,y:2,z:3}});return await godot.get({node},"position");', "write", lambda value: value["x"] == 1 and value["z"] == 3, False)
    cases["native_call"] = (f'return await godot.call({node},"get_position");', "read", lambda value: value["$t"] == "Vector3", False)
    cases["native_block_30ms"] = (f'return await godot.call({widget},"benchmark_sleep",30);', "untracked", lambda value: value == 30, False)
    cases["mixed_batch"] = (f'return await godot.batch([{{op:"get",target:{node},prop:"position"}},{{op:"get",target:{node},prop:"unknown_benchmark_property"}},{{op:"get",target:{node},prop:"name"}}]);', "read", lambda value: len(value) == 3 and [item["ok"] for item in value] == [True, False, True], False)
    cases["packed_128"] = (f'return await godot.call({widget},"large_strings",128);', "untracked", lambda value: len(value["values"]) == 128, False)
    cases["packed_10000"] = (f'return await godot.call({widget},"large_strings",10000);', "untracked", lambda value: "__godotValue" in value, True)
    for count in (16, 64, 256):
        cases[f"passive_page_{count}"] = (f'return await godot.values.read({literal(reference)},{{offset:0,limit:{count}}});', "read", lambda value, count=count: value["items"] == ["entry"] * count and value["count"] == count, False)
    for offset in (0, 9000):
        cases[f"dictionary_page_{offset}"] = (f'return await godot.values.read({literal(dictionary)},{{offset:{offset},limit:256}});', "read", lambda value, offset=offset: value["offset"] == offset and value["count"] == 256 and len(value["entries"]) == 256, False)
    cases["unicode_page_256"] = (f'return await godot.values.read({literal(unicode)},{{offset:0,limit:256}});', "read", lambda value: 0 < value["count"] < 256 and value["items"] == ['é漢\n\t"\\' * 9] * int(value["count"]), False)
    cases["large_page_3"] = (f'return await godot.values.read({literal(large)},{{offset:0,limit:3}});', "read", lambda value: value["count"] == 2 and value["next_offset"] == 2 and value["items"] == ["p" * 15815, "f" * 200], False)
    cases["worker_strings"] = ('const values=Array.from({length:1024},()=>"é漢".repeat(4));checkpoint("worker",values);return values;', "read", lambda value: value == ["é漢" * 4] * 1024, False)
    cases["worker_integers"] = ('const values=Array.from({length:1024},(_,i)=>({index:i,value:i%7}));checkpoint("worker",values);return values;', "read", lambda value: len(value) == 1024 and value[1023] == {"index":1023,"value":1}, False)
    metrics = {}
    def release(value):
        if isinstance(value, dict) and "__godotValue" in value:
            previous = client.profile_label
            client.profile_label = "cleanup"
            try:
                assert client.value(f'return await godot.values.release({literal(value)});', "untracked")["released"]
            finally:
                client.profile_label = previous
    marker = "PROFILE_PRIVATE_CONTENT_SENTINEL"
    assert client.value(f'const value = {{payload:{literal(marker)}}};checkpoint({literal(marker)}, value);return await godot.call({widget},"echo_dictionary",value);', "untracked")["payload"] == marker
    try:
        unknown = set(args.benchmark_case or ()) - cases.keys()
        if unknown:
            raise ValueError("unknown benchmark cases: " + ", ".join(sorted(unknown)))
        for name, (code, mode, validate, owned) in cases.items():
            if args.benchmark_case and name not in args.benchmark_case:
                continue
            client.profile_label = name
            metrics[name] = measure_benchmark(lambda code=code, mode=mode: client.execute(code, mode, args.benchmark_timeout), args.benchmark_warmup, args.benchmark_samples, validate, release if owned else None)
            print("Benchmark case: " + json.dumps({"name": name, "median_ms": metrics[name]["median_ms"], "p95_ms": metrics[name]["p95_ms"], "failed": metrics[name]["failed"]}), flush=True)
    finally:
        release(reference)
        release(dictionary)
        release(unicode)
        release(large)
    return metrics


def parse_performance_log(output, limit=512):
    names = set('''refresh_calls global_discovery scene_nodes script_reflections catalog_publications
        cache_hits cache_misses json_encodings json_bytes page_items value_retains value_releases
        request_inclusive_ns context_inclusive_ns queue_wait_ns catalog_inclusive_ns maintenance_inclusive_ns validation_inclusive_ns
        conversion_inclusive_ns native_inclusive_ns encode_inclusive_ns page_inclusive_ns final_response_inclusive_ns pump_inclusive_ns dictionary_skip_ns
        startup_inclusive_ns json_inclusive_ns process_pump_max_ns js_passive_decode_inclusive_ns maintenance_global_discovery maintenance_script_reflections catalog_live catalog_peak script_cache_entries worker_result_reuse worker_result_parse'''.split())
    names |= {name.replace("_inclusive_ns", "_exclusive_ns") for name in names if name.endswith("_inclusive_ns")}
    result = {"records": [], "omitted": 0, "invalid": 0, "process_pump_max_ns": 0, "latest_gauges": {}}
    prefix = "[godot-cm-perf] "
    for line in output.splitlines():
        if not line.startswith(prefix):
            continue
        try:
            if len(line) > 8192:
                raise ValueError("oversized profile record")
            record = json.loads(line[len(prefix):])
            valid = isinstance(record, dict) and set(record) == {"schema_version", "kind", "timing_semantics", "metrics"}
            valid = valid and type(record["schema_version"]) is int and record["schema_version"] == 1 and record["kind"] in {"execute", "search", "startup", "cumulative"}
            valid = valid and record["timing_semantics"] == "inclusive; cross-thread intervals can overlap"
            valid = valid and isinstance(record["metrics"], dict) and set(record["metrics"]).issubset(names) and all(type(value) is int and 0 <= value < 2**63 for value in record["metrics"].values())
            if not valid:
                raise ValueError("invalid profile record")
        except (ValueError, TypeError, RecursionError):
            result["invalid"] += 1
            continue
        result["process_pump_max_ns"] = max(result["process_pump_max_ns"], record["metrics"].get("process_pump_max_ns", 0))
        for gauge in ("catalog_live", "catalog_peak", "script_cache_entries"):
            if gauge in record["metrics"]:
                result["latest_gauges"][gauge] = record["metrics"][gauge]
        if len(result["records"]) < limit:
            result["records"].append(record)
        else:
            result["omitted"] += 1
    return result


def verify_catalog_cost(profile, warmups):
    seen = {}
    checked = 0
    for record in profile["records"]:
        case = record.get("benchmark_case", "")
        if not case.startswith("batch_"):
            continue
        seen[case] = seen.get(case, 0) + 1
        if seen[case] <= warmups:
            continue
        values = record["metrics"]
        discovery = values["global_discovery"] - values["maintenance_global_discovery"]
        reflection = values["script_reflections"] - values["maintenance_script_reflections"]
        assert discovery == 0 and reflection == 0, {"case": case, "operation_global_discovery": discovery, "operation_script_reflections": reflection}
        checked += 1
    assert checked, "no measured batch profile records were available for the cost regression"


def verify_page_cost(profile, warmups):
    seen = {}
    checked = 0
    for record in profile["records"]:
        case = record.get("benchmark_case", "")
        if not case.startswith("passive_page_"):
            continue
        seen[case] = seen.get(case, 0) + 1
        if seen[case] <= warmups:
            continue
        metrics = record["metrics"]
        assert metrics["json_encodings"] <= metrics["page_items"] + 6, {"case": case, "json_encodings": metrics["json_encodings"], "items": metrics["page_items"]}
        checked += 1
    assert checked, "no measured page profile records were available for the cost regression"


def process_cpu_ms(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) * 1000 / os.sysconf("SC_CLK_TCK")
    except (OSError, ValueError, IndexError):
        return None


def prepare_addon(repository, addons, baseline_tag=None, isolated=False):
    artifact = json.loads((baseline_tag / "identity.json").read_text()) if isinstance(baseline_tag, Path) else None
    revision = artifact["revision"] if artifact else subprocess.check_output(["git", "rev-parse", "--verify", "--end-of-options", (baseline_tag or "HEAD") + "^{commit}"], cwd=repository, text=True).strip()
    if artifact:
        shutil.copytree(baseline_tag / "godot_cm", addons / "godot_cm")
    elif baseline_tag:
        archive = subprocess.run(["git", "archive", revision, "addons/godot_cm"], cwd=repository, capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(archive)) as files:
            files.extractall(addons.parent, filter="data")
    elif isolated:
        shutil.copytree(repository / "addons/godot_cm", addons / "godot_cm")
    else:
        (addons / "godot_cm").symlink_to(repository / "addons/godot_cm", target_is_directory=True)
    addon = addons / "godot_cm"
    config = configparser.ConfigParser()
    config.read(addon / "plugin.cfg")
    identity = {
        "source": "artifact:" + hashlib.sha256(json.dumps(artifact["binary_sha256"], sort_keys=True).encode()).hexdigest()[:12] if artifact else baseline_tag or "working-tree",
        "revision": revision,
        "plugin_version": config["plugin"]["version"].strip('"'),
        "binary_sha256": {file.name: hashlib.sha256(file.read_bytes()).hexdigest() for file in sorted((addon / "bin").iterdir()) if file.is_file() and file.suffix in {".so", ".dll", ".dylib"}},
    }
    if artifact:
        assert identity["binary_sha256"] == artifact["binary_sha256"], "frozen benchmark artifact changed"
    return identity


def exercise_native_reload(client, project):
    assert client.search('return api.describe("PerfReloadTarget").methods.find(m=>m.name==="accept").arguments[0].variant_type === 9;')
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    os.replace(project / "libperf_next.so", project / "libperf.so")
    result = client.value(f'''const type = () => api.describe("PerfReloadTarget").methods.find(m=>m.name==="accept").arguments[0].variant_type;
        const before = type();
        const status = await godot.call({widget},"reload_native","res://perf_reload.gdextension");
        return {{before, after:type(), status}};''', "untracked")
    assert result == {"before": 9, "after": 9, "status": 0}, result
    assert client.search('return api.describe("PerfReloadTarget").methods.find(m=>m.name==="accept").arguments[0].variant_type === 20;')
    print("Catalog extension reload: native Vector3 -> Color; request snapshot stayed pinned", flush=True)


def exercise_catalog_cycles(client, pid):
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    script = literal(client.value('const loader=await godot.getSingleton("ResourceLoader");return await godot.call(loader,"load","res://reload_target.gd");', "untracked"))
    resident = []
    for index in range(24):
        kind = "Vector3" if index % 2 == 0 else "Color"
        source = f'@tool\nextends Node3D\nfunc accept(value: {kind}) -> String:\n    return "cycle"\n'
        result = client.value(f'''const type=()=>api.describe("script:res://reload_target.gd").methods.find(m=>m.name==="accept").arguments[0].variant_type;
            const before=type();await godot.call({widget},"replace_script_source",{script},{literal(source)});
            const status=await godot.call({widget},"reload_script",{script});return {{before,after:type(),status}};''', "untracked")
        assert result["status"] == 0 and result["before"] == result["after"], result
        assert client.search('return api.describe("script:res://reload_target.gd").methods.find(m=>m.name==="accept").arguments[0].variant_type;') == (9 if index % 2 == 0 else 20)
        if index >= 6 and index % 3 == 0:
            fields = Path(f"/proc/{pid}/statm").read_text().split()
            resident.append(int(fields[1]) * os.sysconf("SC_PAGE_SIZE") // 1024)
    assert max(resident[-4:]) - min(resident[-4:]) < 8192, resident
    print("Catalog cycles: 24 pinned snapshots; RSS KiB after warmup:", resident, flush=True)


def prepare_reload_extension(repository, project):
    template = '''#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/string.hpp>
using namespace godot;
class PerfReloadTarget : public RefCounted {
    GDCLASS(PerfReloadTarget, RefCounted)
protected:
    static void _bind_methods() { ClassDB::bind_method(D_METHOD("accept", "value"), &PerfReloadTarget::accept); }
public:
    String accept(VALUE_TYPE value) const { return "LABEL"; }
};
void initialize(ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) ClassDB::register_class<PerfReloadTarget>();
}
void terminate(ModuleInitializationLevel level) {}
extern "C" GDExtensionBool GDE_EXPORT perf_reload_init(GDExtensionInterfaceGetProcAddress address, GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
    GDExtensionBinding::InitObject init(address, library, initialization);
    init.register_initializer(initialize);
    init.register_terminator(terminate);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
'''
    for name, value_type, label in [("libperf.so", "Vector3", "vector"), ("libperf_next.so", "Color", "color")]:
        command = ["c++", "-std=c++17", "-O2", "-fPIC", "-shared", "-fvisibility=hidden", "-fno-gnu-unique", "-pthread", "-x", "c++", "-", "-I", str(repository / "godot-cpp/include"), "-I", str(repository / "godot-cpp/gdextension"), "-I", str(repository / "build/godot-cpp/gen/include"), "-x", "none", str(repository / "build/bin/libgodot-cpp.linux.editor.x86_64.a"), "-o", str(project / name)]
        subprocess.run(command, input=template.replace("VALUE_TYPE", value_type).replace("LABEL", label), text=True, check=True)
    (project / "perf_reload.gdextension").write_text('[configuration]\nentry_symbol="perf_reload_init"\ncompatibility_minimum="4.7"\nreloadable=true\n\n[libraries]\nlinux.editor.x86_64="res://libperf.so"\n')


@contextlib.contextmanager
def boot_editor(args, denied=False):
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="godot-cm-generic-") as directory:
        project = Path(directory)
        addons = project / "addons"
        addons.mkdir()
        identity = prepare_addon(repository, addons, args.baseline_tag, args.benchmark)
        fixture = addons / "generic_test"
        fixture.mkdir()
        (fixture / "plugin.cfg").write_text('[plugin]\nname="Generic MCP Tests"\ndescription="Isolated integration fixture"\nauthor="Tests"\nversion="1"\nscript="fixture.gd"\n')
        bootstrap = BOOTSTRAP
        if args.benchmark and args.benchmark_topology == "scripts":
            names = [f"benchmark_extra_{number}.gd" for number in range(20)]
            for number, name in enumerate(names):
                (project / name).write_text(f'@tool\nclass_name BenchmarkExtra{number}\nextends Node3D\nvar sample: Vector3\nfunc identity(value: Vector3) -> Vector3:\n    return value\n')
            bootstrap = bootstrap.replace('const Widget =', 'const BenchmarkScripts = [' + ','.join(f'preload("res://{name}")' for name in names) + ']\nconst Widget =')
        (fixture / "fixture.gd").write_text(bootstrap)
        (project / "generic_widget.gd").write_text(WIDGET)
        (project / "typed_constructor.gd").write_text(TYPED_CONSTRUCTOR)
        (project / "anonymous.gd").write_text('@tool\nextends Node3D\nvar counter := 0\nfunc _init(initial: int) -> void:\n    counter = initial\n')
        (project / "uncached_widget.gd").write_text(UNCACHED)
        (project / "reload_target.gd").write_text('@tool\nextends Node3D\nfunc accept(value: Vector3) -> String:\n    return "vector"\n')
        (project / "reload_replacement.gd").write_text('@tool\nextends Node3D\nvar counter := 7\n')
        scene = '[gd_scene format=3]\n\n[node name="GenericTestRoot" type="Node3D"]\n'
        if args.benchmark and args.benchmark_topology == "scene1000":
            scene += ''.join(f'\n[node name="BenchNode{number}" type="Node3D" parent="."]\n' for number in range(1000))
        (project / "blank.tscn").write_text(scene)
        renderer = "mobile" if args.terrain_addon else "gl_compatibility"
        (project / "project.godot").write_text('config_version=5\n[application]\nconfig/name="Generic MCP Isolated Test"\n[rendering]\nrenderer/rendering_method="' + renderer + '"\n')
        if args.terrain_addon:
            shutil.copytree(Path(args.terrain_addon).resolve(), addons / "terrain_3d", symlinks=True)
            (project / "physics_observer.gd").write_text('''extends Node3D
var elapsed := 0.0
var samples: Array = []
func _physics_process(delta: float) -> void:
    elapsed += delta
    var body: RigidBody3D = get_node("Ball")
    var p := body.global_position
    samples.append({"x":p.x,"y":p.y,"z":p.z,"spin":body.angular_velocity.length()})
    if elapsed >= 6.0:
        set_physics_process(false)
        var file := FileAccess.open("res://physics_samples.tmp", FileAccess.WRITE)
        file.store_string(JSON.stringify(samples))
        file.close()
        DirAccess.rename_absolute("res://physics_samples.tmp", "res://physics_samples.json")
''')
        cache = project / ".godot"
        cache.mkdir()
        extensions = "res://addons/godot_cm/godot_cm.gdextension\n"
        if args.catalog_lifecycle and not args.benchmark and not denied:
            prepare_reload_extension(repository, project)
            extensions += "res://perf_reload.gdextension\n"
        if args.terrain_addon:
            extensions += "res://addons/terrain_3d/terrain.gdextension\n"
        (cache / "extension_list.cfg").write_text(extensions)
        imported = subprocess.run([args.godot, "--headless", "--editor", "--path", directory, "--import", "--quit"], capture_output=True, text=True, timeout=120)
        assert imported.returncode == 0, f"Godot import exited {imported.returncode}\n" + imported.stdout + imported.stderr
        checked = subprocess.run([args.godot, "--headless", "--path", directory, "--check-only", "--script", "res://generic_widget.gd"], capture_output=True, text=True, timeout=30)
        assert checked.returncode == 0, checked.stdout + checked.stderr
        with (project / "project.godot").open("a") as file:
            file.write('[editor_plugins]\nenabled=PackedStringArray("res://addons/generic_test/plugin.cfg")\n')
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, GENERIC_TEST_PORT=str(port), GODOT_CM_PORT=str(port), GENERIC_TEST_DENY="1" if denied else "0", GODOT_CM_GENERATE_DTS="1" if args.write_declarations and not denied else "0", GENERIC_TEST_DTS=str(repository / "addons/godot_cm/api-surface.d.ts") if args.write_declarations and not denied else "")
        if args.benchmark and args.benchmark_profile is not None:
            env["GODOT_CM_PROFILE"] = args.benchmark_profile
        rendered = args.benchmark and args.benchmark_rendered
        env["GENERIC_TEST_FRAMES"] = "1" if rendered else "0"
        command = [args.godot] + ([] if rendered else ["--headless"]) + ["--editor", "--path", directory]
        if rendered and args.benchmark_display_driver:
            command += ["--display-driver", args.benchmark_display_driver]
        with (project / "editor.log").open("w+") as log:
            process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 60
                while not (project / "ready").exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(0.1)
                activation = json.loads((project / "activation_ms").read_text())
                print(f"Addon activation: {activation['checkbox']} ms; MCP ready: {activation['ready']} ms", flush=True)
                assert activation["checkbox"] < 1000, activation
                busy_ms = int((project / "busy_ms").read_text())
                assert busy_ms < 250, busy_ms
                print(f"MCP service startup: {(project / 'startup_ms').read_text()} ms; occupied port: {busy_ms} ms", flush=True)
                yield SimpleNamespace(client=Client(port), project=project, process=process, log=log, env=env, identity=identity, renderer=renderer, rendered=rendered, busy_ms=busy_ms)
            finally:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def run(args, denied=False):
    with boot_editor(args, denied) as boot:
        client, project, process, log, env = boot.client, boot.project, boot.process, boot.log, boot.env
        identity, renderer, rendered, busy_ms = boot.identity, boot.renderer, boot.rendered, boot.busy_ms
        report = None
        try:
            if True:
                if args.benchmark:
                    assert client.initialized["serverInfo"]["version"] == identity["plugin_version"], identity
                    cpu_before = process_cpu_ms(process.pid)
                    idle_started = time.perf_counter()
                    time.sleep(args.benchmark_idle_seconds)
                    cpu_after = process_cpu_ms(process.pid)
                    idle_wall_ms = round((time.perf_counter() - idle_started) * 1000, 3)
                    report = {
                        "addon": identity,
                        "environment": {"godot": subprocess.check_output([args.godot, "--version"], text=True).strip(), "platform": platform.system(), "architecture": platform.machine(), "logical_cpus": os.cpu_count(), "renderer": renderer, "headless": not rendered, "display_driver": args.benchmark_display_driver if rendered else "headless", "profiling_requested": env.get("GODOT_CM_PROFILE", "0"), "profiling_available": None, "build_configuration": "not embedded; binary identified by SHA-256", "harness_sha256": args.harness_sha256},
                        "round": args.benchmark_round,
                        "topology": args.benchmark_topology,
                        "warmup": args.benchmark_warmup,
                        "sample_count": args.benchmark_samples,
                        "request_timeout_ms": args.benchmark_timeout,
                        "sampling_protocol_met": args.benchmark_samples >= 60 and args.benchmark_warmup >= 10 and args.benchmark_rounds >= 3,
                        "activation": dict(activation, startup=int((project / "startup_ms").read_text()), occupied=busy_ms),
                        "idle": {"wall_ms": idle_wall_ms, "process_cpu_ms": cpu_after - cpu_before if cpu_before is not None and cpu_after is not None else None},
                        "main_thread_occupancy_ms": None,
                        "metrics": {},
                    }
                    children = client.value('const root = await godot.editor.getEditedScene();return await godot.call(root,"get_child_count");')
                    extras = client.search('return api.classes().filter(item => item.name.includes("BenchmarkExtra")).length;')
                    assert children == (1000 if args.benchmark_topology == "scene1000" else 0), children
                    assert extras == (20 if args.benchmark_topology == "scripts" else 0), extras
                    report["observed_topology"] = {"scene_children": children, "extra_script_classes": extras}
                    print("Benchmark addon: " + json.dumps(identity, sort_keys=True), flush=True)
                    if args.verify_worker_semantics:
                        report["semantics"] = verify_worker_semantics(client)
                    if rendered:
                        (project / "frames_start").write_text("start")
                        limit = time.monotonic() + 15
                        while not (project / "frames_ready").exists() and time.monotonic() < limit:
                            time.sleep(0.01)
                        assert (project / "frames_ready").exists(), "rendered frame sampler did not start"
                    if not args.benchmark_startup_only:
                        if identity["plugin_version"] == "0.2.1":
                            report.update(warmup=0, sample_count=5, sampling_protocol_met=False, legacy_api_caveat="internal batch target; passive pagination unavailable")
                            report["metrics"] = benchmark_legacy(client)
                        else:
                            report["metrics"] = benchmark(client, args)
                    if rendered:
                        (project / "frames_stop").write_text("stop")
                        limit = time.monotonic() + 15
                        while not (project / "frames.json").exists() and time.monotonic() < limit:
                            time.sleep(0.01)
                        assert (project / "frames.json").exists(), "rendered frame sampler did not stop"
                        frames = json.loads((project / "frames.json").read_text())
                        intervals = sorted(frames["intervals_ms"])
                        assert intervals and frames["frames_drawn"] > 0, {"intervals":len(intervals),"frames_drawn":frames["frames_drawn"],"continuous_spans":len(frames["continuous_process_frame_ms"])}
                        work = sorted(frames["continuous_process_frame_ms"])
                        assert work
                        frames.update(median_ms=statistics.median(intervals), p95_ms=intervals[math.ceil(len(intervals) * 0.95) - 1], max_ms=intervals[-1], continuous_process_frame_max_ms=work[-1], continuous_process_frame_p95_ms=work[math.ceil(len(work) * 0.95) - 1], scope="setup, warmups, measurements, cleanup; editor UI redraw requested every process frame; continuous span brackets process_frame callbacks around the bridge pump, not total frame/render time")
                        report["rendered_frames"] = frames
                else:
                    exercise(client, project, denied, bool(args.terrain_addon))
                    if args.catalog_lifecycle and not denied:
                        exercise_native_reload(client, project)
                        exercise_catalog_cycles(client, process.pid)
                        if env.get("GODOT_CM_PROFILE") == "1":
                            client.value("return true;")
                            log.flush()
                            log.seek(0)
                            profile = parse_performance_log(log.read())
                            assert not profile["invalid"], profile["invalid"]
                            gauges = profile["latest_gauges"]
                            assert gauges["catalog_live"] == 2 and gauges["catalog_peak"] <= 4 and gauges["script_cache_entries"] < 16, gauges
                            print("Catalog lifetime gauges:", gauges, flush=True)
                    if not denied:
                        exercise_reference_restart(client, project)
                log.flush()
                output = (project / "editor.log").read_text()
                if args.benchmark:
                    report["native_profile"] = parse_performance_log(output)
                    native = report["native_profile"]
                    assert not native["invalid"], native["invalid"]
                    assert all("PROFILE_PRIVATE_CONTENT_SENTINEL" not in line for line in output.splitlines() if line.startswith("[godot-cm-perf] "))
                    report["environment"]["profiling_available"] = bool(native["records"])
                    if env.get("GODOT_CM_PROFILE", "0") != "1":
                        assert not native["records"]
                    elif identity["source"] == "working-tree":
                        assert native["records"], "profiling requested but no native counters were emitted"
                    if report["environment"]["profiling_available"]:
                        executions = [record for record in native["records"] if record["kind"] in {"execute", "search"}]
                        assert len(executions) <= len(client.profile_labels)
                        if not native["omitted"]:
                            assert len(executions) == len(client.profile_labels), (len(executions), len(client.profile_labels))
                        for record, label in zip(executions, client.profile_labels):
                            record["benchmark_case"] = label
                            for phase in ("request", "context", "js_passive_decode"):
                                values = record["metrics"]
                                assert values[phase + "_exclusive_ns"] <= values[phase + "_inclusive_ns"]
                        report["main_thread_occupancy_ms"] = native["process_pump_max_ns"] / 1000000
                        report["main_thread_occupancy_scope"] = "process-lifetime pump maximum, including work before workload frame sampling"
                    if args.benchmark_verify_cost:
                        verify_catalog_cost(native, args.benchmark_warmup)
                    if args.benchmark_verify_pages:
                        verify_page_cost(native, args.benchmark_warmup)
                    if not args.benchmark_output:
                        print("Benchmark report: " + json.dumps(report, sort_keys=True), flush=True)
                assert "SCRIPT ERROR:" not in output, output
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read())
            raise
        return report if args.benchmark else None


