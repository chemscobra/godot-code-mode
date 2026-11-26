// Orb Run (3D) — run via MCP `execute` with collect3d.tscn open, mode:"write".
// Idiomatic version: CharacterBody3D player moved with move_and_slide() in
// _physics_process, orbs as Area3D triggers wired to root._on_orb_body_entered
// with CONNECT_PERSIST so the connections save into the scene. Core loop
// lives in collect3d.gd (attached to the root).

const root = await godot.editor.getEditedScene();
const V3 = (x, y, z) => ({ $t: "Vector3", x, y, z });
const C = (r, g, b) => ({ $t: "Color", r, g, b, a: 1 });
const CONNECT_PERSIST = 2; // Object.CONNECT_PERSIST — serialize into .tscn

// Re-runnable: detach everything under the root before rebuilding.
// Detached nodes don't serialize into the scene and die with it.
for (const child of await godot.call(root, "get_children")) {
	await godot.call(root, "remove_child", child);
}

async function add(cls, props, parent = root) {
	const n = await godot.create(cls);
	for (const [k, v] of Object.entries(props)) await godot.set(n, k, v);
	await godot.call(parent, "add_child", n);
	await godot.call(n, "set_owner", root);
	return n;
}
async function material(color) {
	const m = await godot.create("StandardMaterial3D");
	await godot.set(m, "albedo_color", color);
	return m;
}
async function sphere(radius) {
	const s = await godot.create("SphereShape3D");
	await godot.set(s, "radius", radius);
	return s;
}
async function visual(meshCls, color, parent) {
	const mesh = await godot.create(meshCls);
	const n = await add("MeshInstance3D", { name: "Skin" }, parent);
	await godot.set(n, "mesh", mesh);
	await godot.set(n, "material_override", await material(color));
	return n;
}

// ── Ground: StaticBody3D floor + plane visual ────────────────────────────
const ground = await add("StaticBody3D", { name: "Ground", position: V3(0, -0.1, 0) });
const box = await godot.create("BoxShape3D");
await godot.set(box, "size", V3(20, 0.2, 20));
await add("CollisionShape3D", { name: "Shape", shape: box }, ground);
const plane = await godot.create("PlaneMesh");
await godot.set(plane, "size", ({ $t: "Vector2", x: 20, y: 20 }));
const groundSkin = await add("MeshInstance3D", { name: "Skin" }, ground);
await godot.set(groundSkin, "mesh", plane);
await godot.set(groundSkin, "material_override", await material(C(0.25, 0.25, 0.3)));

// ── Player: CharacterBody3D + collision + visual ─────────────────────────
const player = await add("CharacterBody3D", { name: "Player", position: V3(0, 0.5, 0) });
await add("CollisionShape3D", { name: "Body", shape: await sphere(0.5) }, player);
await visual("SphereMesh", C(0.2, 0.9, 0.3), player);

// ── Orbs: Area3D trigger + shape + visual, signal → root method ──────────
const ORBS = 8;
for (let i = 0; i < ORBS; i++) {
	const a = (i / ORBS) * Math.PI * 2;
	const orb = await add("Area3D", {
		name: "Orb_" + i,
		position: V3(Math.cos(a) * 5, 0.5, Math.sin(a) * 5),
	});
	await add("CollisionShape3D", { name: "Shape", shape: await sphere(0.6) }, orb);
	await visual("SphereMesh", C(1.0, 0.85, 0.1), orb);
	// Persisted signal wiring: survives saveScene as a [connection] entry.
	// Bound arg is the orb's name (a String) — object binds can't be
	// serialized to the scene; the handler resolves it with get_node().
	await godot.connect(orb, "body_entered", root, "_on_orb_body_entered",
		CONNECT_PERSIST, ["Orb_" + i]);
}

// ── Sun, camera, HUD ─────────────────────────────────────────────────────
await add("DirectionalLight3D", {
	name: "Sun", rotation: V3(-0.9, 0.4, 0), shadow_enabled: true,
});
const cam = await add("Camera3D", { name: "Camera3D" });
await godot.call(cam, "look_at_from_position", V3(0, 9, 13), V3(0, 0, 0), V3(0, 1, 0));
const hudLayer = await add("CanvasLayer", { name: "HudLayer" });
await add("Label", {
	name: "Hud",
	position: ({ $t: "Vector2", x: 16, y: 12 }),
	text: "Orbs: 0",
}, hudLayer);

await godot.saveScene();
return { built: "Orb Run", orbs: ORBS, wired: ORBS };
