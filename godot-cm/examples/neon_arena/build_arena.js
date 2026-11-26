// Neon Arena (3D) — run via MCP `execute` with neon_arena.tscn open,
// mode:"write". Idiomatic version: walls/floor/pillars as StaticBody3D
// (real collision for move_and_slide), cells + sentries as Area3D triggers
// wired to root handlers with CONNECT_PERSIST. Core loop in neon_arena.gd.

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
async function material(color, emission) {
	const m = await godot.create("StandardMaterial3D");
	await godot.set(m, "albedo_color", color);
	if (emission) await godot.set(m, "emission", color);
	return m;
}
async function boxMesh(size) {
	const m = await godot.create("BoxMesh");
	await godot.set(m, "size", size);
	return m;
}
async function boxShape(size) {
	const s = await godot.create("BoxShape3D");
	await godot.set(s, "size", size);
	return s;
}
async function sphereShape(radius) {
	const s = await godot.create("SphereShape3D");
	await godot.set(s, "radius", radius);
	return s;
}
// StaticBody3D + matching collision shape + box visual.
async function solid(name, pos, size, color, emission) {
	const body = await add("StaticBody3D", { name, position: pos });
	await add("CollisionShape3D", { name: "Shape", shape: await boxShape(size) }, body);
	const skin = await add("MeshInstance3D", { name: "Skin" }, body);
	await godot.set(skin, "mesh", await boxMesh(size));
	await godot.set(skin, "material_override", await material(color, emission));
	return body;
}
// Area3D trigger + sphere/box shape + scaled sphere/box visual.
async function area(name, pos, shape, skinScale, color, emission, box) {
	const a = await add("Area3D", { name, position: pos });
	await add("CollisionShape3D", { name: "Shape", shape }, a);
	const skin = await add("MeshInstance3D", { name: "Skin" }, a);
	await godot.set(skin, "mesh", box ? await boxMesh(V3(1, 1, 1)) : await godot.create("SphereMesh"));
	await godot.set(skin, "scale", skinScale);
	await godot.set(skin, "material_override", await material(color, emission));
	return a;
}

// Floor + perimeter walls (arena = 30x30, walls at +-15)
await solid("Floor", V3(0, -0.25, 0), V3(30, 0.5, 30), C(0.08, 0.09, 0.14));
await solid("WallN", V3(0, 1, -15), V3(32, 2, 0.5), C(0.1, 0.3, 0.5), true);
await solid("WallS", V3(0, 1, 15), V3(32, 2, 0.5), C(0.1, 0.3, 0.5), true);
await solid("WallE", V3(15, 1, 0), V3(0.5, 2, 32), C(0.1, 0.3, 0.5), true);
await solid("WallW", V3(-15, 1, 0), V3(0.5, 2, 32), C(0.1, 0.3, 0.5), true);

// Pillar grid: 12 pillars (open center)
const spots = [-9, -3, 3, 9];
let p = 0;
for (const x of spots) for (const z of spots) {
	if ((x === -3 || x === 3) && (z === -3 || z === 3)) continue;
	await solid("Pillar_" + p++, V3(x, 1.5, z), V3(1, 3, 1), C(0.35, 0.15, 0.5), true);
}

// Player: CharacterBody3D + sphere collision + visual
const player = await add("CharacterBody3D", { name: "Player", position: V3(0, 0.5, 0) });
await add("CollisionShape3D", { name: "Body", shape: await sphereShape(0.5) }, player);
const pskin = await add("MeshInstance3D", { name: "Skin" }, player);
await godot.set(pskin, "mesh", await godot.create("SphereMesh"));
await godot.set(pskin, "scale", V3(0.5, 0.5, 0.5));
await godot.set(pskin, "material_override", await material(C(0.2, 0.9, 0.3), true));

// 12 energy cells in a double ring — Area3D + persisted body_entered.
// Bound arg is the cell name (String) — object binds can't serialize.
for (let i = 0; i < 12; i++) {
	const a = (i / 12) * Math.PI * 2;
	const r = i % 2 ? 11 : 6;
	const cell = await area("Cell_" + i,
		V3(Math.cos(a) * r, 0.6, Math.sin(a) * r),
		await sphereShape(0.45), V3(0.3, 0.3, 0.3), C(0.1, 0.9, 0.9), true);
	await godot.connect(cell, "body_entered", root, "_on_cell_body_entered",
		CONNECT_PERSIST, ["Cell_" + i]);
}

// 8 sentries on the outer ring — Area3D + persisted body_entered.
for (let i = 0; i < 8; i++) {
	const a = (i / 8) * Math.PI * 2 + 0.4;
	const s = await area("Sentry_" + i,
		V3(Math.cos(a) * 12, 0.6, Math.sin(a) * 12),
		await boxShape(V3(1.2, 1.2, 1.2)), V3(0.8, 0.8, 0.8), C(0.95, 0.2, 0.2), true, true);
	await godot.connect(s, "body_entered", root, "_on_sentry_body_entered",
		CONNECT_PERSIST, ["Sentry_" + i]);
}

// HUD
const layer = await add("CanvasLayer", { name: "HudLayer" });
await add("Label", {
	name: "Hud", position: { $t: "Vector2", x: 20, y: 20 },
	text: "Lives: 3 | Cells: 0",
}, layer);

// Sun + camera
await add("DirectionalLight3D", {
	name: "Sun", rotation: V3(-0.9, 0.5, 0), shadow_enabled: true,
});
const cam = await add("Camera3D", { name: "Camera3D" });
await godot.call(cam, "look_at_from_position",
	V3(0, 22, 18), V3(0, 0, 0), V3(0, 1, 0));

await godot.saveScene();
return { built: "Neon Arena", cells: 12, sentries: 8, pillars: p };
