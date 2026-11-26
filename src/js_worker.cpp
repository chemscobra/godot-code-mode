#include "js_worker.hpp"

#include <chrono>

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "api_index.hpp"
#include "bridge.hpp"

extern "C" {
#include <quickjs.h>
}

using namespace godot;

namespace godot_cm {

namespace {
constexpr int64_t DEFAULT_TIMEOUT_MS = 5000;
constexpr int64_t DEFAULT_WRITE_TIMEOUT_MS = 60000;
constexpr size_t HEAP_LIMIT = 32u << 20;   // 32 MiB
constexpr size_t OUTPUT_LIMIT = 256 << 10; // 256 KiB
constexpr int64_t OP_LIMIT = 1000;         // bridge ops (also interrupt ticks)

struct ExecBudget {
	std::chrono::steady_clock::time_point deadline;
	std::atomic<int64_t> ops{0};
	std::atomic<bool> tripped{false};
};

int interrupt_handler(JSContext * /*ctx*/, void *opaque) {
	ExecBudget *b = static_cast<ExecBudget *>(opaque);
	if (b->tripped.load()) {
		return 1;
	}
	if (std::chrono::steady_clock::now() > b->deadline || b->ops.load() > OP_LIMIT) {
		b->tripped.store(true);
		return 1;
	}
	return 0;
}

Dictionary base_result(bool ok) {
	Dictionary d;
	d["ok"] = ok;
	d["result"] = Variant();
	d["diagnostics"] = Array();
	d["changes"] = Array();
	d["truncated"] = false;
	return d;
}

Dictionary error_result(const String &code, const String &message, const String &phase) {
	Dictionary d = base_result(false);
	Dictionary err;
	err["code"] = code;
	err["message"] = message;
	err["phase"] = phase;
	Array diags;
	diags.push_back(err);
	d["diagnostics"] = diags;
	return d;
}

// Variant -> JSValue for api.* return values (metadata surface only).
JSValue variant_to_js(JSContext *ctx, const Variant &v) {
	switch (v.get_type()) {
		case Variant::NIL:
			return JS_UNDEFINED;
		case Variant::BOOL:
			return JS_NewBool(ctx, (bool)v);
		case Variant::INT:
			return JS_NewInt64(ctx, (int64_t)v);
		case Variant::FLOAT:
			return JS_NewFloat64(ctx, (double)v);
		case Variant::STRING:
		case Variant::STRING_NAME: {
			CharString s = String(v).utf8();
			return JS_NewStringLen(ctx, s.get_data(), s.length());
		}
		case Variant::ARRAY: {
			Array a = v;
			JSValue arr = JS_NewArray(ctx);
			for (int i = 0; i < a.size(); i++) {
				JS_SetPropertyUint32(ctx, arr, i, variant_to_js(ctx, a[i]));
			}
			return arr;
		}
		case Variant::DICTIONARY: {
			Dictionary d = v;
			JSValue obj = JS_NewObject(ctx);
			Array keys = d.keys();
			for (int i = 0; i < keys.size(); i++) {
				String k = String(keys[i]);
				CharString ks = k.utf8();
				JS_SetPropertyStr(ctx, obj, ks.get_data(), variant_to_js(ctx, d[keys[i]]));
			}
			return obj;
		}
		default: {
			CharString s = v.stringify().utf8();
			return JS_NewStringLen(ctx, s.get_data(), s.length());
		}
	}
}

struct ExecCtx; // fwd

ExecCtx *exec_ctx(JSContext *ctx) {
	return static_cast<ExecCtx *>(JS_GetContextOpaque(ctx));
}

ApiIndex *ctx_index(JSContext *ctx);

JSValue js_api_classes(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	ApiIndex *idx = ctx_index(ctx);
	if (!idx) {
		return JS_ThrowInternalError(ctx, "api index not loaded");
	}
	return variant_to_js(ctx, idx->classes());
}

JSValue js_api_describe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	ApiIndex *idx = ctx_index(ctx);
	if (!idx || argc < 1) {
		return JS_ThrowInternalError(ctx, "api.describe(name) needs an argument");
	}
	const char *n = JS_ToCString(ctx, argv[0]);
	Variant out = idx->describe(String::utf8(n ? n : ""));
	if (n) {
		JS_FreeCString(ctx, n);
	}
	return variant_to_js(ctx, out);
}

JSValue js_api_find(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	ApiIndex *idx = ctx_index(ctx);
	if (!idx || argc < 1) {
		return JS_ThrowInternalError(ctx, "api.find(query) needs an argument");
	}
	const char *n = JS_ToCString(ctx, argv[0]);
	Array out = idx->find(String::utf8(n ? n : ""));
	if (n) {
		JS_FreeCString(ctx, n);
	}
	return variant_to_js(ctx, out);
}

// ---- JS side of the editor bridge (spec godot-editor-bridge) --------------
// Every godot.* call enqueues a BridgeOp resolved on the main thread and
// returns a Promise; the drain loop below resolves/rejects as ops complete.

struct InflightOp {
	std::shared_ptr<PendingBridge> pb;
	JSValue resolve;
	JSValue reject;
};

struct ExecCtx {
	Bridge *bridge = nullptr;
	ApiIndex *index = nullptr;
	ExecBudget *budget = nullptr;
	std::string mode = "read";
	std::shared_ptr<std::atomic<bool>> cancel;
	std::shared_ptr<TxState> tx;
	std::deque<InflightOp> inflight;
	godot::Array warnings; // non-fatal op warnings folded into diagnostics
};

ApiIndex *ctx_index(JSContext *ctx) {
	ExecCtx *ec = exec_ctx(ctx);
	return ec ? ec->index : nullptr;
}

Variant unwrap_tagged(const Variant &p);

// JSValue -> Variant. {__godotHandle:"hN"} -> {"$handle":N} for the bridge;
// {$t:"Vector2/3/Color",...} -> tagged engine types; objects via JSON round-trip.
Variant js_to_variant(JSContext *ctx, JSValueConst v) {
	int tag = JS_VALUE_GET_TAG(v);
	switch (tag) {
		case JS_TAG_UNDEFINED:
		case JS_TAG_NULL:
			return Variant();
		case JS_TAG_BOOL:
			return Variant((bool)JS_VALUE_GET_BOOL(v));
		case JS_TAG_INT:
			return Variant((int64_t)JS_VALUE_GET_INT(v));
		case JS_TAG_FLOAT64:
			return Variant(JS_VALUE_GET_FLOAT64(v));
		case JS_TAG_STRING: {
			const char *s = JS_ToCString(ctx, v);
			String out = s ? String::utf8(s) : String();
			if (s) {
				JS_FreeCString(ctx, s);
			}
			return out;
		}
		default:
			break;
	}
	// Objects/arrays: check handle marker first.
	JSValue hprop = JS_GetPropertyStr(ctx, v, "__godotHandle");
	if (JS_IsString(hprop)) {
		const char *s = JS_ToCString(ctx, hprop);
		String hs = s ? String::utf8(s) : String();
		if (s) {
			JS_FreeCString(ctx, s);
		}
		JS_FreeValue(ctx, hprop);
		Dictionary d;
		if (hs.begins_with("h")) {
			d["$handle"] = (uint64_t)hs.substr(1).to_int();
		}
		return d;
	}
	JS_FreeValue(ctx, hprop);

	JSValue json = JS_JSONStringify(ctx, v, JS_UNDEFINED, JS_UNDEFINED);
	const char *s = JS_ToCString(ctx, json);
	String text = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	JS_FreeValue(ctx, json);
	if (text.is_empty()) {
		return Variant();
	}
	Ref<JSON> j;
	j.instantiate();
	if (j->parse(text) != OK) {
		return Variant();
	}
	Variant parsed = j->get_data();
	return unwrap_tagged(parsed);
}

// Recursively resolves {"$t":...} tagged engine types inside arrays and
// dictionaries so e.g. [{$t:"Vector2",...}, ...] becomes real Vector2s
// (which the bridge can then convert to PackedVector2Array etc.).
Variant unwrap_tagged(const Variant &p) {
	if (p.get_type() == Variant::ARRAY) {
		Array a = p;
		for (int i = 0; i < a.size(); i++) {
			a[i] = unwrap_tagged(a[i]);
		}
		return a;
	}
	if (p.get_type() != Variant::DICTIONARY) {
		return p;
	}
	Dictionary d = p;
	if (d.has("$t")) {
		String t = d["$t"];
		if (t == "Vector2") {
			return Vector2((double)d.get("x", 0.0), (double)d.get("y", 0.0));
		}
		if (t == "Vector3") {
			return Vector3((double)d.get("x", 0.0), (double)d.get("y", 0.0), (double)d.get("z", 0.0));
		}
		if (t == "Color") {
			return Color((double)d.get("r", 0.0), (double)d.get("g", 0.0),
					(double)d.get("b", 0.0), (double)d.get("a", 1.0));
		}
		return p;
	}
	for (const Variant &k : d.keys()) {
		d[k] = unwrap_tagged(d[k]);
	}
	return d;
}

bool arg_is_write(const char *kind, const char *member) {
	String k = kind;
	if (k == "create" || k == "set" || k == "save_scene" || k == "unlock_writes" ||
			k == "connect" || k == "disconnect") {
		return true;
	}
	if (k == "call") {
		String m = member;
		// Read-shaped calls don't count as writes.
		static const char *read_prefixes[] = {"get_", "is_", "has_", "find_", "list_", "to_"};
		for (const char *p : read_prefixes) {
			if (m.begins_with(p)) {
				return false;
			}
		}
		return true;
	}
	return false;
}

JSValue reject_value(JSContext *ctx, const BridgeResult &r) {
	JSValue err = JS_NewError(ctx);
	JS_SetPropertyStr(ctx, err, "code", JS_NewString(ctx, r.error_code.utf8().get_data()));
	JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, r.error_message.utf8().get_data()));
	return err;
}

