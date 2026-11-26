#include "mcp_service.hpp"

#include "api_index.hpp"
#include "bridge.hpp"
#include "js_worker.hpp"

#include <random>
#include <thread>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/gd_extension_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "third_party/httplib.h"

using namespace godot;

namespace godot_cm {

static const char *PROTOCOL_VERSION = "2025-11-25";
static const char *SERVER_NAME = "godot-cm";
static const char *SERVER_VERSION = "0.2.1";

void McpService::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "port"), &McpService::start);
	ClassDB::bind_method(D_METHOD("stop"), &McpService::stop);
	ClassDB::bind_method(D_METHOD("is_running"), &McpService::is_running);
	ClassDB::bind_method(D_METHOD("get_port"), &McpService::get_port);
	ClassDB::bind_method(D_METHOD("get_session_token"), &McpService::get_session_token);
	ClassDB::bind_method(D_METHOD("respond_tier_request", "tier", "granted"), &McpService::respond_tier_request);
	ClassDB::bind_method(D_METHOD("get_unlocked_tiers"), &McpService::get_unlocked_tiers);
	ClassDB::bind_method(D_METHOD("set_auto_grant", "enabled"), &McpService::set_auto_grant);
	ClassDB::bind_method(D_METHOD("get_tiers"), &McpService::get_tiers);
	ClassDB::bind_method(D_METHOD("get_tier_classes", "tier"), &McpService::get_tier_classes);
	ClassDB::bind_method(D_METHOD("get_tier_policy", "tier"), &McpService::get_tier_policy);
	ClassDB::bind_method(D_METHOD("set_tier_policy", "tier", "policy"), &McpService::set_tier_policy);
	ADD_SIGNAL(MethodInfo("tier_request",
			PropertyInfo(Variant::STRING, "tier"),
			PropertyInfo(Variant::ARRAY, "classes")));
}

static String random_token() {
	std::random_device rd;
	std::mt19937_64 gen(rd());
	std::uniform_int_distribution<uint64_t> dist;
	return String::num_uint64(dist(gen), 16) + String::num_uint64(dist(gen), 16);
}

static Dictionary error_fallback(const String &code, const String &message, const String &phase) {
	Dictionary d;
	d["ok"] = false;
	d["result"] = Variant();
	d["changes"] = Array();
	d["truncated"] = false;
	Dictionary err;
	err["code"] = code;
	err["message"] = message;
	err["phase"] = phase;
	Array diags;
	diags.push_back(err);
	d["diagnostics"] = diags;
	return d;
}

static bool origin_allowed(const httplib::Request &req) {
	if (!req.has_header("Origin")) {
		return true;
	}
	std::string origin = req.get_header_value("Origin");
	return origin.rfind("http://localhost", 0) == 0 ||
			origin.rfind("http://127.0.0.1", 0) == 0 ||
			origin.rfind("https://localhost", 0) == 0 ||
			origin.rfind("https://127.0.0.1", 0) == 0;
}

bool McpService::is_running() const {
	return running.load();
}

int McpService::get_port() const {
	return port;
}

String McpService::get_session_token() const {
	std::lock_guard<std::mutex> lock(session_mutex);
	return String(session_token.c_str());
}

void McpService::respond_tier_request(const String &tier, bool granted) {
	if (granted && api_index) {
		api_index->unlock_tier(tier);
	}
	if (bridge) {
		bridge->resolve_tier(tier, granted);
	}
	UtilityFunctions::print("[godot-cm] tier '", tier, granted ? "' granted" : "' denied");
}

Array McpService::get_unlocked_tiers() const {
	return api_index ? api_index->unlocked_tiers() : Array();
}

void McpService::set_auto_grant(bool enabled) {
	if (api_index && enabled) {
		api_index->unlock_all_tiers();
	}
	UtilityFunctions::print("[godot-cm] auto-grant tiers: ", enabled ? "ON (session)" : "off");
}

Array McpService::get_tiers() const {
	return api_index ? api_index->tiers() : Array();
}

Array McpService::get_tier_classes(const String &tier) const {
	return api_index ? api_index->tier_class_names(tier) : Array();
}

String McpService::get_tier_policy(const String &tier) const {
	return api_index ? api_index->policy_of(tier) : "deny";
}

