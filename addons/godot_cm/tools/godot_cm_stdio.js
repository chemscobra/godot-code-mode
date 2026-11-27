#!/usr/bin/env node
/* godot-cm stdio adapter (Node.js): proxies MCP stdio (line-delimited
 * JSON-RPC) to the editor's Streamable HTTP endpoint. Same behaviour as
 * godot_cm_stdio.py — use whichever runtime the client machine already has.
 *
 * Token ownership (design D5): the adapter reads the session token from
 * GODOT_CM_TOKEN, GODOT_CM_TOKEN_FILE, or the newest
 * ~/.local/share/godot/app_userdata/<project>/godot_cm_session file the editor wrote.
 * Never takes the token from argv. Env: GODOT_CM_PORT, GODOT_CM_TOKEN_WAIT
 * (seconds to poll for the file while the editor starts), GODOT_CM_PROJECT
 * (pin the session file to one project's app_userdata folder name).
 *
 * Usage as MCP server config:
 *   {"command": "node", "args": ["/path/to/godot_cm_stdio.js"]}
 */
"use strict";
const fs = require("fs");
const os = require("os");
const path = require("path");
const http = require("http");
const readline = require("readline");

const PORT = parseInt(process.env.GODOT_CM_PORT || "8765", 10);
const URL_PATH = "/mcp";
const TOKEN_WAIT = parseFloat(process.env.GODOT_CM_TOKEN_WAIT || "20");
const PROJECT = process.env.GODOT_CM_PROJECT || null;

function appdataRoots() {
	if (process.platform === "win32") {
		return [path.join(process.env.APPDATA || "", "Godot")];
	}
	if (process.platform === "darwin") {
		return [path.join(os.homedir(), "Library", "Application Support", "Godot")];
	}
	return [path.join(os.homedir(), ".local", "share", "godot")];
}

function tokenCandidates() {
	// Session files from all projects, newest first.
	const out = [];
	for (const root of appdataRoots()) {
		const base = path.join(root, "app_userdata");
		let projects;
		try {
			projects = fs.readdirSync(base, { withFileTypes: true });
		} catch {
			continue;
		}
		for (const entry of projects) {
			if (PROJECT && entry.name !== PROJECT) continue;
			if (!entry.isDirectory()) continue;
			const file = path.join(base, entry.name, "godot_cm_session");
			try {
				out.push({ file, mtime: fs.statSync(file).mtimeMs });
			} catch {
				// no session file in this project dir
			}
		}
	}
	out.sort((a, b) => b.mtime - a.mtime);
	return out.map((c) => c.file);
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function findToken() {
	if (process.env.GODOT_CM_TOKEN) return process.env.GODOT_CM_TOKEN.trim();
	const tokenFile = process.env.GODOT_CM_TOKEN_FILE;
	if (tokenFile && fs.existsSync(tokenFile)) {
		return fs.readFileSync(tokenFile, "utf8").trim();
	}
	const deadline = Date.now() + TOKEN_WAIT * 1000;
	while (true) {
		const candidates = tokenCandidates();
		if (candidates.length) {
			return fs.readFileSync(candidates[0], "utf8").trim();
		}
		if (Date.now() >= deadline) return "";
		await sleep(250);
	}
}

function post(payload, sessionId) {
	return new Promise((resolve) => {
		const body = JSON.stringify(payload);
		const req = http.request(
			{
				hostname: "127.0.0.1",
				port: PORT,
				path: URL_PATH,
				method: "POST",
				headers: {
					"Content-Type": "application/json",
					"Content-Length": Buffer.byteLength(body),
					...(sessionId ? { "MCP-Session-Id": sessionId } : {}),
				},
				timeout: 30000,
			},
			(res) => {
				const chunks = [];
				res.on("data", (c) => chunks.push(c));
				res.on("end", () =>
					resolve({
						status: res.statusCode,
						data: Buffer.concat(chunks).toString("utf8"),
						sessionId: res.headers["mcp-session-id"] || sessionId,
					})
				);
			}
		);
		req.on("timeout", () => req.destroy(new Error("request timed out")));
		req.on("error", (e) =>
			resolve({
				status: 0,
				data: JSON.stringify({
					jsonrpc: "2.0",
					id: payload.id ?? null,
					error: {
						code: -32000,
						message: `godot-cm unreachable at http://127.0.0.1:${PORT}${URL_PATH}: ${e.message}`,
					},
				}),
				sessionId,
			})
		);
		req.end(body);
	});
}

async function main() {
	let sessionId = await findToken();
	const rl = readline.createInterface({ input: process.stdin });
	for await (const line of rl) {
		const text = line.trim();
		if (!text) continue;
		let payload;
		try {
			payload = JSON.parse(text);
		} catch {
			continue;
		}
		let { status, data, sessionId: sid } = await post(payload, sessionId);
		sessionId = sid;
		if (data.includes("-32001")) {
			// Stale token — the newest session file may belong to another
			// project. Walk candidates newest-first until one is accepted.
			for (const candidate of tokenCandidates()) {
				sessionId = fs.readFileSync(candidate, "utf8").trim();
				({ status, data, sessionId: sid } = await post(payload, sessionId));
				sessionId = sid;
				if (!data.includes("-32001")) break;
			}
		}
		if (status === 202 || status === 204 || !data) continue; // notifications
		process.stdout.write(data + "\n");
	}
}

main();