// Shared submit for all godot.* ops: enqueue -> promise.
JSValue submit_op(JSContext *ctx, const char *kind, uint64_t target,
		const char *member, const Array &args, const char *class_name = "",
		const Array &elements = Array(), int64_t op_count = 1) {
	ExecCtx *ec = exec_ctx(ctx);
	if (!ec || !ec->bridge) {
		return JS_ThrowInternalError(ctx, "bridge not available");
	}
	ExecCtx *ec_ref = ec;
	BridgeOp op;
	op.kind = kind;
	op.target = target;
	op.member = member ? member : "";
	op.class_name = class_name ? class_name : "";
	op.args = args;
	op.elements = elements;
	op.write = arg_is_write(kind, member);
	op.write_mode = (ec->mode == "write");
	if (ec->budget) {
		ec->budget->ops.fetch_add(op_count);
	}
	if (op.write && ec_ref->mode != "write") {
		return JS_ThrowInternalError(ctx, "PERMISSION: write op in read mode");
	}
	auto pb = ec->bridge->enqueue(op, ec->cancel, ec->tx);

	JSValue funcs[2];
	JSValue promise = JS_NewPromiseCapability(ctx, funcs);
	InflightOp io;
	io.pb = pb;
	io.resolve = funcs[0];
	io.reject = funcs[1];
	ec->inflight.push_back(io);
	return promise;
}