void McpService::set_tier_policy(const String &tier, const String &policy) {
	if (api_index) {
		api_index->set_tier_policy(tier, policy);
		// Source of truth: tightening the policy re-locks the tier for this
		// session; a looser one grants it right away.
		if (policy == "allow") {
			api_index->unlock_tier(tier);
		} else {
			api_index->lock_tier(tier);
		}
	}
	UtilityFunctions::print("[godot-cm] tier '", tier, "' policy -> ", policy);
}

Variant McpService::handle_initialize(const Variant &id, const Dictionary &params) {
	Dictionary result;
	result["protocolVersion"] = PROTOCOL_VERSION;
	Dictionary caps;
	Dictionary tools_caps;
	tools_caps["listChanged"] = false;
	caps["tools"] = tools_caps;
	result["capabilities"] = caps;
	Dictionary info;
	info["name"] = SERVER_NAME;
	info["version"] = SERVER_VERSION;
	result["serverInfo"] = info;

	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	response["result"] = result;
	return response;
}

Variant McpService::handle_tools_list(const Variant &id) {
	Array tools;

	Dictionary search;
	search["name"] = "search";
	search["description"] = "Run read-only JS over a consistent snapshot of the runtime API catalog (api.classes/api.describe/api.find), including installed native extensions and project script classes. Uncached scripts have partial metadata; discovery never loads or instantiates them.";
	Dictionary search_schema;
	search_schema["type"] = "object";
	Dictionary search_props;
	Dictionary code_prop;
	code_prop["type"] = "string";
	search_props["code"] = code_prop;
	search_schema["properties"] = search_props;
	Array search_req;
	search_req.push_back("code");
	search_schema["required"] = search_req;
	search["inputSchema"] = search_schema;
	tools.push_back(search);

	Dictionary exec;
	exec["name"] = "execute";
	exec["description"] = "Run async JS against the editor. mode: read|write|untracked (default read). "
			"read permits queries and native const methods; write journals supported edits and rejects methods without an inverse. "
			"untracked permits generic native and project-script execution without rollback after await godot.requestTier('untracked') is granted. "
			"Untracked code may have arbitrary effects, including file/network access; this is not a sandbox for plugin code. "
			"Failures retain prior effects; inspect effects and partial before retrying. Denied classes/members and direct-use permission tiers remain enforced. "
			"Use api.describe for signatures/constants, godot.create(className), godot.getSingleton(name), and godot.call(handleOrStaticClass, method, ...args). "
			"Project constructors and custom script members require untracked mode; uncached project constructors also require filesystem permission. "
			"Installed plugins require no adapters or core recompilation. requestTier/unlockedTiers manage per-session grants.";
	Dictionary exec_schema;
	exec_schema["type"] = "object";
	Dictionary exec_props;
	exec_props["code"] = code_prop;
	Dictionary timeout_prop;
	timeout_prop["type"] = "integer";
	timeout_prop["description"] =
			"Cooperative budget in ms (default 5000 read / 60000 write or untracked). "
			"write edits roll back on failure; untracked effects never roll back. Native calls cannot be forcibly interrupted.";
	exec_props["timeoutMs"] = timeout_prop;
	Dictionary mode_prop;
	mode_prop["type"] = "string";
	Array modes;
	modes.push_back("read");
	modes.push_back("write");
	modes.push_back("untracked");
	mode_prop["enum"] = modes;
	mode_prop["default"] = "read";
	exec_props["mode"] = mode_prop;
	exec_schema["properties"] = exec_props;
	Array exec_req;
	exec_req.push_back("code");
	exec_schema["required"] = exec_req;
	exec["inputSchema"] = exec_schema;
	tools.push_back(exec);

	Dictionary result;
	result["tools"] = tools;
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	response["result"] = result;
	return response;
}

