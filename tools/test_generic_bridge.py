import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.request


BOOTSTRAP = '''@tool
extends EditorPlugin

const Widget = preload("res://generic_widget.gd")
var service: RefCounted

func _enter_tree() -> void:
    call_deferred("_start")

func _start() -> void:
    service = ClassDB.instantiate("McpService")
    assert(service.start(int(OS.get_environment("GENERIC_TEST_PORT"))))
    service.tier_request.connect(_permission)
    service.set_tier_policy("untracked", "deny" if OS.get_environment("GENERIC_TEST_DENY") == "1" else "ask")
    service.set_tier_policy("filesystem", "ask")
    EditorInterface.open_scene_from_path("res://blank.tscn")
    FileAccess.open("res://ready", FileAccess.WRITE).store_string("ready")

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


def exercise(client, project, denied=False, terrain=False):
    config = configparser.ConfigParser()
    config.read(Path(__file__).resolve().parents[1] / "addons/godot_cm/plugin.cfg")
    assert client.initialized["serverInfo"]["version"] == config["plugin"]["version"].strip('"')
    assert client.search('return api.describe("EditorInterface").methods.some(m => m.name === "get_editor_viewport_3d") && api.describe("EditorInterface").methods.some(m => m.name === "stop_playing_scene");')
    schema = client.rpc("tools/list", {})
    execution = next(tool for tool in schema["tools"] if tool["name"] == "execute")
    assert "untracked" in execution["inputSchema"]["properties"]["mode"]["enum"]
    assert client.search('return api.describe("ResourceLoader").methods.some(m => m.name === "load");')
    assert client.search('return api.describe("EditorInterface").methods.some(m => m.name === "play_current_scene");')
    assert client.search('return api.describe("GenericMcpWidget").kind === "script_class";')
    assert client.search('return api.describe("GenericMcpWidget").methods.some(m => m.name === "increment");')
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
    assert client.value('''
const rf = api.describe("Image").constants.FORMAT_RF;
const image = await godot.call("Image", "create_from_data", 2, 2, false, rf, {$t:"PackedByteArray",values:[0,0,32,64,0,0,32,64,0,0,32,64,0,0,32,64]});
const bytes = await godot.call(image, "get_data");
if (bytes.type !== "PackedByteArray" || !bytes.__godotValue) throw new Error("Missing opaque packed value");
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
    assert client.value(f'return await godot.get({instance}, "name");') == "GenericTestRoot"
    assert not client.execute('return 1;', "invalid")["ok"]
    assert bool(client.search('return api.describe("Terrain3D") != null;')) == terrain
    if terrain:
        assert client.search('return api.describe("Terrain3DData").methods.some(m => m.name === "import_images");')
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


def run(args, denied=False):
    repository = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="godot-cm-generic-") as directory:
        project = Path(directory)
        addons = project / "addons"
        addons.mkdir()
        (addons / "godot_cm").symlink_to(repository / "addons/godot_cm", target_is_directory=True)
        fixture = addons / "generic_test"
        fixture.mkdir()
        (fixture / "plugin.cfg").write_text('[plugin]\nname="Generic MCP Tests"\ndescription="Isolated integration fixture"\nauthor="Tests"\nversion="1"\nscript="fixture.gd"\n')
        (fixture / "fixture.gd").write_text(BOOTSTRAP)
        (project / "generic_widget.gd").write_text(WIDGET)
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
        with (project / "project.godot").open("a") as file:
            file.write('[editor_plugins]\nenabled=PackedStringArray("res://addons/generic_test/plugin.cfg")\n')
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, GENERIC_TEST_PORT=str(port), GENERIC_TEST_DENY="1" if denied else "0")
        with (project / "editor.log").open("w+") as log:
            process = subprocess.Popen([args.godot, "--headless", "--editor", "--path", directory], env=env, stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 60
                while not (project / "ready").exists():
                    if process.poll() is not None or time.monotonic() > deadline:
                        log.seek(0)
                        raise AssertionError(log.read())
                    time.sleep(0.1)
                exercise(Client(port), project, denied, bool(args.terrain_addon))
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
    options = parser.parse_args()
    run(options, denied=True)
    run(options)
    print("Generic dynamic bridge integration tests passed")
