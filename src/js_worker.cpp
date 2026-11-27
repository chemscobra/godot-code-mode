#include "js_worker.hpp"

#include <chrono>
#include <cmath>

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>

#include "api_index.hpp"
#include "bridge.hpp"
#include "value_codec.hpp"
#include "performance.hpp"

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
	d["checkpoints"] = Array();
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

Dictionary bounded_response(Dictionary out) {
	PerfScope cost(PerfMetric::FINAL_RESPONSE_NS);
	for (const char *field : {"diagnostics", "effects", "changes"}) {
		Array records = Array(out.get(field, Array())).duplicate();
		for (int i = 0; i < records.size(); i++) {
			if (records[i].get_type() != Variant::DICTIONARY) continue;
			Dictionary record = Dictionary(records[i]).duplicate(true);
			int64_t omitted = 0;
			for (const Variant &key : record.keys()) {
				if (record[key].get_type() != Variant::STRING) continue;
				String text = record[key];
				if (text.length() <= 512) continue;
				omitted += text.length() - 512;
				record[key] = text.substr(0, 512);
			}
			if (omitted) { record["omitted_characters"] = omitted; out["truncated"] = true; }
			records[i] = record;
		}
		out[field] = records;
	}
	auto fits = [&]() { return Performance::json(out).utf8().length() <= OUTPUT_LIMIT; };
	if (fits()) return out;
	out["truncated"] = true;
	if (out.get("result", Variant()).get_type() != Variant::NIL) {
		out["result"] = Variant();
		out["result_omitted"] = true;
	}
	for (const char *field : {"effects", "changes", "diagnostics"}) {
		if (fits()) break;
		Array records = out.get(field, Array());
		int minimum = String(field) == "diagnostics" && !records.is_empty() ? 1 : 0;
		int low = minimum, high = records.size();
		String omitted_field = String(field) + "_omitted";
		while (low < high) {
			int middle = (low + high + 1) / 2;
			out[field] = records.slice(0, middle);
			out[omitted_field] = records.size() - middle;
			if (fits()) low = middle; else high = middle - 1;
		}
		out[field] = records.slice(0, low);
		out[omitted_field] = records.size() - low;
	}
	return out;
}