Variant McpService::handle_tools_call(const Variant &id, const Dictionary &params) {
	String name = params.get("name", "");
	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	if (name != "search" && name != "execute") {
		Dictionary error;
		error["code"] = -32602;
		error["message"] = "unknown tool: " + name;
		response["error"] = error;
		return response;
	}

	bool is_error = false;
	Dictionary exec_result;
	Dictionary args;
	if (params.has("arguments") && params["arguments"].get_type() == Variant::DICTIONARY) {
		args = params["arguments"];
	}

	if (name == "execute") {
		ExecRequest req;
		req.code = String(args.get("code", "")).utf8().get_data();
		req.mode = String(args.get("mode", "read")).utf8().get_data();
		if (args.has("timeoutMs")) {
			req.timeout_ms = (int64_t)args["timeoutMs"];
		}
		req.cancel = std::make_shared<std::atomic<bool>>(false);
		req.tx = std::make_shared<TxState>();
		String rid = id.stringify();
		{
			std::lock_guard<std::mutex> lock(session_mutex);
			pending_execs[rid.utf8().get_data()] = req.cancel;
		}
		if (js) {
			exec_result = js->submit(req);
		} else {
			exec_result = error_fallback("NO_RUNTIME", "JS worker not running", "init");
		}
		{
			std::lock_guard<std::mutex> lock(session_mutex);
			pending_execs.erase(rid.utf8().get_data());
		}
		is_error = !exec_result.get("ok", false);
	} else {
		ExecRequest req;
		req.code = String(args.get("code", "")).utf8().get_data();
		req.search_only = true;
		if (js && api_index && api_index->is_loaded()) {
			exec_result = js->submit(req);
		} else {
			exec_result = error_fallback("NO_INDEX", "API index not loaded", "init");
		}
		is_error = !exec_result.get("ok", false);
	}

	Array content;
	Dictionary item;
	item["type"] = "text";
	Ref<JSON> j;
	j.instantiate();
	item["text"] = j->stringify(exec_result);
	content.push_back(item);
	Dictionary result;
	result["content"] = content;
	result["isError"] = is_error;
	response["result"] = result;
	return response;
}

Variant McpService::dispatch_rpc(const Dictionary &request) {
	String method = request.get("method", "");
	Variant id = request.get("id", Variant());
	Dictionary params;
	if (request.has("params") && request["params"].get_type() == Variant::DICTIONARY) {
		params = request["params"];
	}

	if (method == "initialize") {
		return handle_initialize(id, params);
	}
	if (method == "ping") {
		Dictionary response;
		response["jsonrpc"] = "2.0";
		response["id"] = id;
		response["result"] = Dictionary();
		return response;
	}
	if (method == "tools/list") {
		return handle_tools_list(id);
	}
	if (method == "tools/call") {
		return handle_tools_call(id, params);
	}
	if (method == "notifications/cancelled") {
		std::lock_guard<std::mutex> lock(session_mutex);
		std::string rid = params.get("requestId", Variant()).stringify().utf8().get_data();
		auto it = pending_execs.find(rid);
		if (it != pending_execs.end()) {
			it->second->store(true);
		}
		return Variant(); // notification: no response
	}
	if (method == "notifications/initialized" || method.begins_with("notifications/")) {
		return Variant();
	}

	Dictionary response;
	response["jsonrpc"] = "2.0";
	response["id"] = id;
	Dictionary error;
	error["code"] = -32601;
	error["message"] = "method not found";
	response["error"] = error;
	return response;
}