uint64_t handle_id_of(JSContext *ctx, JSValueConst v) {
	Variant hv = js_to_variant(ctx, v);
	if (hv.get_type() == Variant::DICTIONARY && ((Dictionary)hv).has("$handle")) {
		return (uint64_t)((Dictionary)hv)["$handle"];
	}
	return 0;
}

JSValue js_godot_create(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsString(argv[0])) {
		return JS_ThrowInternalError(ctx, "godot.create(className)");
	}
	const char *s = JS_ToCString(ctx, argv[0]);
	String cls = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	return submit_op(ctx, "create", 0, "", Array(), cls.utf8().get_data());
}

JSValue js_godot_editor_scene(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	return submit_op(ctx, "editor_scene_root", 0, "", Array());
}

JSValue js_godot_save_scene(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	return submit_op(ctx, "save_scene", 0, "", Array());
}

JSValue js_godot_unlock_writes(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	return submit_op(ctx, "unlock_writes", 0, "", Array());
}

// godot.requestTier("network") -> resolves once the user answers the
// confirmation dialog in the editor.
JSValue js_godot_request_tier(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsString(argv[0])) {
		return JS_ThrowInternalError(ctx, "godot.requestTier(tier)");
	}
	const char *s = JS_ToCString(ctx, argv[0]);
	String tier = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	return submit_op(ctx, "request_tier", 0, tier.utf8().get_data(), Array());
}