// Variant -> JSValue for api.* return values (metadata surface only).
JSValue variant_to_js(JSContext *ctx, const Variant &v) {
	switch (v.get_type()) {
		case Variant::NIL:
			return JS_NULL;
		case Variant::BOOL:
			return JS_NewBool(ctx, (bool)v);
		case Variant::INT:
			return (int64_t)v < -ValueCodec::SAFE_INTEGER || (int64_t)v > ValueCodec::SAFE_INTEGER ? variant_to_js(ctx, ValueCodec().encode(v)) : JS_NewInt64(ctx, (int64_t)v);
		case Variant::FLOAT:
			return !std::isfinite((double)v) || ((double)v == 0 && std::signbit((double)v)) ? variant_to_js(ctx, ValueCodec().encode(v)) : JS_NewFloat64(ctx, (double)v);
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
				JSAtom key = JS_NewAtomLen(ctx, ks.get_data(), ks.length());
				JS_DefinePropertyValue(ctx, obj, key, variant_to_js(ctx, d[keys[i]]), JS_PROP_C_W_E);
				JS_FreeAtom(ctx, key);
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

const ApiCatalog *ctx_index(JSContext *ctx);

const char *identifier_string(JSContext *ctx, JSValueConst value);

JSValue js_api_types(JSContext *ctx, JSValueConst, int, JSValueConst *) { return variant_to_js(ctx, ValueCodec::types()); }

JSValue js_api_describe_type(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "api.describeType(name) requires a string");
	const char *text = identifier_string(ctx, argv[0]);
	String name = text ? String::utf8(text) : String();
	if (text) JS_FreeCString(ctx, text);
	Dictionary info = ValueCodec::describe_type(name);
	return info.is_empty() ? JS_UNDEFINED : variant_to_js(ctx, info);
}

JSValue js_api_classes(JSContext *ctx, JSValueConst, int, JSValueConst *) {
	const ApiCatalog *idx = ctx_index(ctx);
	if (!idx) {
		return JS_ThrowInternalError(ctx, "api index not loaded");
	}
	return variant_to_js(ctx, idx->classes());
}

JSValue js_api_describe(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	const ApiCatalog *idx = ctx_index(ctx);
	if (!idx || argc < 1 || !JS_IsString(argv[0])) {
		return JS_ThrowInternalError(ctx, "api.describe(name) needs an argument");
	}
	const char *n = identifier_string(ctx, argv[0]);
	Variant out = idx->describe(String::utf8(n ? n : ""));
	if (n) {
		JS_FreeCString(ctx, n);
	}
	return out.get_type() == Variant::NIL ? JS_UNDEFINED : variant_to_js(ctx, out);
}

JSValue js_api_find(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	const ApiCatalog *idx = ctx_index(ctx);
	if (!idx || argc < 1 || !JS_IsString(argv[0])) {
		return JS_ThrowInternalError(ctx, "api.find(query) needs an argument");
	}
	const char *n = identifier_string(ctx, argv[0]);
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
	Dictionary source;
};

struct KnownError {
	JSValue value;
	Dictionary diagnostic;
};

struct ExecCtx {
	Bridge *bridge = nullptr;
	std::shared_ptr<const ApiCatalog> catalog;
	ExecBudget *budget = nullptr;
	std::string mode = "read";
	std::shared_ptr<std::atomic<bool>> cancel;
	std::shared_ptr<TxState> tx;
	std::deque<InflightOp> inflight;
	bool native_pending = false;
	JSValue object_prototype = JS_UNDEFINED;
	JSClassID object_class = 0;
	JSValue error_constructor = JS_UNDEFINED;
	JSValue error_stack_getter = JS_UNDEFINED;
	JSValue prepare_getter = JS_UNDEFINED;
	JSValue prepare_setter = JS_UNDEFINED;
	JSValue stack_limit_getter = JS_UNDEFINED;
	godot::Dictionary encoding_error;
	std::vector<KnownError> known_errors;
	int source_lines = 1;
	godot::Array checkpoints;
	size_t checkpoint_bytes = 2;
	godot::Array warnings; // non-fatal op warnings folded into diagnostics
};

const ApiCatalog *ctx_index(JSContext *ctx) {
	ExecCtx *ec = exec_ctx(ctx);
	return ec ? ec->catalog.get() : nullptr;
}

Variant unwrap_tagged(const Variant &p);

// JSValue -> Variant. {__godotHandle:"hN"} -> {"$handle":N} for the bridge;
// {$t:"Vector2/3/Color",...} -> tagged engine types; objects via JSON round-trip.
struct OwnedJs {
	JSContext *ctx;
	JSValue value;
	~OwnedJs() { JS_FreeValue(ctx, value); }
};

struct OwnDescriptor {
	JSContext *ctx;
	JSPropertyDescriptor value{0, JS_UNDEFINED, JS_UNDEFINED, JS_UNDEFINED};
	~OwnDescriptor() { JS_FreeValue(ctx, value.value); JS_FreeValue(ctx, value.getter); JS_FreeValue(ctx, value.setter); }
};

struct DataBudget {
	std::vector<JSValueConst> ancestors;
	size_t bytes = 0;
	int nodes = 0;
	bool has_float = false;
};

String primitive_string(JSContext *ctx, JSValueConst value, String path = "value") {
	size_t length = 0;
	const char *text = JS_ToCStringLen(ctx, &length, value);
	if (!text) ValueCodec::fail("INVALID_VALUE_ENCODING", "unable to read primitive string");
	String result = String::utf8(text, (int)length);
	JS_FreeCString(ctx, text);
	if (JS_IsString(value)) {
		CharString encoded = result.utf8();
		OwnedJs round_trip{ctx, JS_NewStringLen(ctx, encoded.get_data(), encoded.length())};
		if (!JS_IsStrictEqual(ctx, value, round_trip.value)) ValueCodec::fail("INVALID_VALUE_ENCODING", "string cannot be represented by Godot without data loss", path);
	}
	return result;
}

Variant passive_data(JSContext *ctx, JSValueConst value, DataBudget &budget, String path, int depth = 0, bool allow_void = false) {
	PerfScope cost(PerfMetric::JS_DECODE_NS, depth == 0);
	if (depth > ValueCodec::MAX_DEPTH || ++budget.nodes > 100000 || (budget.bytes += 16) > HEAP_LIMIT) ValueCodec::fail("VALUE_DEPTH_LIMIT", "data depth, count or byte budget exceeded", path);
	if (JS_IsUndefined(value)) {
		if (allow_void) return Variant();
		ValueCodec::fail("INVALID_VALUE_ENCODING", "undefined is not transportable data", path);
	}
	if (JS_IsNull(value)) return Variant();
	if (JS_IsBool(value)) return (bool)JS_VALUE_GET_BOOL(value);
	if (JS_IsBigInt(value)) {
		String text = primitive_string(ctx, value, path);
		ValueCodec::integer_text(text, path);
		Dictionary tagged; tagged["$t"] = "int"; tagged["value"] = text;
		return tagged;
	}
	if (JS_IsNumber(value)) {
		if (JS_VALUE_GET_TAG(value) == JS_TAG_INT) return (int64_t)JS_VALUE_GET_INT(value);
		budget.has_float = true;
		return ValueCodec().encode(JS_VALUE_GET_FLOAT64(value), path);
	}
	if (JS_IsString(value)) {
		String text = primitive_string(ctx, value, path);
		budget.bytes += text.utf8().length();
		if (budget.bytes > HEAP_LIMIT) ValueCodec::fail("OUTPUT_LIMIT", "data byte budget exceeded", path);
		return text;
	}
	if (!JS_IsObject(value) || JS_IsProxy(value) || JS_IsFunction(ctx, value)) ValueCodec::fail("INVALID_VALUE_ENCODING", "only passive data is transportable", path);
	if (JS_IsPromise(value)) ValueCodec::fail("UNAWAITED_VALUE", "nested promise must be awaited", path);
	bool array = JS_IsArray(value);
	ExecCtx *ec = exec_ctx(ctx);
	if (!array) {
		if (!ec || JS_GetClassID(value) != ec->object_class) ValueCodec::fail("INVALID_VALUE_ENCODING", "unsupported object class", path);
		OwnedJs prototype{ctx, JS_GetPrototype(ctx, value)};
		if (!JS_IsNull(prototype.value) && !JS_IsStrictEqual(ctx, prototype.value, ec->object_prototype)) ValueCodec::fail("INVALID_VALUE_ENCODING", "expected plain data object", path);
	}
	for (JSValueConst ancestor : budget.ancestors) if (JS_IsStrictEqual(ctx, value, ancestor)) ValueCodec::fail("INVALID_VALUE_ENCODING", "cyclic data", path);
	budget.ancestors.push_back(value);
	struct Pop { std::vector<JSValueConst> &values; ~Pop() { values.pop_back(); } } pop{budget.ancestors};
	JSPropertyEnum *properties = nullptr;
	uint32_t count = 0;
	if (JS_GetOwnPropertyNames(ctx, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK | JS_GPN_SET_ENUM) < 0) ValueCodec::fail("INVALID_VALUE_ENCODING", "unable to inspect own data properties", path);
	struct FreeProperties { JSContext *ctx; JSPropertyEnum *data; uint32_t size; ~FreeProperties() { JS_FreePropertyEnum(ctx, data, size); } } free_properties{ctx, properties, count};
	if (count > 100001) ValueCodec::fail("VALUE_DEPTH_LIMIT", "data property count exceeded", path);
	Array items;
	Dictionary record;
	int64_t length = 0, indices = 0;
	if (array) {
		JSAtom atom = JS_NewAtom(ctx, "length");
		OwnDescriptor own{ctx};
		int found = JS_GetOwnProperty(ctx, &own.value, value, atom);
		JS_FreeAtom(ctx, atom);
		if (found != 1 || (own.value.flags & JS_PROP_TMASK) == JS_PROP_GETSET || !JS_IsNumber(own.value.value) || JS_ToInt64(ctx, &length, own.value.value) < 0 || length < 0 || length > 100000) ValueCodec::fail("INVALID_VALUE_ENCODING", "invalid or oversized array length", path);
		items.resize(length);
	}
	for (uint32_t i = 0; i < count; i++) {
		OwnedJs key{ctx, JS_AtomToValue(ctx, properties[i].atom)};
		if (JS_IsSymbol(key.value)) ValueCodec::fail("INVALID_VALUE_ENCODING", "symbol keys are not data", path);
		String name = primitive_string(ctx, key.value);
		budget.bytes += name.utf8().length();
		String child = path + String(".") + name;
		OwnDescriptor own{ctx};
		if (JS_GetOwnProperty(ctx, &own.value, value, properties[i].atom) != 1 || (own.value.flags & JS_PROP_TMASK) == JS_PROP_GETSET) ValueCodec::fail("INVALID_VALUE_ENCODING", "accessors are not passive data", child);
		if (array && name == "length") continue;
		if (!(own.value.flags & JS_PROP_ENUMERABLE)) ValueCodec::fail("INVALID_VALUE_ENCODING", "non-enumerable data cannot be transported implicitly", child);
		if (array) {
			int64_t index = name.to_int();
			if (index < 0 || index >= length || name != String::num_int64(index)) ValueCodec::fail("INVALID_VALUE_ENCODING", "arrays require indexed elements only", child);
			items[index] = passive_data(ctx, own.value.value, budget, path + String("[") + itos(index) + String("]"), depth + 1);
			indices++;
		} else record[name] = passive_data(ctx, own.value.value, budget, child, depth + 1);
	}
	if (array && indices != length) ValueCodec::fail("INVALID_VALUE_ENCODING", "sparse arrays are not transportable", path);
	return array ? Variant(items) : Variant(record);
}

Variant js_to_variant(JSContext *ctx, JSValueConst v) {
	// Objects/arrays: check handle marker first.
	DataBudget budget;
	return passive_data(ctx, v, budget, "value");
}

Variant parsed_json_numbers(Variant data) {
	if (data.get_type() == Variant::INT) return (double)(int64_t)data;
	if (data.get_type() == Variant::ARRAY) {
		Array items = data;
		for (int i = 0; i < items.size(); i++) items[i] = parsed_json_numbers(items[i]);
	} else if (data.get_type() == Variant::DICTIONARY) {
		Dictionary record = data;
		for (const Variant &key : record.keys()) record[key] = parsed_json_numbers(record[key]);
	}
	return data;
}

String passive_json(JSContext *ctx, JSValueConst value, Variant &data, bool &reusable) {
	try {
		DataBudget budget;
		data = passive_data(ctx, value, budget, "result", 0, true);
		String text = Performance::json(data);
		if (text.utf8().length() > OUTPUT_LIMIT) ValueCodec::fail("OUTPUT_LIMIT", "encoded result exceeds output budget", "result");
		reusable = !budget.has_float;
		Performance::add(reusable ? PerfMetric::WORKER_RESULT_REUSE : PerfMetric::WORKER_RESULT_PARSE);
		if (reusable) data = parsed_json_numbers(data);
		return text;
	} catch (const ValueFailure &failure) {
		exec_ctx(ctx)->encoding_error = failure.diagnostic;
		return "null";
	}
}

JSValue new_passive_error(JSContext *ctx) {
	ExecCtx *ec = exec_ctx(ctx);
	if (!ec || JS_IsUndefined(ec->stack_limit_getter)) return JS_NewObject(ctx);
	OwnedJs limit{ctx, JS_Call(ctx, ec->stack_limit_getter, ec->error_constructor, 0, nullptr)};
	if (!JS_IsNumber(limit.value)) return JS_NewObject(ctx);
	OwnedJs prepare{ctx, JS_Call(ctx, ec->prepare_getter, ec->error_constructor, 0, nullptr)};
	JSValue disabled = JS_UNDEFINED;
	OwnedJs disabling{ctx, JS_Call(ctx, ec->prepare_setter, ec->error_constructor, 1, &disabled)};
	JSValue error = JS_NewError(ctx);
	OwnedJs restoring{ctx, JS_Call(ctx, ec->prepare_setter, ec->error_constructor, 1, &prepare.value)};
	return error;
}

JSValue diagnostic_error(JSContext *ctx, const Dictionary &diagnostic) {
	Dictionary bounded = diagnostic.duplicate(true);
	int64_t omitted = 0;
	for (const Variant &key : bounded.keys()) {
		if (bounded[key].get_type() != Variant::STRING) continue;
		String text = bounded[key];
		if (text.length() > 512) { omitted += text.length() - 512; bounded[key] = text.substr(0, 512); }
	}
	if (omitted) bounded["omitted_characters"] = omitted;
	JSValue error = new_passive_error(ctx);
	for (const Variant &key : bounded.keys()) JS_SetPropertyStr(ctx, error, String(key).utf8().get_data(), variant_to_js(ctx, bounded[key]));
	ExecCtx *ec = exec_ctx(ctx);
	if (ec) ec->known_errors.push_back({JS_DupValue(ctx, error), bounded});
	return error;
}

String own_error_string(JSContext *ctx, JSValueConst error, const char *field) {
	if (!JS_IsObject(error) || JS_IsProxy(error)) return String();
	ExecCtx *ec = exec_ctx(ctx);
	if (String(field) == "stack" && JS_IsError(error) && ec && !JS_IsUndefined(ec->error_stack_getter)) {
		OwnedJs stack{ctx, JS_Call(ctx, ec->error_stack_getter, error, 0, nullptr)};
		if (!JS_IsString(stack.value)) return String();
		try { return primitive_string(ctx, stack.value); } catch (const ValueFailure &) { return String(); }
	}
	JSAtom key = JS_NewAtom(ctx, field);
	OwnDescriptor own{ctx};
	int found = JS_GetOwnProperty(ctx, &own.value, error, key);
	JS_FreeAtom(ctx, key);
	if (found != 1 || (own.value.flags & JS_PROP_TMASK) == JS_PROP_GETSET || !JS_IsString(own.value.value)) return String();
	try { return primitive_string(ctx, own.value.value); }
	catch (const ValueFailure &) { return String(); }
}

Dictionary source_location(JSContext *ctx, JSValueConst error) {
	Dictionary source;
	String normalized;
	ExecCtx *ec = exec_ctx(ctx);
	for (String frame : own_error_string(ctx, error, "stack").split("\n")) {
		int marker = frame.find("<execute>:");
		if (marker < 0) continue;
		int start = marker + 10, end = start;
		while (end < frame.length() && frame[end] >= '0' && frame[end] <= '9') end++;
		if (end == start) continue;
		int line = frame.substr(start, end - start).to_int() - 1;
		if (line < 1 || (ec && line > ec->source_lines)) continue;
		if (source.is_empty()) {
			source["line"] = line;
			int column_start = end + 1, column_end = column_start;
			if (end < frame.length() && frame[end] == ':') {
				while (column_end < frame.length() && frame[column_end] >= '0' && frame[column_end] <= '9') column_end++;
				if (column_end > column_start) source["column"] = frame.substr(column_start, column_end - column_start).to_int();
			}
		}
		if (!normalized.is_empty()) normalized += "\n";
		normalized += frame.substr(0, start) + itos(line) + frame.substr(end);
		if (normalized.length() >= 1024) break;
	}
	if (!source.is_empty()) source["stack"] = normalized.substr(0, 1024);
	return source;
}

Dictionary exception_result(JSContext *ctx, JSValueConst error, const String &fallback) {
	Dictionary diagnostic;
	ExecCtx *ec = exec_ctx(ctx);
	if (ec) for (const KnownError &known : ec->known_errors) {
		if (JS_IsStrictEqual(ctx, known.value, error)) { diagnostic = known.diagnostic.duplicate(true); break; }
	}
	if (diagnostic.is_empty()) {
		diagnostic["code"] = fallback;
		diagnostic["phase"] = "execute";
		String message;
		if (JS_IsString(error)) { try { message = primitive_string(ctx, error); } catch (const ValueFailure &) {} }
		else if (JS_IsError(error) && !JS_IsProxy(error)) message = own_error_string(ctx, error, "message");
		diagnostic["message"] = message.is_empty() ? String("JavaScript execution failed") : message.substr(0, 512);
	}
	if (!diagnostic.has("source")) {
		Dictionary source = source_location(ctx, error);
		if (!source.is_empty()) diagnostic["source"] = source;
	}
	Dictionary result = base_result(false);
	result["diagnostics"] = Array::make(diagnostic);
	return result;
}

template<auto Function>
JSValue guarded(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
	try { return Function(ctx, self, argc, argv); }
	catch (const ValueFailure &failure) {
		ExecCtx *ec = exec_ctx(ctx);
		if (ec && ec->budget) ec->budget->ops.fetch_add(1);
		return JS_Throw(ctx, diagnostic_error(ctx, failure.diagnostic));
	}
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
		if (t.begins_with("Packed") && d.get("values", Variant()).get_type() == Variant::ARRAY) {
			Array items = unwrap_tagged(d["values"]);
			for (const Variant &item : items) {
				Variant::Type type = item.get_type();
				bool numeric = type == Variant::INT || type == Variant::FLOAT;
				double number = numeric ? (double)item : 0;
				bool integer = numeric && std::isfinite(number) && std::floor(number) == number;
				if (t == "PackedByteArray" && (!integer || number < 0 || number > 255)) return p;
				if ((t == "PackedInt32Array" || t == "PackedInt64Array") && !integer) return p;
				if ((t == "PackedFloat32Array" || t == "PackedFloat64Array") && type != Variant::INT && type != Variant::FLOAT) return p;
				if (t == "PackedStringArray" && type != Variant::STRING) return p;
				if (t == "PackedVector2Array" && type != Variant::VECTOR2) return p;
				if (t == "PackedVector3Array" && type != Variant::VECTOR3) return p;
				if (t == "PackedColorArray" && type != Variant::COLOR) return p;
			}
			if (t == "PackedByteArray") return PackedByteArray(items);
			if (t == "PackedInt32Array") return PackedInt32Array(items);
			if (t == "PackedInt64Array") return PackedInt64Array(items);
			if (t == "PackedFloat32Array") return PackedFloat32Array(items);
			if (t == "PackedFloat64Array") return PackedFloat64Array(items);
			if (t == "PackedStringArray") return PackedStringArray(items);
			if (t == "PackedVector2Array") return PackedVector2Array(items);
			if (t == "PackedVector3Array") return PackedVector3Array(items);
			if (t == "PackedColorArray") return PackedColorArray(items);
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
			k == "connect" || k == "disconnect" || k == "value_release") {
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
	Dictionary diagnostic = r.error_context.duplicate(true);
	diagnostic["code"] = r.error_code;
	diagnostic["message"] = r.error_message.substr(0, 512);
	if (!diagnostic.has("phase")) diagnostic["phase"] = "bridge";
	return diagnostic_error(ctx, diagnostic);
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
	op.write = String(kind) == "call" ? false : arg_is_write(kind, member);
	op.write_mode = (ec->mode == "write" || ec->mode == "untracked");
	op.untracked = (ec->mode == "untracked");
	if (ec->budget) {
		ec->budget->ops.fetch_add(op_count);
	}
	if (op.write && !op.write_mode) {
		ValueCodec::fail("PERMISSION", "write operation requires write or untracked mode");
	}
	auto pb = ec->bridge->enqueue(op, ec->cancel, ec->tx);

	JSValue funcs[2];
	JSValue promise = JS_NewPromiseCapability(ctx, funcs);
	InflightOp io;
	io.pb = pb;
	io.resolve = funcs[0];
	io.reject = funcs[1];
	OwnedJs site{ctx, new_passive_error(ctx)};
	io.source = source_location(ctx, site.value);
	ec->inflight.push_back(io);
	return promise;
}

uint64_t handle_id_of(JSContext *ctx, JSValueConst v) {
	Variant hv = js_to_variant(ctx, v);
	if (hv.get_type() == Variant::DICTIONARY && ((Dictionary)hv).has("__godotValue")) ValueCodec::fail("REFERENCE_KIND_MISMATCH", "value references are not object targets", "target");
	if (hv.get_type() == Variant::DICTIONARY && ((Dictionary)hv).has("__godotHandle")) {
		ExecCtx *ec = exec_ctx(ctx);
		return ec && ec->bridge ? ec->bridge->handle_id(((Dictionary)hv)["__godotHandle"]) : 0;
	}
	return 0;
}

const char *identifier_string(JSContext *ctx, JSValueConst value) {
	if (!JS_IsString(value)) ValueCodec::fail("INVALID_VALUE_ENCODING", "member name must be a string", "member");
	primitive_string(ctx, value, "member");
	return JS_ToCString(ctx, value);
}

JSValue js_checkpoint(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc != 2) ValueCodec::fail("BAD_ARGS", "checkpoint(name,value) requires two arguments", "checkpoint");
	ExecCtx *ec = exec_ctx(ctx);
	if (!JS_IsString(argv[0]) || ec->checkpoints.size() >= 32) ValueCodec::fail("CHECKPOINT_LIMIT", "checkpoint name or entry limit exceeded", "checkpoint.name");
	String name = primitive_string(ctx, argv[0], "checkpoint.name");
	int bytes = name.utf8().length();
	if (bytes < 1 || bytes > 128) ValueCodec::fail("CHECKPOINT_LIMIT", "checkpoint names require 1..128 UTF-8 bytes", "checkpoint.name");
	DataBudget budget;
	Variant data;
	try { data = passive_data(ctx, argv[1], budget, "checkpoint.value"); }
	catch (ValueFailure &failure) {
		if (String(failure.diagnostic.get("code", "")) == "VALUE_DEPTH_LIMIT") failure.diagnostic["code"] = "CHECKPOINT_LIMIT";
		failure.diagnostic["phase"] = "checkpoint";
		throw;
	}
	Dictionary entry; entry["name"] = name; entry["value"] = data;
	size_t encoded_bytes = Performance::json(entry).utf8().length() + (ec->checkpoints.is_empty() ? 0 : 1);
	if (encoded_bytes > (64u << 10) - ec->checkpoint_bytes) ValueCodec::fail("CHECKPOINT_LIMIT", "checkpoint byte budget exceeded", "checkpoint.value");
	ec->checkpoints.push_back(entry);
	ec->checkpoint_bytes += encoded_bytes;
	return JS_UNDEFINED;
}

JSValue value_submit(JSContext *ctx, int argc, JSValueConst *argv, const char *kind, int maximum) {
	if (argc < 1 || argc > maximum) ValueCodec::fail("BAD_ARGS", "invalid value operation arguments");
	Array args; for (int i = 0; i < argc; i++) args.push_back(js_to_variant(ctx, argv[i]));
	return submit_op(ctx, kind, 0, "", args);
}

JSValue js_value_inspect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return value_submit(ctx, argc, argv, "value_inspect", 1); }
JSValue js_value_read(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return value_submit(ctx, argc, argv, "value_read", 2); }
JSValue js_value_release(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) { return value_submit(ctx, argc, argv, "value_release", 1); }

