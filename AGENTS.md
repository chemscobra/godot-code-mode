# Build and verification

- Godot API floor: 4.7; the checked-in API dump comes from Godot 4.7.2.
- Linux editor build: `cmake -B build -DGODOTCPP_TARGET=editor -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 6`.
- Windows x86-64 editor cross-build: `cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=toolchains/mingw-w64.cmake -DGODOTCPP_TARGET=editor -DCMAKE_BUILD_TYPE=Release && cmake --build build-windows -j 6`. Use a fresh build directory when switching toolchains. The toolchain statically links the MinGW runtime.
- Generic MCP integration tests: `python3 tools/test_generic_bridge.py` (requires `godot` on PATH, or `--godot /path/to/godot`).
- Optional real-extension/physics test: `python3 tools/test_generic_bridge.py --terrain-addon /path/to/project/addons/terrain_3d`.
- Tests create isolated temporary projects and copy the optional third-party addon. Do not import or run tests against the user's example project: Godot may rewrite project settings and import metadata.
- The isolated fixture seeds `.godot/extension_list.cfg` before initial import so native classes are available at editor startup.
- Scene construction, resource persistence, playback and runtime-result retrieval in the optional physics test use MCP. Its temporary observer script only samples a generic rigid body; it does not integrate Terrain3D.
- Keep third-party implementations/adapters out of `src/` and `addons/godot_cm/`. Test-only plugin usage belongs in `tools/` or isolated fixtures.
- Before finishing: rebuild both native binaries after source/version changes, run relevant integration tests, and run `git diff --check`. A successful Windows cross-build does not establish Windows runtime compatibility.
- Activation tests exercise the shipped EditorPlugin, immediate disable/re-enable, pending/running status, occupied ports, and measure checkbox latency separately from service readiness. Startup discovers native APIs from ClassDB without parsing the static API dump; inherited signatures are shared within each immutable catalog.
- Normal activation skips diagnostic spike checks and TypeScript generation. Opt in with `GODOT_CM_RUN_CHECKS=1` or `GODOT_CM_GENERATE_DTS=1` when developing; generated declarations are written to `user://godot_cm-api-surface.d.ts`.
