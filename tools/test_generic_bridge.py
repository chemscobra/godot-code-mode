import argparse
import configparser
import json
import io
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
var service: RefCounted

func _enter_tree() -> void:
    call_deferred("_start")

func _start() -> void:
    await _probe_activation()
    service = ClassDB.instantiate("McpService")
    var started := Time.get_ticks_msec()
    assert(service.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
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
    EditorInterface.open_scene_from_path("res://blank.tscn")
    FileAccess.open("res://ready", FileAccess.WRITE).store_string("ready")

func _process(_delta: float) -> void:
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
    service.stop()
    service = ClassDB.instantiate("McpService")
    assert(service.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
    service.tier_request.connect(_permission)
    service.set_tier_policy("untracked", "ask")
    service.set_tier_policy("filesystem", "ask")
    FileAccess.open("res://restart_ready", FileAccess.WRITE).store_string("ready")

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
    if service:
        service.stop()
        service = null
'''

WIDGET = '''@tool
class_name GenericMcpWidget
extends Node3D

static var constructions := 0
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
        if timeout is not None:
            arguments["timeoutMs"] = timeout
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
    assert array["$t"] == "Array" and array["element_type"]["type"] == "Vector3", array
    assert without_math_tags(array["values"]) == [vector], array
    empty = client.value(f'return await godot.call({widget},"echo_typed_array",[]);', "untracked")
    assert empty["element_type"]["type"] == "Vector3" and empty["values"] == [], empty
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
    assert widget_array["element_type"]["type"] == "GenericMcpWidget", widget_array
    replay = client.value(f'return await godot.call({widget},"echo_widget_array",{literal(widget_array)});', "untracked")
    assert replay["element_type"] == widget_array["element_type"], replay
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


def exercise(client, project, denied=False, terrain=False):
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
    node = client.value('return await godot.create("Node3D");', "write")
    handle = literal(node)
    assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 1);', "write")["ok"]
    for operation in [{"op": "create", "class": "Node3D", "write": False}, {"op": "set", "target": node, "prop": "name", "value": "Spoofed", "write": False}]:
        result = client.value('return await godot.batch(' + literal([operation]) + ');')
        assert not result[0]["ok"], result
    assert not client.execute(f'return await godot.connect({handle}, "renamed", {handle}, "call");', "write")["ok"]
    assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 1);', "untracked")["ok"]
    grant = client.value('return await godot.requestTier("untracked");')
    if denied:
        assert not grant["granted"]
        assert not client.execute(f'await godot.call({handle}, "set_meta", "generic_test", 2);', "untracked")["ok"]
        assert not client.value(f'return await godot.call({handle}, "has_meta", "generic_test");')
        return
    assert grant["granted"]
    exercise_passive_data(client, node)
    exercise_value_validation(client, node)
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
    widget = literal(client.value('return await godot.create("GenericMcpWidget");', "untracked"))
    assert client.value(f'return await godot.call({widget}, "constructor_count");', "untracked") == 1
    exercise_variant_matrix(client, widget)
    exercise_diagnostics(client, node, widget)
    exercise_rotation_contract(client, widget)
    exercise_collections(client, node, widget)
    exercise_value_references(client, node, widget)
    exercise_checkpoints(client, node, widget)
    exercise_reference_quotas(client, widget)
    exercise_integration_contracts(client, project, node, widget)
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


def benchmark(client):
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


def run(args, denied=False):
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="godot-cm-generic-") as directory:
        project = Path(directory)
        addons = project / "addons"
        addons.mkdir()
        if args.baseline_021:
            archive = subprocess.run(["git", "archive", "v0.2.1", "addons/godot_cm"], cwd=repository, capture_output=True, check=True).stdout
            with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                files.extractall(project, filter="data")
        else:
            (addons / "godot_cm").symlink_to(repository / "addons/godot_cm", target_is_directory=True)
        fixture = addons / "generic_test"
        fixture.mkdir()
        (fixture / "plugin.cfg").write_text('[plugin]\nname="Generic MCP Tests"\ndescription="Isolated integration fixture"\nauthor="Tests"\nversion="1"\nscript="fixture.gd"\n')
        (fixture / "fixture.gd").write_text(BOOTSTRAP)
        (project / "generic_widget.gd").write_text(WIDGET)
        (project / "typed_constructor.gd").write_text(TYPED_CONSTRUCTOR)
        (project / "anonymous.gd").write_text('@tool\nextends Node3D\nvar counter := 0\nfunc _init(initial: int) -> void:\n    counter = initial\n')
        (project / "uncached_widget.gd").write_text(UNCACHED)
        (project / "blank.tscn").write_text('[gd_scene format=3]\n\n[node name="GenericTestRoot" type="Node3D"]\n')
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
        with (project / "editor.log").open("w+") as log:
            process = subprocess.Popen([args.godot, "--headless", "--editor", "--path", directory], env=env, stdout=log, stderr=subprocess.STDOUT)
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
                client = Client(port)
                if args.benchmark:
                    benchmark(client)
                else:
                    exercise(client, project, denied, bool(args.terrain_addon))
                    if not denied:
                        exercise_reference_restart(client, project)
                log.flush()
                log.seek(0)
                output = log.read()
                assert "SCRIPT ERROR:" not in output, output
            except BaseException:
                log.flush()
                log.seek(0)
                print(log.read())
                raise
            finally:
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--godot", default=shutil.which("godot") or "godot")
    parser.add_argument("--terrain-addon")
    parser.add_argument("--inventory-only", action="store_true")
    parser.add_argument("--write-declarations", action="store_true")
    parser.add_argument("--benchmark", action="store_true")
    parser.add_argument("--baseline-021", action="store_true")
    options = parser.parse_args()
    if options.baseline_021 and (not options.benchmark or options.write_declarations):
        parser.error("--baseline-021 requires --benchmark and cannot generate declarations")
    verify_variant_inventory(Path(__file__).resolve().parents[1])
    if not options.inventory_only:
        if options.benchmark:
            run(options)
        else:
            run(options, denied=True)
            run(options)
            print("Generic dynamic bridge integration tests passed")
