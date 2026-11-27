import argparse
import json
import math
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


ROOT = Path(__file__).resolve().parents[1]


class Client:
    def __init__(self, url, token=None):
        self.url, self.token, self.sequence = url, token, 0

    def rpc(self, method, params):
        self.sequence += 1
        headers = {"Content-Type": "application/json"}
        if self.token:
            headers["MCP-Session-Id"] = self.token
        request = urllib.request.Request(self.url, json.dumps({
            "jsonrpc": "2.0", "id": self.sequence, "method": method, "params": params,
        }).encode(), headers)
        with urllib.request.urlopen(request, timeout=90) as response:
            self.token = response.headers.get("MCP-Session-Id", self.token)
            return json.load(response)

    def initialize(self):
        return self.rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                      "clientInfo": {"name": "terrain-physics-test", "version": "1"}})

    def execute(self, code, mode="read", tool="execute"):
        response = self.rpc("tools/call", {"name": tool, "arguments": {
            "code": code, "mode": mode, "timeoutMs": 60000,
        }})
        assert "result" in response, response
        result = json.loads(response["result"]["content"][0]["text"])
        assert result["ok"], result
        return result["result"]

    def wait(self, code, predicate, seconds=30):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            result = self.execute(code)
            if predicate(result):
                return result
            time.sleep(0.2)
        raise AssertionError(f"Timed out: {code}; last result: {result}")