JSValue js_godot_unlocked_tiers(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	return submit_op(ctx, "unlocked_tiers", 0, "", Array());
}

// godot.batch(ops): ops are plain objects {op, target?, prop?|method?|member?,
// class?, args?|value?}. Returns one promise resolving to per-element results.
JSValue js_godot_batch(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsArray(argv[0])) {
		return JS_ThrowInternalError(ctx, "godot.batch(ops[])");
	}
	JSValue len_v = JS_GetPropertyStr(ctx, argv[0], "length");
	int64_t len = 0;
	JS_ToInt64(ctx, &len, len_v);
	JS_FreeValue(ctx, len_v);
	if (len > OP_LIMIT) {
		return JS_ThrowInternalError(ctx, "godot.batch: element count exceeds op budget");
	}
	Array elements;
	for (int64_t i = 0; i < len; i++) {
		JSValue el = JS_GetPropertyUint32(ctx, argv[0], i);
		Variant dv = js_to_variant(ctx, el);
		JS_FreeValue(ctx, el);
		if (dv.get_type() != Variant::DICTIONARY) {
			return JS_ThrowInternalError(ctx, "godot.batch: elements must be objects");
		}
		Dictionary d = dv;
		String kind = d.get("op", d.get("kind", ""));
		String member = d.get("member", d.get("prop", d.get("method", "")));
		Dictionary out;
		out["kind"] = kind;
		out["member"] = member;
		out["class_name"] = d.get("class", d.get("class_name", ""));
		uint64_t target = 0;
		Variant tv = d.get("target", Variant());
		if (tv.get_type() == Variant::DICTIONARY && ((Dictionary)tv).has("$handle")) {
			target = (uint64_t)((Dictionary)tv)["$handle"];
		}
		out["target"] = target;
		Array args = d.get("args", Array());
		if (kind == "set" && args.is_empty() && d.has("value")) {
			args.push_back(d["value"]);
		}
		out["args"] = args;
		out["write"] = arg_is_write(kind.utf8().get_data(), member.utf8().get_data());
		elements.push_back(out);
	}
	return submit_op(ctx, "batch", 0, "", Array(), "", elements, len);
}