JSValue js_godot_create(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsString(argv[0])) {
		return JS_ThrowInternalError(ctx, "godot.create(className)");
	}
	const char *s = identifier_string(ctx, argv[0]);
	String cls = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	for (int i = 1; i < argc; i++) args.push_back(js_to_variant(ctx, argv[i]));
	return submit_op(ctx, "create", 0, "", args, cls.utf8().get_data());
}

JSValue js_godot_singleton(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "godot.getSingleton(className)");
	const char *name = identifier_string(ctx, argv[0]);
	JSValue result = submit_op(ctx, "singleton", 0, "", Array(), name ? name : "");
	if (name) JS_FreeCString(ctx, name);
	return result;
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
	const char *s = identifier_string(ctx, argv[0]);
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
	Array decoded = js_to_variant(ctx, argv[0]);
	int64_t len = decoded.size();
	if (len > OP_LIMIT) {
		return JS_ThrowInternalError(ctx, "godot.batch: element count exceeds op budget");
	}
	Array elements;
	for (int64_t i = 0; i < len; i++) {
		Variant dv = decoded[i];
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
		if (tv.get_type() == Variant::DICTIONARY && ((Dictionary)tv).has("__godotHandle")) {
			target = exec_ctx(ctx)->bridge->handle_id(((Dictionary)tv)["__godotHandle"]);
		}
		out["target"] = target;
		Array args = d.get("args", Array());
		if (kind == "set" && args.is_empty() && d.has("value")) {
			args.push_back(d["value"]);
		}
		out["args"] = args;
		out["write"] = kind == "call" ? false : arg_is_write(kind.utf8().get_data(), member.utf8().get_data());
		elements.push_back(out);
	}
	return submit_op(ctx, "batch", 0, "", Array(), "", elements, len);
}