def create_and_run(client):
    tools = client.rpc("tools/list", {})
    assert {tool["name"] for tool in tools["result"]["tools"]} == {"search", "execute"}
    client.wait('return api.describe("terrain3d.generate_hill") != null;', bool)
    client.execute('return await godot.requestTier("filesystem");')
    tag = str(time.time_ns())
    scene = f"res://mcp_tests/terrain_roll_{tag}.tscn"
    directory = f"res://mcp_tests/terrain_roll_{tag}_data"
    client.execute('return await godot.pluginAction("editor.create_scene", ' + json.dumps({"path": scene, "name": "TerrainRollTest"}) + ');', "write")
    client.wait('return await godot.pluginRead("editor.state");', lambda s: s["scene"] == scene)
    terrain = client.execute('''
const root=await godot.editor.getEditedScene();
const t=await godot.create("Terrain3D");
await godot.set(t,"name","Terrain");
await godot.set(t,"region_size",128);
await godot.call(root,"add_child",t);
await godot.set(t,"owner",root);
return t;
''', "write")
    terrain_literal = json.dumps(terrain)
    info = client.execute(f'return await godot.pluginRead("terrain3d.info", {{terrain:{terrain_literal}}});')
    modes = [value for name, value in info["collision_modes"].items() if "FULL" in name and "GAME" in name]
    assert modes, info
    client.execute(f'await godot.set({terrain_literal},"collision_mode",{modes[0]});return await godot.pluginCall("terrain3d.generate_hill",{{terrain:{terrain_literal}}});', "write")
    client.execute('''
const root=await godot.editor.getEditedScene();
const terrain=await godot.call(root,"get_node","Terrain");
async function make(type,name,parent=root){
 const n=await godot.create(type); await godot.set(n,"name",name);
 await godot.call(parent,"add_child",n); await godot.set(n,"owner",root); return n;
}
const vec=(x,y,z)=>({$t:"Vector3",x,y,z});
const color=(r,g,b)=>({$t:"Color",r,g,b,a:1});
const physics=await godot.create("PhysicsMaterial");
await godot.set(physics,"friction",0.9);await godot.set(physics,"bounce",0.0);
await godot.set(terrain,"physics_material",physics);
const ball=await make("RigidBody3D","Ball");
const data=await godot.call(terrain,"get_data");
const h=await godot.call(data,"get_height",vec(36,0,32));
await godot.set(ball,"position",vec(36,h+0.82,32));
await godot.set(ball,"physics_material_override",physics);
await godot.set(ball,"linear_damp",0.05);await godot.set(ball,"angular_damp",0.05);
const mesh=await godot.create("SphereMesh");
await godot.set(mesh,"radius",0.75);await godot.set(mesh,"height",1.5);
const material=await godot.create("StandardMaterial3D");
await godot.set(material,"albedo_color",color(1,0.35,0.06));
await godot.set(material,"roughness",0.45);await godot.set(mesh,"material",material);
const visual=await make("MeshInstance3D","Visual",ball);await godot.set(visual,"mesh",mesh);
const shape=await godot.create("SphereShape3D");await godot.set(shape,"radius",0.75);
const collision=await make("CollisionShape3D","Collision",ball);await godot.set(collision,"shape",shape);
const camera=await make("Camera3D","Camera");
await godot.set(camera,"position",vec(64,36,72));await godot.call(camera,"look_at",vec(32,3,32));
await godot.set(camera,"current",true);
const light=await make("DirectionalLight3D","Sun");
await godot.set(light,"rotation_degrees",vec(-55,-30,0));await godot.set(light,"light_energy",1.5);
const env=await godot.create("Environment");await godot.set(env,"background_mode",1);
await godot.set(env,"background_color",color(0.12,0.18,0.28));
await godot.set(env,"ambient_light_source",3);await godot.set(env,"ambient_light_color",color(1,1,1));
await godot.set(env,"ambient_light_energy",0.7);
const world=await make("WorldEnvironment","Environment");await godot.set(world,"environment",env);
return await godot.pluginCall("editor.watch_rigidbody",{target:ball});
''', "write")
    client.execute('return await godot.pluginAction("terrain3d.save_new_directory",' + json.dumps({"terrain": terrain, "directory": directory}) + ');', "write")
    client.execute('return await godot.pluginAction("editor.save_scene",{confirm_save:true});', "write")
    client.wait('return await godot.pluginRead("editor.state");', lambda s: not s["unsaved"])
    client.execute('return await godot.pluginAction("editor.play_scene");', "write")
    try:
        sessions = client.wait('return await godot.pluginRead("editor.runtime_samples");',
                               lambda sessions: any(len(s["samples"]) >= 30 for s in sessions), seconds=45)
        samples = max(sessions, key=lambda session: len(session["samples"]))["samples"]
        start, end = samples[0], samples[-1]
        displacement = math.hypot(end["position"][0] - start["position"][0], end["position"][2] - start["position"][2])
        drop = start["position"][1] - end["position"][1]
        angular_speed = max(math.sqrt(sum(v * v for v in s["angular_velocity"])) for s in samples)
        assert displacement > 3 and drop > 2 and angular_speed > 0.1, samples
        assert min(s["position"][1] for s in samples) > -2, "Ball fell through the terrain"
        print(json.dumps({"scene": scene, "terrain_data": directory, "samples": len(samples),
                          "start": start["position"], "end": end["position"],
                          "horizontal_distance": displacement, "height_drop": drop,
                          "max_angular_speed": angular_speed}, indent=2))
    finally:
        client.execute('return await godot.pluginAction("editor.stop_scene");', "write")
        client.wait('return await godot.pluginRead("editor.state");', lambda s: not s["playing"])
    print("Terrain3D full MCP physics test passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--terrain-addon", type=Path)
    parser.add_argument("--url")
    args = parser.parse_args()
    if args.url:
        client = Client(args.url, os.environ.get("GODOT_CM_MCP_TOKEN"))
        client.initialize()
        create_and_run(client)
        return
    if not args.terrain_addon or not (args.terrain_addon / "terrain.gdextension").is_file():
        parser.error("Pass --terrain-addon PATH for an isolated test or --url for MCP-only scene creation")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryDirectory(prefix="godot-cm-physics-") as directory, tempfile.TemporaryFile(mode="w+") as log:
        project = Path(directory)
        addons = project / "addons"
        addons.mkdir()
        for name, source in [("godot_cm", ROOT / "addons/godot_cm"), ("terrain_3d", args.terrain_addon.resolve()),
                             ("godot_cm_terrain3d_adapter", ROOT / "godot-cm/addons/godot_cm_terrain3d_adapter")]:
            (addons / name).symlink_to(source, target_is_directory=True)
        driver = addons / "mcp_test_driver"
        driver.mkdir()
        (driver / "plugin.cfg").write_text('[plugin]\nname="MCP Test Consent Driver"\nscript="plugin.gd"\n')
        (driver / "plugin.gd").write_text('''@tool
extends EditorPlugin
var _bound := false
func _process(_delta: float) -> void:
    if _bound:
        return
    var service = EditorInterface.get_base_control().get_meta("godot_cm_service", null)
    if service:
        service.tier_request.connect(func(tier: String, _classes: Array) -> void:
            service.respond_tier_request(tier, true)
        )
        _bound = true
''')
        (project / "project.godot").write_text('''config_version=5
[application]
config/name="Godot CM MCP Physics Test"
run/main_scene="res://initial.tscn"
[editor_plugins]
enabled=PackedStringArray("res://addons/godot_cm/plugin.cfg", "res://addons/godot_cm_terrain3d_adapter/plugin.cfg", "res://addons/mcp_test_driver/plugin.cfg")
[editor]
run/main_run_args="--headless"
''')
        (project / "initial.tscn").write_text('[gd_scene format=3]\n[node name="Initial" type="Node3D"]\n')
        (project / ".godot").mkdir()
        (project / ".godot/extension_list.cfg").write_text('res://addons/godot_cm/godot_cm.gdextension\nres://addons/terrain_3d/terrain.gdextension\n')
        process = subprocess.Popen(["godot", "--headless", "--editor", "--path", str(project), "res://initial.tscn"],
                                   env=dict(os.environ, GODOT_CM_PORT=str(port)), stdout=log, stderr=subprocess.STDOUT)
        try:
            client = Client(f"http://127.0.0.1:{port}/mcp")
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("Editor exited during startup")
                try:
                    client.initialize()
                    break
                except (urllib.error.URLError, ConnectionError):
                    time.sleep(0.2)
            else:
                raise RuntimeError("Timed out waiting for the isolated editor")
            create_and_run(client)
        finally:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if sys.exc_info()[0] is not None:
                log.seek(0)
                print(log.read())


if __name__ == "__main__":
    main()
