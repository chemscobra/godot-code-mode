// Coin Rush (2D) — run via MCP `execute` with collect2d.tscn open, mode:"write".
// Idiomatic version: CharacterBody2D player, Area2D coin triggers wired to
// root._on_coin_body_entered with CONNECT_PERSIST so the connections save
// into the scene. Core loop lives in collect2d.gd (attached to the root).

const root = await godot.editor.getEditedScene();
const V2 = (x, y) => ({ $t: "Vector2", x, y });
const C = (r, g, b, a = 1) => ({ $t: "Color", r, g, b, a });
const CONNECT_PERSIST = 2; // Object.CONNECT_PERSIST — serialize into .tscn

// Re-runnable: detach everything under the root before rebuilding.
// Detached nodes don't serialize into the scene and die with it.
for (const child of await godot.call(root, "get_children")) {
	await godot.call(root, "remove_child", child);
}

async function mk(cls, props, parent = root) {
	const n = await godot.create(cls);
	for (const [k, v] of Object.entries(props)) await godot.set(n, k, v);
	await godot.call(parent, "add_child", n);
	await godot.call(n, "set_owner", root);
	return n;
}
async function circle(radius) {
	const s = await godot.create("CircleShape2D");
	await godot.set(s, "radius", radius);
	return s;
}
// Octagon approximating a filled disc — Polygon2D needs no texture.
const disc = (r) => Array.from({ length: 8 }, (_, i) => {
	const a = (i / 8) * Math.PI * 2;
	return V2(Math.cos(a) * r, Math.sin(a) * r);
});

// ── Player: CharacterBody2D + collision + visual ─────────────────────────
const player = await mk("CharacterBody2D", { name: "Player", position: V2(0, 0) });
await mk("CollisionShape2D", { name: "Body", shape: await circle(16) }, player);
await mk("Polygon2D", { name: "Skin", polygon: disc(16), color: C(0.3, 0.9, 0.4) }, player);

// ── Coins: Area2D trigger + shape + visual, signal → root method ─────────
const COINS = 8;
for (let i = 0; i < COINS; i++) {
	const coin = await mk("Area2D", {
		name: "Coin_" + i,
		position: V2(-350 + (i % 4) * 230, -200 + Math.floor(i / 4) * 320),
	});
	await mk("CollisionShape2D", { name: "Shape", shape: await circle(14) }, coin);
	await mk("Polygon2D", { name: "Skin", polygon: disc(14), color: C(1, 0.85, 0.2) }, coin);
	// Persisted signal wiring: survives saveScene as a [connection] entry.
	// Bound arg is the coin's name (a String) — object binds can't be
	// serialized to the scene; the handler resolves it with get_node().
	await godot.connect(coin, "body_entered", root, "_on_coin_body_entered",
		CONNECT_PERSIST, ["Coin_" + i]);
}

// ── Camera + HUD ──────────────────────────────────────────────────────────
await mk("Camera2D", { name: "Camera2D", position: V2(0, 0), zoom: V2(1, 1) });
const hud_layer = await mk("CanvasLayer", { name: "HudLayer" });
await mk("Label", { name: "Hud", position: V2(16, 12), text: "Coins: 0" }, hud_layer);

await godot.saveScene();
return { nodes: 2 + COINS * 3 + 3, wired: COINS };