bool McpService::start(int p_port) {
	if (running.load()) {
		return true;
	}
	session_token = random_token().utf8().get_data();
	session_initialized = false;
	pending_execs.clear();

	server = std::make_unique<httplib::Server>();
	server->set_socket_options([](socket_t socket) {
#ifdef _WIN32
		httplib::detail::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
		httplib::detail::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
	});
	if (!server->bind_to_port("127.0.0.1", p_port)) {
		server.reset();
		return false;
	}

	api_index = std::make_unique<ApiIndex>();
	if (!api_index->load("res://addons/godot_cm/allowlist.json")) return false;
	if (OS::get_singleton()->get_environment("GODOT_CM_GENERATE_DTS") == "1") {
		api_index->generate_dts("user://godot_cm-api-surface.d.ts");
	}

	bridge = std::make_unique<Bridge>();
	bridge->index = api_index.get();
	// request_tier ops park until the user answers; plugin.gd shows the
	// confirmation dialog on this signal (main thread via drain()).
	bridge->tier_request_cb = [this](const String &tier) {
		Array cls = api_index ? api_index->tier_class_names(tier) : Array();
		emit_signal("tier_request", tier, cls);
	};

	// Pump drains bridge ops on the main thread each frame (D3/D9).
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree) {
		pump.instantiate();
		pump->bridge = bridge.get();
		tree->connect("process_frame", Callable(pump.ptr(), "pump"));
		GDExtensionManager::get_singleton()->connect("extensions_reloaded", Callable(pump.ptr(), "invalidate_catalog"));
	} else {
		UtilityFunctions::printerr("[godot-cm] no SceneTree; bridge disabled");
	}

	js = std::make_unique<JsWorker>();
	js->start(api_index.get(), bridge.get());

	server->Post("/mcp", [this](const httplib::Request &req, httplib::Response &res) {
		res.set_header("Cache-Control", "no-cache");

		if (!origin_allowed(req)) {
			res.status = 403;
			res.set_content("{\"error\":\"disallowed origin\"}", "application/json");
			return;
		}

		Ref<JSON> json;
		json.instantiate();
		Error err = json->parse(String::utf8(req.body.c_str(), req.body.size()));
		if (err != OK || json->get_data().get_type() != Variant::DICTIONARY) {
			res.status = 400;
			res.set_content("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"parse error\"}}",
					"application/json");
			return;
		}
		Dictionary rpc_req = json->get_data();
		String method = rpc_req.get("method", "");

		{
			std::lock_guard<std::mutex> lock(session_mutex);
			if (session_initialized) {
				std::string sid = req.get_header_value("MCP-Session-Id");
				if (sid != session_token) {
					res.status = 401;
					res.set_content("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32001,\"message\":\"invalid session token\"}}",
							"application/json");
					return;
				}
			}
		}

		Variant rpc_res = dispatch_rpc(rpc_req);
		if (method == "initialize") {
			std::lock_guard<std::mutex> lock(session_mutex);
			session_initialized = true;
		}

		if (rpc_res.get_type() == Variant::NIL) {
			res.status = 202;
			res.set_content("", "application/json");
			return;
		}
		res.set_header("MCP-Session-Id", session_token);
		Ref<JSON> out;
		out.instantiate();
		res.set_content(out->stringify(rpc_res).utf8().get_data(), "application/json");
	});

	server->Get("/mcp", [](const httplib::Request &, httplib::Response &res) {
		res.status = 405;
		res.set_header("Allow", "POST");
	});

	server->Delete("/mcp", [this](const httplib::Request &, httplib::Response &res) {
		std::lock_guard<std::mutex> lock(session_mutex);
		session_initialized = false;
		pending_execs.clear();
		res.status = 200;
	});

	port = p_port;
	running.store(true);
	server_thread = new std::thread([this]() {
		if (!server->listen_after_bind()) {
			running.store(false);
			UtilityFunctions::printerr("[godot-cm] MCP server failed to bind 127.0.0.1:", port);
		}
	});

	// Brief spin to confirm bind before reporting success.
	for (int i = 0; i < 100 && !server->is_running(); i++) {
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	bool ok = server->is_running();
	if (!ok) {
		stop();
		return false;
	}
	// Publish the session token for the stdio adapter (D5: file/env, never argv).
	String token_path = ProjectSettings::get_singleton()->globalize_path("user://godot_cm_session");
	Ref<FileAccess> f = FileAccess::open(token_path, FileAccess::WRITE);
	if (f.is_valid()) {
		f->store_string(session_token.c_str());
		f->close();
		UtilityFunctions::print("[godot-cm] session token written to ", token_path);
	}

	UtilityFunctions::print("[godot-cm] MCP server on 127.0.0.1:", port);
	return true;
}

void McpService::stop() {
	if (pump.is_valid()) {
		SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
		if (tree && tree->is_connected("process_frame", Callable(pump.ptr(), "pump"))) {
			tree->disconnect("process_frame", Callable(pump.ptr(), "pump"));
		}
		GDExtensionManager *extensions = GDExtensionManager::get_singleton();
		Callable refresh(pump.ptr(), "invalidate_catalog");
		if (extensions && extensions->is_connected("extensions_reloaded", refresh)) extensions->disconnect("extensions_reloaded", refresh);
		pump.unref();
	}
	if (bridge) {
		bridge->reset();
		bridge.reset();
	}
	if (js) {
		js->stop();
		js.reset();
	}
	if (server) {
		server->stop();
	}
	if (server_thread) {
		if (server_thread->joinable()) {
			server_thread->join();
		}
		delete server_thread;
		server_thread = nullptr;
	}
	server.reset();
	{
		std::lock_guard<std::mutex> lock(session_mutex);
		session_initialized = false;
		pending_execs.clear();
	}
	running.store(false);
	UtilityFunctions::print("[godot-cm] MCP server stopped");
}

McpService::~McpService() {
	stop();
}

} // namespace godot_cm
