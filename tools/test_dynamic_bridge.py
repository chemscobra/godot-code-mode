import configparser
import json
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


def main():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, GODOT_CM_TEST_PORT=str(port))
    with tempfile.TemporaryDirectory(prefix="godot-cm-test-") as directory, tempfile.TemporaryFile(mode="w+") as log:
        project = Path(directory)
        (project / "addons").mkdir()
        (project / "addons/godot_cm").symlink_to(ROOT / "addons/godot_cm", target_is_directory=True)
        (project / "project.godot").write_text(
            'config_version=5\n[application]\nconfig/name="Godot CM Integration Test"\n'
            '[rendering]\nrenderer/rendering_method="gl_compatibility"\n')
        (project / "main.tscn").write_text('[gd_scene format=3]\n[node name="IntegrationRoot" type="Node3D"]\n')
        (project / "test_script.gd").write_text('extends Node3D\n')
        (project / "test_tool.gd").write_text('@tool\nextends Node3D\n')
        (project / "test_base.gd").write_text('extends Node2D\n')
        (project / ".godot").mkdir()
        extensions = "res://addons/godot_cm/godot_cm.gdextension\n"
        terrain_addon = os.environ.get("GODOT_CM_TERRAIN3D")
        if terrain_addon:
            (project / "addons/terrain_3d").symlink_to(Path(terrain_addon).resolve(), target_is_directory=True)
            (project / "addons/godot_cm_terrain3d_adapter").symlink_to(ROOT / "godot-cm/addons/godot_cm_terrain3d_adapter", target_is_directory=True)
            extensions += "res://addons/terrain_3d/terrain.gdextension\n"
        (project / ".godot/extension_list.cfg").write_text(extensions)
        process = subprocess.Popen([
            "godot", "--headless", "--editor", "--path", str(project),
            "--script", str(ROOT / "tools/test_dynamic_bridge.gd"),
        ], env=env, stdout=log, stderr=subprocess.STDOUT)
        token = None
        request_id = 0

        def rpc(method, params):
            nonlocal token, request_id
            request_id += 1
            headers = {"Content-Type": "application/json"}
            if token:
                headers["MCP-Session-Id"] = token
            request = urllib.request.Request(
                f"http://127.0.0.1:{port}/mcp",
                json.dumps({"jsonrpc": "2.0", "id": request_id,
                            "method": method, "params": params}).encode(), headers)
            with urllib.request.urlopen(request, timeout=15) as response:
                token = response.headers.get("MCP-Session-Id", token)
                return json.load(response)

        def execute(code, mode="read", tool="execute", verbose=False):
            response = rpc("tools/call", {"name": tool, "arguments": {
                "code": code, "mode": mode, "timeoutMs": 10000, "verbose": verbose}})
            return json.loads(response["result"]["content"][0]["text"])

        def check(code, expected, mode="read", tool="execute"):
            response = execute(code, mode, tool)
            assert response["ok"], response
            assert response["result"] == expected, response

        def search(code, expected):
            check(code, expected, tool="search")

        def control(action, **kwargs):
            return execute('return await godot.pluginRead("test.catalog_control", '
                           + json.dumps(dict(action=action, **kwargs)) + ');')

        try:
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError("Godot exited before starting the test service")
                try:
                    initialized = rpc("initialize", {"protocolVersion": "2025-11-25", "capabilities": {},
                                                     "clientInfo": {"name": "dynamic-bridge-test", "version": "1"}})
                    break
                except (urllib.error.URLError, ConnectionError):
                    time.sleep(0.1)
            else:
                raise RuntimeError("Timed out waiting for test service")
            plugin_config = configparser.ConfigParser()
            plugin_config.read(ROOT / "addons/godot_cm/plugin.cfg")
            expected_version = plugin_config["plugin"]["version"].strip('"')
            assert initialized["result"]["serverInfo"]["version"] == expected_version, initialized
            with (ROOT / "addons/godot_cm/extension_api.json").open() as seed_file:
                seed_names = [entry["name"] for entry in json.load(seed_file)["classes"]]
            r7 = execute("return 7;"); print("R7:", r7); assert r7 == {"ok": True, "result": 7}
            compact = execute('const n = await godot.create("Node3D"); for (let i=0; i<24; i++) await godot.set(n,"name","N"+i); return true;', "write")
            assert compact["ok"] and compact["changes"]["counts"]["Node3D.set"] == 24, compact
            assert compact["changes"]["status"] == "committed", compact
            detailed = execute('await godot.create("Node3D"); return true;', "write", verbose=True)
            assert isinstance(detailed["changes"], list) and len(detailed["changes"]) == 1, detailed
            rolled_back = execute('await godot.create("Node3D"); throw Error("rollback");', "write")
            assert not rolled_back["ok"] and rolled_back["changes"]["status"] == "rolled_back", rolled_back
            check('const seed = new Set(' + json.dumps(seed_names) + '); return api.classes().some(c => !seed.has(c.name));', True)
            check('return api.describe("ResourceLoader").methods.some(m => m.name === "load");', True)
            check('return api.describe("EditorInterface").methods.some(m => m.name === "get_edited_scene_root");', True)
            check('const e = await godot.getSingleton("EditorInterface"); return !!e.__godotHandle;', True)
            check('try { await godot.getSingleton("ResourceLoader"); return false; } catch(e) { return e.code === "PERMISSION"; }', True)
            check('const p = await godot.discoverPlugins(); return p.scripts[0].class === "DynamicTestProvider" && p.methods.some(m => m.name === "test.increment");', True)
            search('return typeof godot === "undefined";', True)
            search('return api.find("DynamicTestProvider").includes("DynamicTestProvider");', True)
            search('const c = api.describe("DynamicTestProvider"); return c.kind === "script_class" && c.execution.direct_access === false && c.metadata_status === "cached_members" && c.methods.some(m => m.name === "_increment");', True)
            search('const c = api.describe("test.increment"); return c.kind === "operation" && c.arguments[0].name === "amount" && c.return === "int" && c.execution.bridge === "godot.pluginCall" && c.execution.mode === "write" && c.execution.undo === "capture_restore_contract" && !("provider" in c) && !("capture" in c);', True)
            search('return api.find("counter by an amount").includes("test.increment");', True)
            search('return api.find("_increment").includes("DynamicTestProvider");', True)
            search('return api.classes().some(c => c.name === "test.increment" && c.kind === "operation");', True)
            search('const c = api.describe("test.gated"); return c.permissions.tier === "filesystem" && c.permissions.locked && !c.execution.invocable;', True)
            control("register")
            search('return api.find("test.late").includes("test.late") && api.describe("operation:test.late").execution.bridge === "godot.pluginRead";', True)
            control("unregister")
            search('return api.describe("test.late") == null && !api.find("test.late").includes("test.late");', True)
            control("register", name="Node3D")
            search('const c = api.describe("Node3D"); return c.ambiguous && c.matches.includes("native:Node3D") && c.matches.includes("operation:Node3D") && api.describe("native:Node3D").kind === "native_class";', True)
            search('return api.find("Node3D").includes("operation:Node3D") && api.find("Node3D").includes("native:Node3D");', True)
            control("unregister", name="Node3D")
            control("partial")
            search('const c = api.describe("script:res://unloaded.gd"); return c.metadata_status === "class_only" && c.methods.length === 0 && c.execution.direct_access === false;', True)
            control("partial", enabled=False)
            search('return api.describe("UnloadedProvider") == null;', True)
            control("expire")
            deadline = time.monotonic() + 2
            while execute('return api.describe("test.ephemeral") == null;', tool="search")["result"] is not True:
                assert time.monotonic() < deadline, "expired provider remained in the catalog"
                time.sleep(0.02)
            control("unsafe_metadata")
            search('return api.describe("test.metadata").input_schema.default == null;', True)
            assert control("metadata_status")["result"] == 0, "search executed metadata object methods"
            search('return api.find("counter").includes("DynamicTestProvider");', True)
            control("stress", count=30)
            search('const first = JSON.stringify(api.describe("test.toggling")); const end = Date.now() + 600; while (Date.now() < end) { if (JSON.stringify(api.describe("test.toggling")) !== first) throw new Error("mixed snapshot"); } return true;', True)
            ticks = control("status")
            assert ticks["ok"] and ticks["result"] > 1, ticks
            check('return await godot.pluginRead("test.read");', 0)
            check('try { await godot.create("DynamicTestProvider"); return false; } catch(e) { return e.code === "PERMISSION"; }', True, "write")
            check('try { await godot.pluginRead("test.gated"); return false; } catch(e) { return e.code === "PERMISSION"; }', True)
            check('try { await godot.pluginRead("test.read", []); return false; } catch(e) { return e.code === "BAD_ARGS"; }', True)
            check('try { await godot.pluginRead("test.increment"); return false; } catch(e) { return e.code === "PERMISSION"; }', True)
            check('return await godot.pluginCall("test.increment", {amount: 3});', 3, "write")
            check('return await godot.pluginRead("test.read");', 3)
            failed = execute('await godot.pluginCall("test.increment", {amount: 8, fail: true});', "write")
            assert not failed["ok"], failed
            check('return await godot.pluginRead("test.read");', 3)
            failed = execute('await godot.pluginCall("test.increment", {amount: 10}); throw new Error("rollback");', "write")
            assert not failed["ok"], failed
            check('return await godot.pluginRead("test.read");', 3)
            check('return api.describe("McpService") == null;', True)
            search('const op=api.describe("test.external");return op.execution.bridge === "godot.pluginAction" && op.execution.undo === "not_rollbackable";', True)
            check('try {await godot.pluginCall("test.external");return false;}catch(e){return e.code === "EFFECT_MISMATCH";}', True, "write")
            check('try {await godot.pluginAction("test.increment");return false;}catch(e){return e.code === "EFFECT_MISMATCH";}', True, "write")
            failed = execute('await godot.pluginAction("test.external");')
            assert not failed["ok"] and not failed.get("effects"), failed
            check('try {await godot.pluginAction("editor.create_scene",{path:"res://forbidden.tscn"});return false;}catch(e){return e.code === "PERMISSION";}', True, "write")
            response = execute('return await godot.pluginAction("test.external");', "write")
            assert response["ok"] and response["effects"][0]["rollbackable"] is False, response
            failed = execute('await godot.create("Node3D"); await godot.pluginAction("test.external");', "write")
            assert not failed["ok"] and not failed.get("effects"), failed
            check('return await godot.pluginRead("test.external_count");', 1)
            failed = execute('await godot.pluginAction("test.external"); await godot.create("Node3D");', "write")
            assert not failed["ok"] and failed["effects"][0]["status"] == "completed", failed
            check('return await godot.pluginRead("test.external_count");', 2)
            failed = execute('await godot.pluginAction("test.external", {fail: true});', "write")
            assert not failed["ok"] and failed["effects"][0]["status"] == "failed", failed
            check('return await godot.pluginRead("test.external_count");', 3)
            failed = execute('await godot.batch([{op:"plugin_action", method:"test.external"}]);', "write")
            assert not failed["ok"], failed
            check('return await godot.pluginRead("test.external_count");', 3)
            check('await godot.requestTier("filesystem"); const r = await godot.getSingleton("ResourceLoader"); return !!r.__godotHandle;', True)
            search('const c = api.describe("test.gated"); return !c.permissions.locked && c.execution.invocable;', True)
            check('const scene = await godot.loadResource("res://main.tscn"); const node = await godot.instantiateScene(scene); return await godot.call(node, "get_class");', "Node3D", "write")
            check('const n = await godot.create("Node3D"); await godot.attachScript(n,"res://test_script.gd"); return !!(await godot.call(n,"get_script"));', True, "write")
            check('try { const n = await godot.create("Node3D"); await godot.attachScript(n,"res://test_tool.gd"); return false; } catch(e) { return e.code === "PLUGIN_FAILED" || e.code === "NO_UNDO"; }', True, "write")
            check('try { const n = await godot.create("Node3D"); await godot.attachScript(n,"res://test_base.gd"); return false; } catch(e) { return e.code === "PLUGIN_FAILED" || e.code === "NO_UNDO"; }', True, "write")
            check('try { const n = await godot.create("Node3D"); await godot.attachScript(n,"res://missing.gd"); return false; } catch(e) { return e.code === "PLUGIN_FAILED" || e.code === "NO_UNDO"; }', True, "write")
            failed = execute('const n = await godot.create("Node3D"); await godot.attachScript(n,"res://test_script.gd"); throw new Error("rollback");', "write")
            assert not failed["ok"], failed
            check('const r = await godot.build({type:"Node3D",name:"B",children:[{type:"Node3D",name:"c1"},{type:"Node3D",name:"c2",children:[{type:"Node",name:"gc"}]}]}); return (await godot.call(r,"get_child_count")) === 2;', True, "write")
            check('const r = await godot.build({type:"Node3D",name:"T",children:[{type:"Node",name:"k"}]}); const t = await godot.scene.tree({root:r}); return t.name==="T" && t.class==="Node3D" && t.children[0].name==="k";', True, "write")
            check('try { const t = await godot.scene.tree(); return typeof t === "object"; } catch(e) { return e.code === "NOT_FOUND"; }', True)
            check('const r = await godot.scene.new("res://gen_scene.tscn", {type:"Node3D", name:"Gen"}); const n = await godot.call(r,"get_name"); return n === "Gen" || (n && n.value === "Gen");', True, "write")
            check('const n = await godot.create("MeshInstance3D"); const m = await godot.create("StandardMaterial3D"); await godot.set(n,"material_override",m); return !!(await godot.get(n,"material_override"));', True, "write")
            check('const n = await godot.create("MeshInstance3D"); const m = await godot.create("StandardMaterial3D"); await godot.call(n,"set_material_override",m); return !!(await godot.get(n,"material_override"));', True, "write")
            search('const e = api.example("Transform3D"); return e.$t === "Transform3D" && e.basis.x.x === 1;', True)
            check('const n = await godot.create("Node3D"); await godot.set(n,"transform",api.example("Transform3D")); const p = await godot.get(n,"position"); return p && p.x === 0 && p.y === 1 && p.z === -5;', True, "write")
            check('const s = await godot.permissions(); return s.tiers.length >= 2 && s.denied_classes.includes("ProjectSettings");', True)
            check('const p = await godot.permissions({class:"ProjectSettings", member:"set_setting"}); return p.allowed === false && p.reason === "class_denied";', True)
            check('const p = await godot.permissions({class:"ResourceLoader", member:"load"}); return p.tier.tier === "filesystem";', True)
            check('const p = await godot.permissions({class:"Node", member:"rpc"}); return p.allowed === false && p.reason === "member_denied";', True)
            check('const p = await godot.permissions({class:"Object", member:"call"}); return p.allowed === false && p.reason === "meta_method_denied";', True)
            check('const p = await godot.permissions({class:"EditorInterface", member:"get_editor_theme"}); return p.allowed === false && p.reason === "restricted_class";', True)
            check('const p = await godot.permissions({class:"Node3D", member:"get_name"}); return p.allowed === true;', True)
            # const methods no longer count as writes (fixed the prefix heuristic)
            made = execute('const n = await godot.create("Node3D"); return n;', "write")
            hid = made["result"]["__godotHandle"]
            check('try { await godot.call({"__godotHandle":"' + hid + '"}, "can_process"); return true; } catch(e) { return e.code; }', True)
            # journaled write calls beyond the original whitelist
            check('const n = await godot.create("Node3D"); await godot.call(n,"hide"); return true;', True, "write")
            check('const c = await godot.create("Node3D"); await godot.call(c,"add_to_group","g1"); return await godot.call(c,"is_in_group","g1");', True, "write")
            check('const n = await godot.create("Node3D"); await godot.call(n,"translate",{$t:"Vector3",x:0,y:1,z:0}); const p = await godot.get(n,"position"); return p && p.x === 0 && p.y === 1 && p.z === 0;', True, "write")
            check('const n = await godot.create("Control"); await godot.call(n,"grab_focus"); return true;', True, "write")
            # wider EditorInterface restrict list
            check('const e = await godot.getSingleton("EditorInterface"); const s = await godot.call(e,"get_open_scenes"); return Array.isArray(s) || Array.isArray(s && s.values);', True)
            check('try { await godot.loadResource("res://../outside.tscn"); return false; } catch(e) { return e.code === "BAD_PATH"; }', True, "write")
            check('try { await godot.loadResource("res://addons/godot_cm/plugin.gd"); return false; } catch(e) { return e.code === "PERMISSION"; }', True, "write")
            check('try {await godot.pluginAction("editor.create_scene", {path:"res://../bad.tscn"});return false;}catch(e){return e.code === "PLUGIN_FAILED";}', True, "write")
            check('try {await godot.pluginAction("editor.create_scene", {path:"res://main.tscn"});return false;}catch(e){return e.code === "PLUGIN_FAILED";}', True, "write")
            check('try {await godot.pluginAction("editor.save_scene");return false;}catch(e){return e.code === "PLUGIN_FAILED";}', True, "write")
            if not terrain_addon:
                search('return api.describe("Terrain3D") == null && api.describe("terrain3d.generate_hill") == null;', True)
            if terrain_addon:
                created = execute('const root=await godot.pluginRead("test.scene_root"); const t=await godot.create("Terrain3D"); await godot.set(t,"region_size",128);await godot.call(root,"add_child",t); return t;', "write")
                assert created["ok"], created
                terrain = json.dumps(created["result"])
                info = f'await godot.pluginRead("terrain3d.info", {{terrain:{terrain}}})'
                check(f'return ({info}).regions;', 0)
                hill = f'await godot.pluginCall("terrain3d.generate_hill", {{terrain:{terrain}}})'
                failed = execute(hill + '; throw new Error("rollback terrain");', "write")
                assert not failed["ok"], failed
                check(f'return ({info}).regions;', 0)
                response = execute('return ' + hill + ';', "write")
                assert response["ok"], response
                check(f'const d=await godot.call({terrain},"get_data");return await godot.call(d,"get_height",{{$t:"Vector3",x:32,y:0,z:32}});', 8)
                failed = execute(f'await godot.pluginCall("terrain3d.generate_hill",{{terrain:{terrain},height:12}});throw new Error("restore height");', "write")
                assert not failed["ok"], failed
                check(f'const d=await godot.call({terrain},"get_data");return await godot.call(d,"get_height",{{$t:"Vector3",x:32,y:0,z:32}});', 8)
                response = execute(f'return await godot.pluginAction("terrain3d.save_new_directory",{{terrain:{terrain},directory:"res://saved_terrain"}});', "write")
                assert response["ok"] and response["result"]["files"], response
                failed = execute(f'await godot.pluginAction("terrain3d.save_new_directory",{{terrain:{terrain},directory:"res://saved_terrain"}});', "write")
                assert not failed["ok"], failed
                print("Terrain3D native integration tests passed")
            print("Dynamic bridge integration tests passed")
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            log.seek(0)
            output = log.read()
            if process.returncode not in (0, -15) or sys.exc_info()[0] is not None:
                print(output)


if __name__ == "__main__":
    main()