JSValue js_godot_get(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 2) {
		return JS_ThrowInternalError(ctx, "godot.get(handle, prop)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = identifier_string(ctx, argv[1]);
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
	const char *s = identifier_string(ctx, argv[1]);
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
	const char *s = identifier_string(ctx, argv[1]);
	String method = s ? String::utf8(s) : String();
	if (s) {
		JS_FreeCString(ctx, s);
	}
	Array args;
	for (int i = 2; i < argc; i++) {
		args.push_back(js_to_variant(ctx, argv[i]));
	}
	String class_name;
	if (JS_IsString(argv[0])) {
		const char *name = identifier_string(ctx, argv[0]);
		class_name = name ? String::utf8(name) : String();
		if (name) JS_FreeCString(ctx, name);
	}
	return submit_op(ctx, "call", hid, method.utf8().get_data(), args, class_name.utf8().get_data());
}

// godot.connect(emitter, signal, to, method, flags?, binds?)
JSValue js_godot_connect(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
	if (argc < 4) {
		return JS_ThrowInternalError(ctx, "godot.connect(handle, signal, toHandle, method)");
	}
	uint64_t hid = handle_id_of(ctx, argv[0]);
	const char *s = identifier_string(ctx, argv[1]);
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
	const char *s = identifier_string(ctx, argv[1]);
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
			if (!it->source.is_empty()) r.error_context["source"] = it->source;
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
		budget_ms = req.mode != "read" ? DEFAULT_WRITE_TIMEOUT_MS : DEFAULT_TIMEOUT_MS;
	}
	std::unique_lock<std::mutex> lock(mutex);
	bool finished = job->done->wait_for(lock, std::chrono::milliseconds(budget_ms + 10000),
			[&] { return !job->result->is_empty() || quit.load(); });
	if (!finished) {
		if (req.cancel) req.cancel->store(true);
		Dictionary result = error_result("WORKER_TIMEOUT", "native execution did not settle before the worker deadline; outcome unknown, inspect state before retrying", "execute");
		result["mode"] = req.mode.c_str();
		result["outcome"] = "unknown";
		result["rollback_supported"] = req.mode == "write";
		return result;
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
		{
			std::lock_guard<std::mutex> lock(mutex);
			*job->result = res;
		}
		job->done->notify_all();
	}
}