JSValue js_godot_get(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_ThrowInternalError(ctx, "godot.get(handle, prop)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = JS_ToCString(ctx, argv[1]);
	String prop = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	return submit_op(ctx, "get", hid, prop.utf8().get_data(), Array());
}

JSValue js_godot_set(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 3) {
		return JS_ThrowInternalError(ctx, "godot.set(handle, prop, value)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = JS_ToCString(ctx, argv[1]);
	String prop = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	args.push_back(js_to_variant(ctx, argv[2]));
	return submit_op(ctx, "set", hid, prop.utf8().get_data(), args);
}

JSValue js_godot_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_ThrowInternalError(ctx, "godot.call(handle, method, ...args)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = JS_ToCString(ctx, argv[1]);
	String method = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	for (int i = 2; i < argc; i++) {
		args.push_back(js_to_variant(ctx, argv[i]));
	}
	return submit_op(ctx, "call", hid, method.utf8().get_data(), args);
}

// godot.connect(emitter, signal, to, method, flags?, binds?)
JSValue js_godot_connect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 4) {
		return JS_ThrowInternalError(ctx, "godot.connect(handle, signal, toHandle, method)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = JS_ToCString(ctx, argv[1]);
	String sig = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	for (int i = 2; i < argc; i++) {
		args.push_back(js_to_variant(ctx, argv[i]));
	}
	return submit_op(ctx, "connect", hid, sig.utf8().get_data(), args);
}

// godot.disconnect(emitter, signal, to, method)
JSValue js_godot_disconnect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 4) {
		return JS_ThrowInternalError(ctx, "godot.disconnect(handle, signal, toHandle, method)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = JS_ToCString(ctx, argv[1]);
	String sig = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	for (int i = 2; i < argc; i++) {
		args.push_back(js_to_variant(ctx, argv[i]));
	}
	return submit_op(ctx, "disconnect", hid, sig.utf8().get_data(), args);
}

// Resolve/reject all completed ops. Called from the drain loop.
void sweep_inflight(JSContext *ctx, ExecCtx *ec) {
	auto it = ec->inflight.begin();
	while (it != ec->inflight.end()) {
		if (!it->pb->done.load()) {
			++it;
			continue;
		}
		BridgeResult r = it->pb->result;
		for (int w = 0; w < r.warnings.size(); w++) {
			ec->warnings.push_back(r.warnings[w]);
		}
		if (r.ok) {
			JSValue arg = variant_to_js(ctx, r.value);
			JSValue ret = JS_Call(ctx, it->resolve, JS_UNDEFINED, 1, &arg);
			JS_FreeValue(ctx, ret);
			JS_FreeValue(ctx, arg);
		} else {
			JSValue arg = reject_value(ctx, r);
			JSValue ret = JS_Call(ctx, it->reject, JS_UNDEFINED, 1, &arg);
			JS_FreeValue(ctx, ret);
			JS_FreeValue(ctx, arg);
		}
		JS_FreeValue(ctx, it->resolve);
		JS_FreeValue(ctx, it->reject);
		it = ec->inflight.erase(it);
	}
}

void free_inflight(JSContext *ctx, ExecCtx *ec) {
	for (auto &io : ec->inflight) {
		JS_FreeValue(ctx, io.resolve);
		JS_FreeValue(ctx, io.reject);
	}
	ec->inflight.clear();
}

// Blocking sentinel op (tx_commit / tx_rollback): no JS promise involved —
// the worker waits on the shared cell like any other bridge op.
BridgeResult wait_bridge_op(Bridge *b, const char *kind, ExecCtx *ec) {
	BridgeOp op;
	op.kind = kind;
	auto pb = b->enqueue(op, nullptr, ec->tx);
	while (!pb->done.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return pb->result;
}
} // namespace

void JsWorker::start(ApiIndex *p_index, Bridge *p_bridge) {
	api_index = p_index;
	bridge = p_bridge;
	worker = std::thread([this]() { run(); });
}

void JsWorker::stop() {
	{
		std::lock_guard<std::mutex> lock(mutex);
		quit.store(true);
	}
	cv.notify_all();
	if (worker.joinable()) {
		worker.join();
	}
}

JsWorker::~JsWorker() {
	stop();
}

Dictionary JsWorker::submit(const ExecRequest &req) {
	auto job = std::make_shared<Job>();
	job->req = req;
	job->result = std::make_shared<Dictionary>();
	job->done = std::make_shared<std::condition_variable>();
	{
		std::lock_guard<std::mutex> lock(mutex);
		queue.push_back(job);
	}
	cv.notify_one();

	int64_t budget_ms = req.timeout_ms;
	if (budget_ms <= 0) {
		budget_ms = req.mode == "write" ? DEFAULT_WRITE_TIMEOUT_MS : DEFAULT_TIMEOUT_MS;
	}
	std::unique_lock<std::mutex> lock(mutex);
	bool finished = job->done->wait_for(lock, std::chrono::milliseconds(budget_ms + 10000),
			[&] { return !job->result->is_empty() || quit.load(); });
	if (!finished) {
		return error_result("WORKER_TIMEOUT", "execution exceeded worker deadline", "execute");
	}
	return *job->result;
}

void JsWorker::run() {
	while (true) {
		std::shared_ptr<Job> job;
		{
			std::unique_lock<std::mutex> lock(mutex);
			cv.wait(lock, [&] { return !queue.empty() || quit.load(); });
			if (quit.load()) {
				return;
			}
			job = queue.front();
			queue.pop_front();
		}
		Dictionary res = execute_one(job->req);
		*job->result = res;
		job->done->notify_all();
	}
}

Dictionary JsWorker::execute_one(const ExecRequest &req) {
	ExecBudget budget;
	int64_t timeout_ms = req.timeout_ms;
	if (timeout_ms <= 0) {
		timeout_ms = req.mode == "write" ? DEFAULT_WRITE_TIMEOUT_MS : DEFAULT_TIMEOUT_MS;
	}
	budget.deadline = std::chrono::steady_clock::now() +
			std::chrono::milliseconds(timeout_ms);

	JSRuntime *rt = JS_NewRuntime();
	if (!rt) {
		return error_result("RUNTIME_INIT", "failed to create JS runtime", "init");
	}
	JS_SetMemoryLimit(rt, HEAP_LIMIT);
	JS_SetInterruptHandler(rt, interrupt_handler, &budget);
	JSContext *ctx = JS_NewContext(rt);
	if (!ctx) {
		JS_FreeRuntime(rt);
		return error_result("RUNTIME_INIT", "failed to create JS context", "init");
	}

	ExecCtx ec;
	ec.bridge = bridge;
	ec.index = api_index;
	ec.budget = &budget;
	ec.mode = req.mode;
	ec.cancel = req.cancel ? req.cancel : std::make_shared<std::atomic<bool>>(false);
	ec.tx = req.tx ? req.tx : std::make_shared<TxState>();
	JS_SetContextOpaque(ctx, &ec);

	JSValue global = JS_GetGlobalObject(ctx);

	// api.* — immutable authorized metadata surface (search tool).
	if (api_index) {
		JSValue api_obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, api_obj, "classes", JS_NewCFunction(ctx, js_api_classes, "classes", 0));
		JS_SetPropertyStr(ctx, api_obj, "describe", JS_NewCFunction(ctx, js_api_describe, "describe", 1));
		JS_SetPropertyStr(ctx, api_obj, "find", JS_NewCFunction(ctx, js_api_find, "find", 1));
		JS_SetPropertyStr(ctx, global, "api", api_obj);
	}

	if (!req.search_only) {
		JSValue godot_obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, godot_obj, "mode", JS_NewString(ctx, req.mode.c_str()));
		JS_SetPropertyStr(ctx, godot_obj, "create", JS_NewCFunction(ctx, js_godot_create, "create", 1));
		JS_SetPropertyStr(ctx, godot_obj, "get", JS_NewCFunction(ctx, js_godot_get, "get", 2));
		JS_SetPropertyStr(ctx, godot_obj, "set", JS_NewCFunction(ctx, js_godot_set, "set", 3));
		JS_SetPropertyStr(ctx, godot_obj, "call", JS_NewCFunction(ctx, js_godot_call, "call", 2));
		JS_SetPropertyStr(ctx, godot_obj, "connect",
				JS_NewCFunction(ctx, js_godot_connect, "connect", 4));
		JS_SetPropertyStr(ctx, godot_obj, "disconnect",
				JS_NewCFunction(ctx, js_godot_disconnect, "disconnect", 4));
		JS_SetPropertyStr(ctx, godot_obj, "saveScene",
				JS_NewCFunction(ctx, js_godot_save_scene, "saveScene", 0));
		JS_SetPropertyStr(ctx, godot_obj, "unlockWrites",
				JS_NewCFunction(ctx, js_godot_unlock_writes, "unlockWrites", 0));
		JS_SetPropertyStr(ctx, godot_obj, "requestTier",
				JS_NewCFunction(ctx, js_godot_request_tier, "requestTier", 1));
		JS_SetPropertyStr(ctx, godot_obj, "unlockedTiers",
				JS_NewCFunction(ctx, js_godot_unlocked_tiers, "unlockedTiers", 0));
		JS_SetPropertyStr(ctx, godot_obj, "batch",
				JS_NewCFunction(ctx, js_godot_batch, "batch", 1));
		JSValue editor_obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, editor_obj, "getEditedScene",
				JS_NewCFunction(ctx, js_godot_editor_scene, "getEditedScene", 0));
		JS_SetPropertyStr(ctx, godot_obj, "editor", editor_obj);
		JS_SetPropertyStr(ctx, global, "godot", godot_obj);
	}

	// User code becomes the body of an async function; awaiting = async by design.
	std::string wrapped = "(async () => {\n" + req.code + "\n})()";
	JSValue val = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<execute>", JS_EVAL_TYPE_GLOBAL);

	Dictionary out;
	if (JS_IsException(val)) {
		JSValue exc = JS_GetException(ctx);
		JSValue str = JS_ToString(ctx, exc);
		const char *s = JS_ToCString(ctx, str);
		out = error_result(budget.tripped.load() ? "INTERRUPTED" : "JS_ERROR",
				s ? s : "unknown error", "execute");
		if (s) {
			JS_FreeCString(ctx, s);
		}
		JS_FreeValue(ctx, str);
		JS_FreeValue(ctx, exc);
	} else if (JS_VALUE_GET_TAG(val) == JS_TAG_OBJECT &&
			JS_PromiseState(ctx, val) != JS_PROMISE_NOT_A_PROMISE) {
		// Promise-based execution: run microtasks; after each, resolve any
		// bridge ops the main thread has completed (this is the await path).
		JSContext *pctx = nullptr;
		while (true) {
			while (JS_ExecutePendingJob(rt, &pctx)) {
				if (budget.tripped.load()) {
					break;
				}
			}
			sweep_inflight(ctx, &ec);
			// Wall-clock deadline: the interrupt handler only runs during JS
			// eval, so time spent awaiting bridge ops must be checked here.
			if (std::chrono::steady_clock::now() > budget.deadline) {
				budget.tripped.store(true);
			}
			if (budget.tripped.load() || (ec.cancel && ec.cancel->load())) {
				break;
			}
			if (ec.inflight.empty()) {
				// Resolving ops queued await continuations; run them so the
				// outer promise settles. They may submit NEW ops (sequential
				// awaits) — if so, keep pumping instead of leaving.
				while (JS_ExecutePendingJob(rt, &pctx)) {
				}
				if (!ec.inflight.empty()) {
					continue;
				}
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		if (!ec.inflight.empty()) {
			// Loop exited with ops still pending (interrupt or cancel).
			// Reject them all so awaiting code settles instead of leaking
			// live objects into JS_FreeRuntime's gc list.
			const char *code = (ec.cancel && ec.cancel->load()) ? "CANCELLED" : "INTERRUPTED";
			for (auto &io : ec.inflight) {
				JSValue arg = JS_NewError(ctx);
				JS_SetPropertyStr(ctx, arg, "code", JS_NewString(ctx, code));
				JSValue ret = JS_Call(ctx, io.reject, JS_UNDEFINED, 1, &arg);
				JS_FreeValue(ctx, ret);
				JS_FreeValue(ctx, arg);
				JS_FreeValue(ctx, io.resolve);
				JS_FreeValue(ctx, io.reject);
			}
			ec.inflight.clear();
			while (JS_ExecutePendingJob(rt, &pctx)) {
			}
		}
		JSPromiseStateEnum st = JS_PromiseState(ctx, val);
		if (st == JS_PROMISE_FULFILLED) {
			JSValue r = JS_PromiseResult(ctx, val);
			JSValue json = JS_JSONStringify(ctx, r, JS_UNDEFINED, JS_UNDEFINED);
			const char *s = JS_ToCString(ctx, json);
			String text = s ? String::utf8(s) : String();
			if (s) {
				JS_FreeCString(ctx, s);
			}
			out = base_result(true);
			if (text.length() > (int)OUTPUT_LIMIT) {
				// Spec: bounded prefix + truncated flag — a truncated blob
				// won't re-parse, so report it as a string.
				out["result"] = text.substr(0, OUTPUT_LIMIT);
				out["truncated"] = true;
			} else {
				Ref<JSON> j;
				j.instantiate();
				out["result"] = (text.is_empty() || j->parse(text) != OK) ? Variant() : j->get_data();
			}
			JS_FreeValue(ctx, json);
			JS_FreeValue(ctx, r);
		} else if (st == JS_PROMISE_REJECTED) {
			JSValue r = JS_PromiseResult(ctx, val);
			JSValue str = JS_ToString(ctx, r);
			const char *s = JS_ToCString(ctx, str);
			out = error_result("JS_REJECTED", s ? s : "promise rejected", "execute");
			if (s) {
				JS_FreeCString(ctx, s);
			}
			JS_FreeValue(ctx, str);
			JS_FreeValue(ctx, r);
		} else {
			out = error_result(budget.tripped.load() ? "INTERRUPTED" : "UNSETTLED",
					budget.tripped.load() ? "execution interrupted by budget"
										  : "promise never settled (no ops in flight)",
					"execute");
		}
	} else {
		// Non-promise value: user code didn't await anything; still a result.
		JSValue json = JS_JSONStringify(ctx, val, JS_UNDEFINED, JS_UNDEFINED);
		const char *s = JS_ToCString(ctx, json);
		String text = s ? String::utf8(s) : String();
		if (s) {
			JS_FreeCString(ctx, s);
		}
		out = base_result(true);
		if (text.length() > (int)OUTPUT_LIMIT) {
			out["result"] = text.substr(0, OUTPUT_LIMIT);
			out["truncated"] = true;
		} else {
			Ref<JSON> j;
			j.instantiate();
			out["result"] = (text.is_empty() || j->parse(text) != OK) ? Variant() : j->get_data();
		}
		JS_FreeValue(ctx, json);
	}
	JS_FreeValue(ctx, val);
	JS_FreeValue(ctx, global);

	free_inflight(ctx, &ec);

	// Drain pending jobs before freeing: the interrupt handler stays tripped,
	// so queued jobs reject immediately instead of leaking objects into
	// JS_FreeRuntime's gc list.
	{
		JSContext *pctx = nullptr;
		while (JS_ExecutePendingJob(rt, &pctx)) {
		}
	}
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);

	// Transaction epilogue (spec editor-transactions): a successful write
	// execution commits one undoable action; any failure rolls the journal
	// back. Runs after the JS teardown — the journal lives Godot-side.
	if (ec.tx && !ec.tx->journal.is_empty()) {
		bool success = out.get("ok", false) && !ec.tx->conflict.load();
		BridgeResult txr = wait_bridge_op(bridge,
				success ? "tx_commit" : "tx_rollback", &ec);
		if (!txr.ok) {
			out["ok"] = false;
			Array diags = out.get("diagnostics", Array());
			Dictionary d;
			d["code"] = txr.error_code;
			d["message"] = txr.error_message;
			d["phase"] = "transaction";
			diags.push_back(d);
			out["diagnostics"] = diags;
		}
	}
	if (ec.tx) {
		out["changes"] = ec.tx->changes;
	}
	// Op warnings (e.g. MIXED_SPACE) surface as diagnostics; the op itself
	// already applied — the agent decides whether to act on the hint.
	if (!ec.warnings.is_empty()) {
		Array diags = out.get("diagnostics", Array());
		for (int w = 0; w < ec.warnings.size(); w++) {
			Dictionary wd = ec.warnings[w];
			wd["phase"] = "op";
			diags.push_back(wd);
		}
		out["diagnostics"] = diags;
	}
	return out;
}

} // namespace godot_cm