Dictionary JsWorker::execute_one(const ExecRequest &req) {
	PerfReport profile(req.search_only ? "search" : "execute");
	PerfScope request_cost(PerfMetric::REQUEST_NS);
	PerfScope context_cost(PerfMetric::CONTEXT_NS);
	if (req.mode != "read" && req.mode != "write" && req.mode != "untracked") return error_result("INVALID_MODE", "mode must be read, write or untracked", "validate");
	ExecBudget budget;
	int64_t timeout_ms = req.timeout_ms;
	if (timeout_ms <= 0) {
		timeout_ms = req.mode != "read" ? DEFAULT_WRITE_TIMEOUT_MS : DEFAULT_TIMEOUT_MS;
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
	ec.budget = &budget;
	ec.mode = req.mode;
	ec.cancel = req.cancel ? req.cancel : std::make_shared<std::atomic<bool>>(false);
	ec.tx = req.tx ? req.tx : std::make_shared<TxState>();
	ec.tx->untracked = req.mode == "untracked";
	if (api_index) ec.catalog = api_index->snapshot();
	JS_SetContextOpaque(ctx, &ec);

	JSValue global = JS_GetGlobalObject(ctx);
	JSValue prototype_probe = JS_NewObject(ctx);
	ec.object_prototype = JS_GetPrototype(ctx, prototype_probe);
	ec.object_class = JS_GetClassID(prototype_probe);
	JS_FreeValue(ctx, prototype_probe);
	ec.error_constructor = JS_GetPropertyStr(ctx, global, "Error");
	{
		OwnedJs prototype{ctx, JS_GetPropertyStr(ctx, ec.error_constructor, "prototype")};
		auto accessor = [&](JSValueConst object, const char *name, bool setter = false) {
			JSAtom key = JS_NewAtom(ctx, name);
			OwnDescriptor own{ctx};
			int found = JS_GetOwnProperty(ctx, &own.value, object, key);
			JS_FreeAtom(ctx, key);
			return found == 1 ? JS_DupValue(ctx, setter ? own.value.setter : own.value.getter) : JS_UNDEFINED;
		};
		ec.error_stack_getter = accessor(prototype.value, "stack");
		ec.prepare_getter = accessor(ec.error_constructor, "prepareStackTrace");
		ec.prepare_setter = accessor(ec.error_constructor, "prepareStackTrace", true);
		ec.stack_limit_getter = accessor(ec.error_constructor, "stackTraceLimit");
		OwnedJs limit_setter{ctx, accessor(ec.error_constructor, "stackTraceLimit", true)};
		JSValue limit = JS_NewInt32(ctx, 32);
		OwnedJs configured{ctx, JS_Call(ctx, limit_setter.value, ec.error_constructor, 1, &limit)};
		JS_DefinePropertyValueStr(ctx, ec.error_constructor, "prepareStackTrace", JS_UNDEFINED, 0);
		JS_DefinePropertyValueStr(ctx, ec.error_constructor, "stackTraceLimit", JS_NewInt32(ctx, 32), 0);
	}
	for (char character : req.code) if (character == '\n') ec.source_lines++;

	// api.* — immutable authorized metadata surface (search tool).
	if (api_index) {
		JSValue api_obj = JS_NewObject(ctx);
	JS_SetPropertyStr(ctx, api_obj, "types", JS_NewCFunction(ctx, guarded<js_api_types>, "types", 0));
	JS_SetPropertyStr(ctx, api_obj, "describeType", JS_NewCFunction(ctx, guarded<js_api_describe_type>, "describeType", 1));
		JS_SetPropertyStr(ctx, api_obj, "classes", JS_NewCFunction(ctx, js_api_classes, "classes", 0));
		JS_SetPropertyStr(ctx, api_obj, "describe", JS_NewCFunction(ctx, guarded<js_api_describe>, "describe", 1));
		JS_SetPropertyStr(ctx, api_obj, "find", JS_NewCFunction(ctx, guarded<js_api_find>, "find", 1));
		JS_SetPropertyStr(ctx, global, "api", api_obj);
	}

	if (!req.search_only) {
		JS_SetPropertyStr(ctx, global, "checkpoint", JS_NewCFunction(ctx, guarded<js_checkpoint>, "checkpoint", 2));
		JSValue godot_obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, godot_obj, "mode", JS_NewString(ctx, req.mode.c_str()));
		JSValue values = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, values, "inspect", JS_NewCFunction(ctx, guarded<js_value_inspect>, "inspect", 1));
		JS_SetPropertyStr(ctx, values, "read", JS_NewCFunction(ctx, guarded<js_value_read>, "read", 2));
		JS_SetPropertyStr(ctx, values, "release", JS_NewCFunction(ctx, guarded<js_value_release>, "release", 1));
		JS_SetPropertyStr(ctx, godot_obj, "values", values);
		JS_SetPropertyStr(ctx, godot_obj, "create", JS_NewCFunction(ctx, guarded<js_godot_create>, "create", 1));
		JS_SetPropertyStr(ctx, godot_obj, "getSingleton", JS_NewCFunction(ctx, guarded<js_godot_singleton>, "getSingleton", 1));
		JS_SetPropertyStr(ctx, godot_obj, "get", JS_NewCFunction(ctx, guarded<js_godot_get>, "get", 2));
		JS_SetPropertyStr(ctx, godot_obj, "set", JS_NewCFunction(ctx, guarded<js_godot_set>, "set", 3));
		JS_SetPropertyStr(ctx, godot_obj, "call", JS_NewCFunction(ctx, guarded<js_godot_call>, "call", 2));
		JS_SetPropertyStr(ctx, godot_obj, "connect",
				JS_NewCFunction(ctx, guarded<js_godot_connect>, "connect", 4));
		JS_SetPropertyStr(ctx, godot_obj, "disconnect",
				JS_NewCFunction(ctx, guarded<js_godot_disconnect>, "disconnect", 4));
		JS_SetPropertyStr(ctx, godot_obj, "saveScene",
				JS_NewCFunction(ctx, guarded<js_godot_save_scene>, "saveScene", 0));
		JS_SetPropertyStr(ctx, godot_obj, "unlockWrites",
				JS_NewCFunction(ctx, guarded<js_godot_unlock_writes>, "unlockWrites", 0));
		JS_SetPropertyStr(ctx, godot_obj, "requestTier",
				JS_NewCFunction(ctx, guarded<js_godot_request_tier>, "requestTier", 1));
		JS_SetPropertyStr(ctx, godot_obj, "unlockedTiers",
				JS_NewCFunction(ctx, guarded<js_godot_unlocked_tiers>, "unlockedTiers", 0));
		JS_SetPropertyStr(ctx, godot_obj, "batch",
				JS_NewCFunction(ctx, guarded<js_godot_batch>, "batch", 1));
		JSValue editor_obj = JS_NewObject(ctx);
		JS_SetPropertyStr(ctx, editor_obj, "getEditedScene",
				JS_NewCFunction(ctx, js_godot_editor_scene, "getEditedScene", 0));
		JS_SetPropertyStr(ctx, godot_obj, "editor", editor_obj);
		JS_SetPropertyStr(ctx, global, "godot", godot_obj);
	}

	context_cost.stop();
	// User code becomes the body of an async function; awaiting = async by design.
	std::string wrapped = "(async () => {\n" + req.code + "\n})()";
	JSValue val = JS_Eval(ctx, wrapped.c_str(), wrapped.size(), "<execute>", JS_EVAL_TYPE_GLOBAL);

	Dictionary out;
	if (JS_IsException(val)) {
		JSValue exc = JS_GetException(ctx);
		out = exception_result(ctx, exc, budget.tripped.load() ? "INTERRUPTED" : "JS_ERROR");
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
			ec.native_pending = true;
			ec.cancel->store(true);
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
		if (budget.tripped.load()) {
			out = error_result("INTERRUPTED", "cooperative execution budget exceeded", "execute");
		} else if (ec.cancel && ec.cancel->load()) {
			out = error_result("CANCELLED", "execution cancelled", "execute");
		} else if (st == JS_PROMISE_FULFILLED) {
			JSValue r = JS_PromiseResult(ctx, val);
			Variant data;
			bool reusable = false;
			String text = passive_json(ctx, r, data, reusable);
			out = base_result(true);
			if (text.length() > (int)OUTPUT_LIMIT) {
				// Spec: bounded prefix + truncated flag — a truncated blob
				// won't re-parse, so report it as a string.
				out["result"] = text.substr(0, OUTPUT_LIMIT);
				out["truncated"] = true;
			} else if (reusable) {
				out["result"] = data;
			} else {
				Ref<JSON> j;
				j.instantiate();
				out["result"] = (text.is_empty() || j->parse(text) != OK) ? Variant() : j->get_data();
			}
			JS_FreeValue(ctx, r);
		} else if (st == JS_PROMISE_REJECTED) {
			JSValue r = JS_PromiseResult(ctx, val);
			out = exception_result(ctx, r, "JS_ERROR");
			JS_FreeValue(ctx, r);
		} else {
			out = error_result(budget.tripped.load() ? "INTERRUPTED" : "UNSETTLED",
					budget.tripped.load() ? "execution interrupted by budget"
										  : "promise never settled (no ops in flight)",
					"execute");
		}
	} else {
		// Non-promise value: user code didn't await anything; still a result.
		Variant data;
		bool reusable = false;
		String text = passive_json(ctx, val, data, reusable);
		out = base_result(true);
		if (text.length() > (int)OUTPUT_LIMIT) {
			out["result"] = text.substr(0, OUTPUT_LIMIT);
			out["truncated"] = true;
		} else if (reusable) {
			out["result"] = data;
		} else {
			Ref<JSON> j;
			j.instantiate();
			out["result"] = (text.is_empty() || j->parse(text) != OK) ? Variant() : j->get_data();
		}
	}
	JS_FreeValue(ctx, val);
	JS_FreeValue(ctx, global);
	if (!ec.encoding_error.is_empty()) {
		out = base_result(false);
		out["diagnostics"] = Array::make(ec.encoding_error);
		if (String(ec.encoding_error.get("code", "")) == "OUTPUT_LIMIT") { out["truncated"] = true; out["result_omitted"] = true; }
	}

	bool pending_native_ops = ec.native_pending || !ec.inflight.empty();
	if (pending_native_ops) {
		ec.cancel->store(true);
		if (out.get("ok", false)) out = error_result("UNAWAITED_OPS", "await all bridge operations before returning", "execute");
	}
	free_inflight(ctx, &ec);

	// Drain pending jobs before freeing: the interrupt handler stays tripped,
	// so queued jobs reject immediately instead of leaking objects into
	// JS_FreeRuntime's gc list.
	{
		JSContext *pctx = nullptr;
		while (JS_ExecutePendingJob(rt, &pctx)) {
		}
	}
	for (const KnownError &known : ec.known_errors) JS_FreeValue(ctx, known.value);
	JS_FreeValue(ctx, ec.error_constructor);
	JS_FreeValue(ctx, ec.error_stack_getter);
	JS_FreeValue(ctx, ec.prepare_getter);
	JS_FreeValue(ctx, ec.prepare_setter);
	JS_FreeValue(ctx, ec.stack_limit_getter);
	JS_FreeValue(ctx, ec.object_prototype);
	JS_FreeContext(ctx);
	JS_FreeRuntime(rt);
	if (pending_native_ops && bridge) wait_bridge_op(bridge, "barrier", &ec);

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
		out["effects"] = ec.tx->effects;
		out["partial"] = req.mode == "untracked" && !(bool)out.get("ok", false) && !ec.tx->effects.is_empty();
	}
	out["mode"] = req.mode.c_str();
	out["rollback_supported"] = req.mode == "write";
	out["checkpoints"] = ec.checkpoints;
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
	return bounded_response(out);
}

} // namespace godot_cm
