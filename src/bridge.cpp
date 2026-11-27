#include "bridge.hpp"
#include <godot_cpp/classes/json.hpp>

#include "api_index.hpp"
#include "value_codec.hpp"
#include "performance.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace godot_cm {

void BridgePump::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pump"), &BridgePump::pump);
	ClassDB::bind_method(D_METHOD("invalidate_catalog"), &BridgePump::invalidate_catalog);
	ClassDB::bind_method(D_METHOD("invalidate_scripts"), &BridgePump::invalidate_scripts);
}

void BridgePump::invalidate_catalog() {
	if (bridge && bridge->index) bridge->index->invalidate();
}

void BridgePump::invalidate_scripts() {
	if (bridge && bridge->index) bridge->index->invalidate(false);
}

void BridgePump::pump() {
	PerfScope cost(PerfMetric::PUMP_NS);
	if (bridge) {
		bridge->drain();
		uint64_t now = Time::get_singleton()->get_ticks_msec();
		if (bridge->index && now - last_catalog_refresh >= 100) {
			PerfScope maintenance(PerfMetric::MAINTENANCE_NS);
			PerfMaintenance source;
			bridge->index->refresh(true);
			last_catalog_refresh = now;
		}
		cost.stop();
		if (Performance::enabled()) {
			static uint64_t last_report = 0;
			if (now - last_report >= 1000) {
				Performance::report("cumulative", Performance::Snapshot{});
				last_report = now;
			}
		}
	}
}

Bridge::Bridge() {
	static std::atomic<uint64_t> sequence{0};
	generation = String::num_uint64((uint64_t)std::chrono::steady_clock::now().time_since_epoch().count()) + String("_") + String::num_uint64(++sequence);
}

String Bridge::reference_name(uint64_t id, const char *kind) const {
	return String(kind) + generation + String(".") + String::num_uint64(id);
}

uint64_t Bridge::reference_id(const String &name, const char *kind) const {
	String prefix = String(kind) + generation + String(".");
	if (!name.begins_with(prefix)) return 0;
	String suffix = name.substr(prefix.length());
	if (!suffix.is_valid_int()) return 0;
	int64_t id = suffix.to_int();
	return id > 0 && suffix == String::num_int64(id) ? (uint64_t)id : 0;
}

uint64_t Bridge::handle_id(const String &name) const {
	return reference_id(name, "h");
}

uint64_t Bridge::mint_handle(Object *obj) {
	uint64_t oid = obj->get_instance_id();
	auto existing = object_handles.find(oid);
	if (existing != object_handles.end()) return existing->second;
	uint64_t id = next_handle++;
	handles[id] = obj->get_instance_id();
	object_handles[oid] = id;
	if (index) {
		index->observe_script(obj);
		if (!index->script_class(obj).is_empty()) index->refresh();
	}
	return id;
}

uint64_t Bridge::make_handle_for(Object *obj) {
	std::lock_guard<std::mutex> lock(mutex);
	return mint_handle(obj);
}

std::shared_ptr<PendingBridge> Bridge::enqueue(const BridgeOp &op_in,
		std::shared_ptr<std::atomic<bool>> cancel,
		std::shared_ptr<TxState> tx) {
	auto pb = std::make_shared<PendingBridge>();
	pb->queued_ns = Performance::enabled() ? Performance::now() : 0;
	pb->op = op_in;
	pb->op.id = next_op++;
	pb->cancel = cancel;
	pb->tx = tx;
	std::lock_guard<std::mutex> lock(mutex);
	in_queue.push_back(pb);
	return pb;
}

static BridgeResult op_error(const String &code, const String &msg) {
	BridgeResult r;
	r.ok = false;
	r.error_code = code;
	r.error_message = msg;
	return r;
}

static BridgeResult op_value(const Variant &v) {
	BridgeResult r;
	r.ok = true;
	r.value = v;
	return r;
}

static BridgeResult invoke_checked(Object *target, const StringName &method, const Array &args) {
	PerfScope cost(PerfMetric::NATIVE_NS);
	std::vector<const Variant *> pointers;
	for (int i = 0; i < args.size(); i++) pointers.push_back(&args[i]);
	Variant receiver(target), value;
	GDExtensionCallError error{};
	receiver.callp(method, pointers.data(), (int)pointers.size(), value, error);
	BridgeResult result = error.error == GDEXTENSION_CALL_OK ? op_value(value)
			: op_error("CALL_FAILED", String(method) + ": call error " + itos(error.error) + ", argument " + itos(error.argument));
	result.dispatched = true;
	return result;
}

static BridgeResult begin_external(const std::shared_ptr<TxState> &tx, const String &name) {
	if (!tx || tx->wrote || tx->external) {
		return op_error("MIXED_EFFECTS", "external actions require a separate execute call with no other writes");
	}
	tx->external = true;
	Dictionary effect;
	effect["name"] = name;
	effect["rollbackable"] = false;
	effect["status"] = "dispatched";
	tx->effects.push_back(effect);
	return op_value(Variant());
}

// Convert a Variant result into wire form: objects become handle dicts,
// arrays and dictionaries are converted recursively.
Variant Bridge::to_wire(const Variant &v) {
	PerfScope cost(PerfMetric::ENCODE_NS);
	uint64_t checkpoint = next_value;
	try { return make_codec().encode(v); }
	catch (...) { rollback_values(checkpoint); throw; }
}

void Bridge::rollback_values(uint64_t checkpoint) {
	if (checkpoint == next_value) return;
	for (auto entry = values.begin(); entry != values.end();) {
		if (entry->first < checkpoint) { ++entry; continue; }
		retained_value_bytes -= value_sizes[entry->first];
		value_sizes.erase(entry->first);
		entry = values.erase(entry);
		Performance::add(PerfMetric::VALUE_RELEASES);
	}
}

Variant Bridge::retain_value(const Variant &value) {
	int64_t bytes = ValueCodec::estimate(value);
	if (values.size() >= ValueCodec::MAX_VALUE_REFERENCES || bytes > ValueCodec::MAX_VALUE_BYTES - retained_value_bytes) ValueCodec::fail("VALUE_REFERENCE_LIMIT", "retained value quota exceeded");
	uint64_t id = next_value++;
	values[id] = value;
	value_sizes[id] = bytes;
	retained_value_bytes += bytes;
	Performance::add(PerfMetric::VALUE_RETAINS);
	Dictionary result;
	result["__godotValue"] = reference_name(id, "v");
	result["type"] = Variant::get_type_name(value.get_type());
	return result;
}

ValueCodec Bridge::make_codec() {
	ValueCodec codec;
	codec.retain = [this](const Variant &value) { return retain_value(value); };
	codec.retention_checkpoint = [this]() { return next_value; };
	codec.rollback_retention = [this](uint64_t checkpoint) { rollback_values(checkpoint); };
	codec.collection_type = [this](int builtin, const StringName &class_name, const Variant &script) {
		if (script.get_type() == Variant::OBJECT && script.get_validated_object()) {
			String name = index ? index->script_class(script.get_validated_object()) : String();
			if (name.is_empty() || index->describe(name).get_type() != Variant::DICTIONARY) return Dictionary();
			return ValueCodec::descriptor(name);
		}
		return ValueCodec::descriptor(builtin == Variant::NIL ? String("Variant") : builtin == Variant::OBJECT ? class_name.is_empty() ? String("Object") : String(class_name) : Variant::get_type_name((Variant::Type)builtin));
	};
	codec.resolve = [this](const Dictionary &wire, const String &path) -> Variant {
		bool object = wire.has("__godotHandle"), value = wire.has("__godotValue");
		if (object == value || wire.has("$handle")) ValueCodec::fail("INVALID_VALUE_ENCODING", "ambiguous reference envelope", path);
		Variant name = wire[object ? "__godotHandle" : "__godotValue"];
		if (name.get_type() != Variant::STRING) ValueCodec::fail("INVALID_VALUE_ENCODING", "reference identifier must be a string", path);
		uint64_t id = reference_id(name, object ? "h" : "v");
		if (object) {
			auto entry = handles.find(id);
			Object *target = entry == handles.end() ? nullptr : ObjectDB::get_instance(entry->second);
			if (!target) ValueCodec::fail("INVALID_HANDLE", "object reference is stale or unknown", path);
			return target;
		}
		auto entry = values.find(id);
		if (entry == values.end()) ValueCodec::fail("INVALID_VALUE_REFERENCE", "value reference is stale or unknown", path);
		return entry->second;
	};
	codec.object_wire = [this](const Variant &value) -> Variant {
		Object *obj = value.get_validated_object();
		if (!obj) return Variant();
		if (object_handles.find((uint64_t)obj->get_instance_id()) == object_handles.end() && Object::cast_to<RefCounted>(obj)) retained_rc.push_back(Ref<RefCounted>(value));
		Dictionary result;
		result["__godotHandle"] = reference_name(mint_handle(obj), "h");
		result["class_name"] = obj->get_class();
		if (index) {
			String script = index->script_class(obj);
			if (!script.is_empty()) result["script_class"] = script;
		}
		return result;
	};
	codec.object_matches = [this](Object *object, const String &cls) {
		// class_name may carry a hint list ("BaseMaterial3D,ShaderMaterial").
		for (const String &want : cls.split(",", false)) {
			if (object->is_class(want)) return true;
			String current = index ? index->script_class(object) : String();
			for (int i = 0; !current.is_empty() && i < 64; i++) {
				if (current == want) return true;
				Variant entry = index->describe(current);
				if (entry.get_type() != Variant::DICTIONARY) break;
				current = ((Dictionary)entry).get("inherits", "");
			}
		}
		return false;
	};
	codec.storage_type = [this](const Dictionary &descriptor) {
		Dictionary result = descriptor.duplicate();
		String cls = result.get("class_name", "");
		if (cls.is_empty() || ClassDBSingleton::get_singleton()->class_exists(cls)) return result;
		Variant entry = index ? index->describe(cls) : Variant();
		String path = entry.get_type() == Variant::DICTIONARY ? String(((Dictionary)entry).get("path", "")) : String();
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (path.is_empty() || !loader->has_cached(path)) ValueCodec::fail("TYPE_METADATA_UNAVAILABLE", "typed script collection requires already cached metadata", "type");
		Ref<Script> script(loader->get_cached_ref(path));
		if (script.is_null()) ValueCodec::fail("TYPE_METADATA_UNAVAILABLE", "cached collection type is not a script", "type");
		result["class_name"] = script->get_instance_base_type();
		result["script"] = script;
		return result;
	};
	return codec;
}

BridgeResult Bridge::value_operation(const BridgeOp &op) {
	if (op.args.is_empty() || op.args[0].get_type() != Variant::DICTIONARY) ValueCodec::fail("INVALID_VALUE_REFERENCE", "expected a value reference", "reference");
	Dictionary reference = op.args[0];
	if (reference.has("__godotHandle")) ValueCodec::fail("REFERENCE_KIND_MISMATCH", "expected a value reference, not an object handle", "reference");
	if (!reference.has("__godotValue")) ValueCodec::fail("INVALID_VALUE_REFERENCE", "expected a value reference", "reference");
	ValueCodec codec = make_codec();
	Variant value = codec.resolve(reference, "reference");
	uint64_t id = reference_id(reference["__godotValue"], "v");
	String kind = op.kind.c_str();
	if (kind == "value_release") {
		if (!op.untracked) return op_error("UNTRACKED_REQUIRED", "value release requires authorized untracked mode");
		retained_value_bytes -= value_sizes[id];
		value_sizes.erase(id);
		values.erase(id);
		Performance::add(PerfMetric::VALUE_RELEASES);
		Dictionary result; result["released"] = true;
		return op_value(result);
	}
	int type = value.get_type();
	bool dictionary = type == Variant::DICTIONARY;
	bool collection = dictionary || type == Variant::ARRAY || type >= Variant::PACKED_BYTE_ARRAY;
	bool text = type == Variant::STRING;
	std::vector<ValueField> fields = ValueCodec::fields(type);
	int64_t size = collection ? (int64_t)value.call("size") : text ? String(value).length() : (int64_t)fields.size();
	Dictionary result;
	result["type"] = Variant::get_type_name(value.get_type());
	result["variant_type"] = type;
	if (kind == "value_inspect") {
		result["size"] = size;
		result["readable"] = collection || text || !fields.empty();
		Array names; for (const ValueField &field : fields) names.push_back(field.name);
		result["fields"] = names;
		result["accounted_bytes"] = value_sizes[id];
		result["retained_references"] = (int64_t)values.size();
		result["retained_accounted_bytes"] = retained_value_bytes;
		result["reference_limit"] = ValueCodec::MAX_VALUE_REFERENCES;
		result["accounted_byte_limit"] = ValueCodec::MAX_VALUE_BYTES;
		return op_value(result);
	}
	if (!collection && !text && fields.empty()) return op_error("UNSUPPORTED_VALUE_OPERATION", "value supports passive metadata only");
	PerfScope page_cost(PerfMetric::PAGE_NS);
	Dictionary options;
	if (op.args.size() > 1) {
		if (op.args[1].get_type() != Variant::DICTIONARY) ValueCodec::fail("INVALID_VALUE_RANGE", "expected range options", "options");
		options = op.args[1];
	}
	for (const Variant &key : options.keys()) if (key != Variant("offset") && key != Variant("limit")) ValueCodec::fail("INVALID_VALUE_RANGE", "unknown range option", "options");
	int64_t offset = codec.decode(options.get("offset", 0), ValueCodec::descriptor("int"), "options.offset");
	int64_t limit = codec.decode(options.get("limit", 64), ValueCodec::descriptor("int"), "options.limit");
	if (offset < 0 || offset > size || limit < 1 || limit > ValueCodec::INLINE_ELEMENTS) ValueCodec::fail("INVALID_VALUE_RANGE", "offset or limit out of range", "options");
	result["offset"] = offset;
	result["total"] = size;
	Array items;
	Dictionary named;
	String page_key = dictionary ? "entries" : fields.empty() ? "items" : "fields";
	result[page_key] = fields.empty() ? Variant(items) : Variant(named);
	int64_t prefix_bytes = Performance::json(result).utf8().length();
	Variant cursor;
	bool valid = false, more = dictionary ? value.iter_init(cursor, valid) : false;
	if (dictionary) {
		PerfScope skip_cost(PerfMetric::DICTIONARY_SKIP_NS);
		for (int64_t i = 0; i < offset && more; i++) more = value.iter_next(cursor, valid);
	}
	int64_t count = 0;
	for (int64_t i = offset; i < size && count < limit; i++) {
		uint64_t checkpoint = next_value;
		Variant item, encoded;
		String field_name;
		if (dictionary) {
			if (!more || !valid) ValueCodec::fail("INVALID_VALUE_REFERENCE", "dictionary changed during paging", "reference");
			Variant key = value.iter_get(cursor, valid);
			item = Dictionary(value)[key];
			Dictionary entry; entry["key"] = to_wire(key); entry["value"] = to_wire(item); encoded = entry;
		} else if (text) item = String(value).substr(i, 1);
		else if (collection) { bool oob = false; item = value.get_indexed(i, valid, oob); }
		else { field_name = fields[i].name; item = value.get_named(field_name, valid); }
		if (!dictionary) encoded = to_wire(item);
		int64_t encoded_bytes = Performance::json(encoded).utf8().length();
		if (encoded_bytes > ValueCodec::INLINE_BYTES - 512) {
			rollback_values(checkpoint);
			if (dictionary) {
				Variant key = value.iter_get(cursor, valid);
				Dictionary entry; entry["key"] = ValueCodec::estimate(key) > 1024 ? retain_value(key) : to_wire(key); entry["value"] = item.get_type() == Variant::OBJECT ? to_wire(item) : retain_value(item); encoded = entry;
			} else encoded = item.get_type() == Variant::OBJECT ? to_wire(item) : retain_value(item);
			encoded_bytes = Performance::json(encoded).utf8().length();
		}
		int64_t addition = encoded_bytes + (count ? 1 : 0);
		if (!field_name.is_empty()) addition += Performance::json(field_name).utf8().length() + 1;
		if (prefix_bytes + addition > ValueCodec::INLINE_BYTES - 256) {
			rollback_values(checkpoint);
			if (count == 0) ValueCodec::fail("OUTPUT_LIMIT", "element cannot fit in a bounded page", "reference");
			break;
		}
		items.push_back(encoded);
		if (!field_name.is_empty()) named[field_name] = encoded;
		prefix_bytes += addition;
		count++;
		Performance::add(PerfMetric::PAGE_ITEMS);
		if (dictionary) more = value.iter_next(cursor, valid);
	}
	result[page_key] = fields.empty() ? Variant(items) : Variant(named);
	result["count"] = count;
	result["next_offset"] = offset + count < size ? Variant(offset + count) : Variant();
	result["truncated"] = offset + count < size;
	if (Performance::json(result).utf8().length() > ValueCodec::INLINE_BYTES) ValueCodec::fail("OUTPUT_LIMIT", "page exceeds encoded byte limit", "reference");
	return op_value(result);
}

BridgeResult Bridge::encoded_result(const Variant &value, bool dispatched) {
	try {
		BridgeResult result = op_value(to_wire(value));
		result.dispatched = dispatched;
		return result;
	} catch (const ValueFailure &failure) {
		BridgeResult result = op_error(failure.diagnostic["code"], failure.diagnostic["message"]);
		result.error_context = failure.diagnostic;
		result.dispatched = dispatched;
		return result;
	}
}

Array Bridge::converted_args(const BridgeOp &op, Object *target) {
	PerfScope cost(PerfMetric::CONVERSION_NS);
	String kind = op.kind.c_str();
	if (kind == "batch" || kind.begins_with("value_")) return op.args;
	Dictionary signature;
	String cls = target ? String(target->get_class()) : String(op.class_name.c_str());
	if (index && (kind == "set" || kind == "call" || kind == "create")) {
		String member = kind == "create" ? String("_init") : String(op.member.c_str());
		if (target && index->member_declared(index->script_class(target), member)) signature = index->member_info(index->script_class(target), member, kind == "set");
		else signature = index->member_info(cls, member, kind == "set");
	}
	Array parameters = signature.get("arguments", Array());
	if ((kind == "call" || kind == "create") && !signature.is_empty()) {
		int required = 0;
		for (const Variant &item : parameters) if (!((Dictionary)item).has("default")) required++;
		if (op.args.size() < required || (!(bool)signature.get("is_vararg", false) && op.args.size() > parameters.size())) ValueCodec::fail("BAD_ARGS", "argument count does not match the declared signature", "args");
	}
	ValueCodec codec = make_codec();
	Array result;
	for (int i = 0; i < op.args.size(); i++) {
		Dictionary expected = kind == "set" ? signature : i < parameters.size() ? Dictionary(parameters[i]) : Dictionary();
		String path = kind == "set" ? String("value") : String("args[") + itos(i) + String("]");
		try { result.push_back(codec.decode(op.args[i], expected, path)); }
		catch (ValueFailure &failure) { failure.diagnostic["argument"] = i; throw; }
	}
	return result;
}

// Resolve a handle id to a live Object*, or null.
static Object *resolve_handle(const std::unordered_map<uint64_t, ObjectID> &handles, uint64_t hid) {
	auto it = handles.find(hid);
	if (it == handles.end()) {
		return nullptr;
	}
	return ObjectDB::get_instance(it->second);
}

// Unwrap {"$handle": id} argument dicts into live objects, recursively.
// Also accepts the wire form {"__godotHandle":"hN"} — handle objects
// nested inside arrays reach the bridge as wire dicts via the JSON
// round-trip instead of the "$handle" arg form.
static Variant unwrap_arg(const std::unordered_map<uint64_t, ObjectID> &handles, const std::unordered_map<uint64_t, Variant> &values, const Variant &v) {
	if (v.get_type() == Variant::DICTIONARY) {
		Dictionary d = v;
		if (d.has("__godotValue")) {
			String name = d["__godotValue"];
			uint64_t id = name.begins_with("v") ? (uint64_t)name.substr(1).to_int() : 0;
			auto found = values.find(id);
			return found == values.end() ? Variant() : found->second;
		}
		if (d.has("$handle")) {
			uint64_t hid = (uint64_t)d["$handle"];
			Object *obj = resolve_handle(handles, hid);
			return obj ? Variant(obj) : Variant();
		}
		if (d.has("__godotHandle")) {
			String hs = d["__godotHandle"];
			uint64_t hid = hs.begins_with("h") ? hs.substr(1).to_int() : 0;
			Object *obj = hid ? resolve_handle(handles, hid) : nullptr;
			return obj ? Variant(obj) : Variant();
		}
		Dictionary out;
		for (const Variant &k : d.keys()) {
			out[k] = unwrap_arg(handles, values, d[k]);
		}
		return out;
	}
	if (v.get_type() == Variant::ARRAY) {
		Array a = v;
		Array out;
		for (int i = 0; i < a.size(); i++) {
			out.push_back(unwrap_arg(handles, values, a[i]));
		}
		return out;
	}
	return v;
}

// Permission check: index entries already merge inherited members, so the
// object's own class entry is the complete grant set for it. (Walk a few
// native ancestors too: a non-indexed intermediate class shouldn't hide
// members granted by its bases.)
static bool member_authorized(ApiIndex *index, Object *obj, const String &member, bool property = false) {
	if (!index || !obj || !index->class_allowed(obj->get_class())) return false;
	for (const String &cls : {String(obj->get_class()), index->script_class(obj)}) {
		if (index->class_allowed(cls) && !index->member_info(cls, member, property).is_empty()) return true;
	}
	return false;
}

// If a locked tier is why `member` was denied on `obj`, returns its name
// (else "") — used to point agents at godot.requestTier in error text.
static String member_locked_tier(ApiIndex *index, Object *obj, const String &member) {
	if (index == nullptr || obj == nullptr) {
		return "";
	}
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
	String cls = obj->get_class();
	for (int guard = 0; !cls.is_empty() && guard < 64; guard++) {
		String t = index->tier_of(cls);
		if (!t.is_empty()) {
			Variant d = index->describe(cls);
			if (d.get_type() == Variant::DICTIONARY) {
				for (const char *key : {"methods", "properties"}) {
					Array ms = ((Dictionary)d).get(key, Array());
					for (int m = 0; m < ms.size(); m++) {
						if (String(((Dictionary)ms[m]).get("name", "")) == member) {
							return t;
						}
					}
				}
			}
		}
		cls = cdb->get_parent_class(cls);
	}
	return "";
}

static String member_denied_msg(ApiIndex *index, Object *obj, const String &kind, const String &member) {
	String msg = String(kind) + " not allowed: " + member;
	String t = member_locked_tier(index, obj, member);
	if (!t.is_empty()) {
		msg += " (tier '" + t + "' locked — unlock via godot.requestTier(\"" + t + "\"))";
	}
	return msg;
}

static BridgeResult op_permission(const String &msg, const String &remediation = "") {
	BridgeResult r = op_error("PERMISSION", msg);
	if (!remediation.is_empty()) {
		r.error_context["remediation"] = remediation;
	}
	return r;
}

// Every denial names its way forward: a tier to request, a trusted op to
// use, or an honest "never". `for_write` distinguishes read-only denials.
static String remediation_for(ApiIndex *index, const String &cls, const String &member, bool for_write) {
	if (index == nullptr) {
		return "";
	}
	static const char *script_family[] = {
		"Script", "GDScript", "CSharpScript", "ScriptExtension",
		"ScriptLanguage", "ScriptLanguageExtension", "ScriptEditor", "ScriptEditorBase",
	};
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
	for (String c = cls; !c.is_empty();) {
		for (const char *s : script_family) {
			if (c == s) {
				return "denied permanently by policy; use godot.createScript / godot.attachScript instead";
			}
		}
		if (index->class_denied(c)) {
			String g = index->denied_group(c);
			return g.is_empty() ? String("denied permanently by policy")
					: String("denied permanently by policy (group '" + g + "')");
		}
		String t = index->tier_of(c);
		if (!t.is_empty()) {
			String p = index->policy_of(t);
			if (p == "deny" || p == "write") {
				return "group '" + t + "' policy is " + p + " — not unlockable this session";
			}
			return "call godot.requestTier(\"" + t + "\")";
		}
		c = cdb ? cdb->get_parent_class(c) : "";
	}
	if (for_write && index->class_write_denied(cls)) {
		return "writes denied permanently by policy; read-only calls on this class are allowed";
	}
	if (!member.is_empty()) {
		if (index->global_member_denied(member)) {
			return "meta-method blocked by design; use the dedicated godot.* helpers";
		}
		String denied_on;
		if (index->member_denied_on(cls, member, denied_on)) {
			return "member denied on " + denied_on + "; see allowlist.json deny_members/restrict";
		}
	}
	return "";
}

static String remediation_for_obj(ApiIndex *index, Object *obj, const String &member, bool for_write) {
	return obj ? remediation_for(index, obj->get_class(), member, for_write) : String();
}

// Passive scene inspection: names, native classes, script paths — no script
// loading or instantiation. depth/maxNodes bound the walk; truncation is
// reported instead of cutting mid-subtree.
static void scene_tree_walk(Node *n, int depth, int max_depth, int &budget,
		Dictionary &out, bool &truncated) {
	if (budget <= 0) {
		truncated = true;
		return;
	}
	budget--;
	out["name"] = String(n->get_name());
	out["class"] = n->get_class();
	Variant sv = n->get_script();
	if (sv.get_type() == Variant::OBJECT) {
		Object *s = sv;
		if (s != nullptr) {
			String p = String(s->get("resource_path"));
			if (!p.is_empty()) {
				out["script"] = p;
			}
		}
	}
	Array kids;
	if (depth < max_depth) {
		for (int i = 0; i < n->get_child_count(); i++) {
			Dictionary cd;
			scene_tree_walk(n->get_child(i), depth + 1, max_depth, budget, cd, truncated);
			kids.push_back(cd);
		}
	} else if (n->get_child_count() > 0) {
		truncated = true;
	}
	if (!kids.is_empty()) {
		out["children"] = kids;
	}
}

// Compact change reporting: default counts grouped as "Class.op"; verbose
// keeps the ordered per-op detail in tx->changes.
static void record_change(const std::unordered_map<uint64_t, ObjectID> &handles,
		TxState *tx, const BridgeOp &op) {
	if (!tx) {
		return;
	}
	String cls = op.class_name.c_str();
	if (cls.is_empty()) {
		Object *t = resolve_handle(handles, op.target);
		if (t) {
			cls = t->get_class();
		}
	}
	if (cls.is_empty()) {
		cls = "?";
	}
	if (tx->verbose) {
		Dictionary ch;
		ch["op"] = op.kind.c_str();
		ch["member"] = op.member.c_str();
		ch["class"] = cls;
		tx->changes.push_back(ch);
		return;
	}
	String key = cls + "." + String(op.kind.c_str());
	tx->change_counts[key] = (int64_t)tx->change_counts.get(key, (int64_t)0) + 1;
}

// --- Transaction journal ---------------------------------------------------
// Journal entries: {"f": forward-op, "i": inverse-op}. Simple ops are dicts:
//   {k:"set",  oid, prop, val}
//   {k:"call", oid, m, a0}         (single-arg calls only)
//   {k:"ref",  oid}                (created object: do_reference / best-effort free)
// ObjectIDs are resolved live when applied; dead ids make an op fail.

static Dictionary simple_op(const char *k, uint64_t oid) {
	Dictionary d;
	d["k"] = k;
	d["oid"] = oid;
	return d;
}

static void journal(TxState *tx, const Dictionary &fwd, const Dictionary &inv) {
	if (!tx || tx->untracked) {
		return;
	}
	Dictionary e;
	e["f"] = fwd;
	e["i"] = inv;
	tx->journal.push_back(e);
}

static bool apply_simple(const Dictionary &d) {
	uint64_t raw = d.get("oid", 0);
	Object *obj = ObjectDB::get_instance(ObjectID(raw));
	if (obj == nullptr) {
		return false;
	}
	String k = d["k"];
	if (k == "set") {
		obj->set(StringName(String(d["prop"])), d["val"]);
		return true;
	}
	if (k == "call" || k == "callv" || k == "plugin_call" || k == "plugin_restore") {
		Array args = d.has("aa") ? (Array)d["aa"] : Array();
		if (!d.has("aa")) {
			args.push_back(d["a0"]);
		}
		if (!obj->has_method(StringName(String(d["m"])))) return false;
		Variant result = obj->callv(StringName(String(d["m"])), args);
		return k != "plugin_restore" || (result.get_type() == Variant::BOOL && (bool)result);
	}
	if (k == "free") {
		// Created-but-unadopted node: safe to delete; adopted ones were
		// already detached by their add_child inverse (journal order).
		if (RefCounted *rc = Object::cast_to<RefCounted>(obj)) {
			(void)rc; // refcounted: dropping our ref is enough
			return true;
		}
		Node *n = Object::cast_to<Node>(obj);
		if (n && n->get_parent() != nullptr) {
			return false; // still in tree — refuse blind delete
		}
		memdelete(obj);
		return true;
	}
	if (k == "ref") {
		return true; // marker only used at commit
	}
	return false;
}

// Register one journal entry's forward/inverse methods into an undo action.
static void bind_entry(EditorUndoRedoManager *ur, const Dictionary &e, int &bound) {
	static const char *sides[] = {"f", "i"};
	for (const char *side : sides) {
		Dictionary d = e[side];
		String k = d.get("k", "");
		Object *obj = ObjectDB::get_instance(ObjectID((uint64_t)d.get("oid", 0)));
		bool is_do = (String(side) == "f");
		if (k == "ref") {
			if (is_do && obj) {
				ur->add_do_reference(obj);
			}
			continue;
		}
		if (obj == nullptr) {
			continue;
		}
		if (k == "set") {
			if (is_do) {
				ur->add_do_method(obj, "set", StringName(String(d["prop"])), d["val"]);
			} else {
				ur->add_undo_method(obj, "set", StringName(String(d["prop"])), d["val"]);
			}
			bound++;
		} else if (k == "call" || k == "callv" || k == "plugin_call" || k == "plugin_restore") {
			StringName m = StringName(String(d["m"]));
			Array aa = d.has("aa") ? (Array)d["aa"] : Array();
			if (!d.has("aa")) {
				aa.push_back(d["a0"]);
			}
			// Unpack up to 4 args into the variadic undo-redo binding.
			switch (aa.size()) {
				case 0:
					if (is_do) ur->add_do_method(obj, m); else ur->add_undo_method(obj, m);
					break;
				case 1:
					if (is_do) ur->add_do_method(obj, m, aa[0]); else ur->add_undo_method(obj, m, aa[0]);
					break;
				case 2:
					if (is_do) ur->add_do_method(obj, m, aa[0], aa[1]); else ur->add_undo_method(obj, m, aa[0], aa[1]);
					break;
				case 3:
					if (is_do) ur->add_do_method(obj, m, aa[0], aa[1], aa[2]); else ur->add_undo_method(obj, m, aa[0], aa[1], aa[2]);
					break;
				default:
					if (is_do) ur->add_do_method(obj, m, aa[0], aa[1], aa[2], aa[3]); else ur->add_undo_method(obj, m, aa[0], aa[1], aa[2], aa[3]);
					break;
			}
			bound++;
		} else if (k == "free" && !is_do && obj) {
			// Undo of create-without-adopt: nothing to register (kept by ref).
		}
	}
}

BridgeResult Bridge::finish_tx(const std::shared_ptr<TxState> &tx, bool commit) {
	if (!tx || tx->journal.is_empty()) {
		return op_value(Variant());
	}
	EditorInterface *ei = EditorInterface::get_singleton();
	EditorUndoRedoManager *ur = ei ? ei->get_editor_undo_redo() : nullptr;

	if (commit) {
		if (ur == nullptr) {
			return op_error("NO_UNDO", "EditorUndoRedoManager unavailable");
		}
		ur->create_action("godot-cm execute");
		int bound = 0;
		for (int i = 0; i < tx->journal.size(); i++) {
			bind_entry(ur, tx->journal[i], bound);
		}
		ur->commit_action(false); // work already applied live

		// Screen sync: after a committed write the editor's main screen
		// should match the edited scene's space. Node3D root → "3D";
		// CanvasItem (Node2D/Control) → "2D"; anything else → leave as-is.
		if (ei) {
			Node *root = ei->get_edited_scene_root();
			if (root && root->is_class("Node3D")) {
				ei->set_main_screen_editor("3D");
			} else if (root && root->is_class("CanvasItem")) {
				ei->set_main_screen_editor("2D");
			}
		}

		Dictionary d;
		d["undoable"] = bound;
		return op_value(d);
	}

	// Rollback: apply inverses newest-first.
	Array failures;
	for (int i = tx->journal.size() - 1; i >= 0; i--) {
		Dictionary e = tx->journal[i];
		if (!apply_simple(e["i"])) {
			failures.push_back(e["i"]);
		}
	}
	if (!failures.is_empty()) {
		// Spec: report partial state and block further writes pending review.
		write_locked.store(true);
		BridgeResult r = op_error("ROLLBACK_PARTIAL",
				"rollback incomplete; writes locked pending review");
		Dictionary d;
		d["unreverted"] = failures;
		r.value = d;
		return r;
	}
	return op_value(Variant());
}

BridgeResult Bridge::execute_op(const BridgeOp &incoming, const std::shared_ptr<TxState> &tx) {
	BridgeOp op = incoming;
	String kind = op.kind.c_str();
	Object *target = resolve_handle(handles, op.target);
	if (kind == "create" || kind == "set" || kind == "connect" || kind == "disconnect" || kind == "save_scene" || kind == "unlock_writes" || kind == "value_release") op.write = true;
	if (kind == "value_release" && !op.untracked) return op_error("UNTRACKED_REQUIRED", "value release requires authorized untracked mode");
	if (index && (target || kind == "create" || kind == "call" || kind == "singleton")) {
		if (target) index->observe_script(target);
		index->refresh();
	}
	PerfScope validation_cost(PerfMetric::VALIDATION_NS);
	if ((kind == "get" || kind == "set" || (kind == "call" && op.class_name.empty())) && !target) return op_error("INVALID_HANDLE", "handle expired or unknown");
	if (target && (kind == "get" || kind == "set" || kind == "call") && !member_authorized(index, target, op.member.c_str(), kind != "call")) return op_permission(member_denied_msg(index, target, kind, op.member.c_str()), remediation_for_obj(index, target, op.member.c_str(), kind != "get"));
	if (kind == "call" && !op.class_name.empty() && (!index || !index->member_allowed(op.class_name.c_str(), op.member.c_str()))) return op_permission("static member not allowed or class tier locked", remediation_for(index, op.class_name.c_str(), op.member.c_str(), true));
	if (index && target && (kind == "get" || kind == "set")) {
		Dictionary property = index->member_info(target->get_class(), op.member.c_str(), true);
		bool unknown = kind == "get" ? !(bool)property.get("is_const", false) : !index->property_reversible(target->get_class(), op.member.c_str());
		if (unknown) {
			op.write = true;
			if (!op.untracked) return op_error("UNTRACKED_REQUIRED", "unknown property accessor effects require mode untracked");
		}
	}
	bool scripted = index && target && index->member_declared(index->script_class(target), op.member.c_str());
	if (kind == "call") {
		String cls = target ? String(target->get_class()) : String(op.class_name.c_str());
		op.write = scripted || !index || !index->method_is_const(cls, op.member.c_str());
	}
	if (scripted && (kind == "get" || kind == "set" || kind == "call")) {
		op.write = true;
		if (!op.untracked) return op_error("UNTRACKED_REQUIRED", "project script access requires mode untracked and explicit permission");
	}
	if (op.write && !op.write_mode) return op_permission("mutating operation in read mode", "rerun with mode \"write\" or \"untracked\"");
	if (op.write && op.untracked && (!index || !index->tier_unlocked("untracked"))) {
		return op_permission("request tier untracked before executing without rollback", "call godot.requestTier(\"untracked\")");
	}
	String effect_class = target ? String(target->get_class()) : String(op.class_name.c_str());
	if (op.write && index && !effect_class.is_empty() && index->class_write_denied(effect_class)) {
		return op_permission("writes denied on read-only group: " + effect_class, remediation_for(index, effect_class, op.member.c_str(), true));
	}
	if (op.write && kind != "unlock_writes" && write_locked.load()) return op_error("TX_LOCKED", "writes locked pending review");
	validation_cost.stop();
	BridgeResult result;
	uint64_t checkpoint = next_value;
	try {
		op.args = converted_args(op, target);
		result = dispatch_op(op, tx);
	} catch (const ValueFailure &failure) {
		rollback_values(checkpoint);
		result = op_error(failure.diagnostic["code"], failure.diagnostic["message"]);
		result.error_context = failure.diagnostic;
	}
	if (index && op.write && kind != "value_release" && (result.ok || result.dispatched)) index->refresh(true);
	if (!result.ok) {
		result.error_context["operation"] = kind;
		result.error_context["class"] = effect_class;
		result.error_context["member"] = String(op.member.c_str());
	}
	result.write = op.write;
	if (op.untracked && op.write && tx && (result.ok || result.dispatched)) {
		Dictionary effect;
		effect["op"] = kind;
		effect["class"] = effect_class;
		effect["member"] = op.member.c_str();
		effect["status"] = result.ok ? "completed" : "failed";
		effect["rollbackable"] = false;
		tx->effects.push_back(effect);
	}
	return result;
}

BridgeResult Bridge::dispatch_op(const BridgeOp &op, const std::shared_ptr<TxState> &tx) {
	const String kind = op.kind.c_str();
	if (kind == "barrier") return op_value(Variant());
	if (kind.begins_with("value_")) return value_operation(op);

	if (kind == "tx_commit") {
		return finish_tx(tx, true);
	}
	if (kind == "tx_rollback") {
		return finish_tx(tx, false);
	}

	// Unlock path (spec editor-transactions): clears the write lock left by a
	// partial rollback. Lives before the write gate — a locked op must be
	// able to run while write_locked is set. Write-mode-gated by the worker.
	if (kind == "unlock_writes") {
		bool was = write_locked.exchange(false);
		Dictionary d;
		d["was_locked"] = was;
		return op_value(d);
	}

	if (kind == "unlocked_tiers") {
		return op_value(index ? index->unlocked_tiers() : Array());
	}

	if (kind == "permissions") {
		if (!index) {
			return op_error("NO_INDEX", "api index not loaded");
		}
		Dictionary q = op.args.size() > 0 && op.args[0].get_type() == Variant::DICTIONARY
				? (Dictionary)op.args[0] : Dictionary();
		String cls = q.get("class", "");
		String member = q.get("member", "");
		return cls.is_empty() ? op_value(index->permissions_summary())
				: op_value(index->permission_info(cls, member));
	}

	if (kind == "save_scene") {
		if (!op.untracked) return op_error("UNTRACKED_REQUIRED", "saving is not undoable; use mode untracked");
		if (!index || !index->tier_unlocked("filesystem")) return op_permission("saving requires filesystem permission", "call godot.requestTier(\"filesystem\")");
		EditorInterface *ei = EditorInterface::get_singleton();
		if (ei == nullptr) {
			return op_error("NO_EDITOR", "EditorInterface unavailable");
		}
		Error err = ei->save_scene();
		return err == OK ? op_value(Variant()) : op_error("SAVE_FAILED", "save_scene failed: " + itos((int)err));
	}

	// External-change detection: any write inside a tx re-checks the editor
	// undo version captured at the tx's first write (spec: External change).
	if (op.write) {
		if (tx && tx->external) return op_error("MIXED_EFFECTS", "no writes may follow an external action in the same execute call");
		if (tx && kind != "plugin_action" && kind != "batch") tx->wrote = true;
		if (write_locked.load()) {
			return op_error("TX_LOCKED", "writes locked: prior rollback left partial state");
		}
		if (tx && !tx->untracked) {
			EditorInterface *ei = EditorInterface::get_singleton();
			EditorUndoRedoManager *ur = ei ? ei->get_editor_undo_redo() : nullptr;
			if (ur) {
				int64_t v = (int64_t)ur->get_history_undo_redo(
										   EditorUndoRedoManager::GLOBAL_HISTORY)
									->get_version();
				if (tx->undo_version < 0) {
					tx->undo_version = v;
				} else if (v != tx->undo_version) {
					tx->conflict.store(true);
					return op_error("EXTERNAL_CONFLICT",
							"scene modified externally while transaction in flight");
				}
			}
		}
	}

	// Batch (spec godot-editor-bridge): elements run inline in this same
	// dispatch; each gets its own result so one failure doesn't fail siblings.
	if (kind == "batch") {
		for (int i = 0; i < op.elements.size(); i++) {
			String element_kind = ((Dictionary)op.elements[i]).get("kind", "");
			if (element_kind == "plugin_action" || element_kind == "save_scene") {
				return op_error("MIXED_EFFECTS", "external actions cannot be batched");
			}
		}
		Array results;
		Array warns;
		for (int i = 0; i < op.elements.size(); i++) {
			Dictionary el = op.elements[i];
			BridgeResult r;
			String ek = el.get("kind", "");
			BridgeOp sub;
			sub.kind = ek.utf8().get_data();
			sub.target = (uint64_t)el.get("target", 0);
			sub.class_name = String(el.get("class_name", "")).utf8().get_data();
			sub.member = String(el.get("member", "")).utf8().get_data();
			sub.args = el.get("args", Array());
			sub.write = el.get("write", false);
			sub.write_mode = op.write_mode;
			sub.untracked = op.untracked;
			if (sub.kind != "create" && sub.kind != "get" && sub.kind != "set" && sub.kind != "call" && sub.kind != "connect" && sub.kind != "disconnect" && sub.kind != "singleton") {
				r = op_error("BAD_OP", "invalid or nested batch element");
			} else if (sub.write && !op.write_mode) {
				r = op_permission("write op in read mode", "rerun with mode \"write\" or \"untracked\"");
			} else {
				r = execute_op(sub, tx);
				for (int w = 0; w < r.warnings.size(); w++) {
					Dictionary wd = r.warnings[w];
					wd["element"] = i;
					warns.push_back(wd);
				}
				if (r.ok && r.write && tx && !tx->untracked) {
					record_change(handles, tx.get(), sub);
				}
			}
			Dictionary rd;
			rd["ok"] = r.ok;
			if (r.ok) {
				rd["result"] = r.value;
			} else {
				Dictionary e = r.error_context.duplicate(true);
				e["code"] = r.error_code;
				e["message"] = r.error_message;
				e["batch_index"] = i;
				rd["error"] = e;
			}
			results.push_back(rd);
		}
		BridgeResult br = op_value(results);
		br.warnings = warns;
		return br;
	}

	if (kind == "editor_scene_root") {
		EditorInterface *ei = EditorInterface::get_singleton();
		if (ei == nullptr) {
			return op_error("NO_EDITOR", "EditorInterface unavailable");
		}
		return op_value(to_wire(Variant(ei->get_edited_scene_root())));
	}

	if (kind == "singleton") {
		String cls = op.class_name.c_str();
		Object *object = cls == "EditorInterface" ? EditorInterface::get_singleton()
				: Engine::get_singleton()->has_singleton(cls) ? Engine::get_singleton()->get_singleton(cls) : nullptr;
		if (!object) return op_error("NOT_FOUND", "singleton unavailable: " + cls);
		if (!index || !index->class_allowed(object->get_class())) return op_permission("singleton not allowed or class tier locked: " + cls, remediation_for(index, object->get_class(), "", false));
		return op_value(to_wire(object));
	}

	if (kind == "call" && !op.class_name.empty()) {
		String cls = op.class_name.c_str(), method = op.member.c_str();
		if (!index || !index->member_allowed(cls, method)) return op_permission("static method not allowed: " + cls + "." + method, remediation_for(index, cls, method, true));
		Dictionary entry = index->describe(cls);
		bool is_static = false;
		for (const Variant &item : (Array)entry.get("methods", Array())) {
			Dictionary info = item;
			if (String(info.get("name", "")) == method) { is_static = info.get("is_static", false); break; }
		}
		if (!is_static) return op_error("NOT_STATIC", "use an instance handle or getSingleton for this method");
		if (op.write && !op.untracked) return op_error("NO_UNDO", "static calls without an inverse require mode untracked");
		bool project_class = String(entry.get("kind", "")) == "script_class";
		Array args;
		if (!project_class) { args.push_back(cls); args.push_back(method); }
		for (const Variant &arg : op.args) args.push_back(arg);
		Ref<Script> script;
		if (project_class) {
			String path = entry.get("path", "");
			ResourceLoader *loader = ResourceLoader::get_singleton();
			if (!path.begins_with("res://") || path.simplify_path() != path || path.contains("\\")) return op_error("BAD_PATH", "invalid project class path");
			if (!loader->has_cached(path) && !index->tier_unlocked("filesystem")) return op_permission("loading a project class requires filesystem permission", "call godot.requestTier(\"filesystem\")");
			script = loader->has_cached(path) ? loader->get_cached_ref(path) : loader->load(path);
			if (script.is_null()) return op_error("LOAD_FAILED", "could not load project class");
		}
		BridgeResult result = project_class ? invoke_checked(script.ptr(), method, args) : invoke_checked(ClassDBSingleton::get_singleton(), "class_call_static", args);
		if (result.ok) result.value = to_wire(result.value);
		return result;
	}

	if (kind == "discover_plugins") {
		Dictionary out;
		out["scripts"] = script_metadata;
		Array methods;
		for (const Variant &name : plugin_methods.keys()) {
			Dictionary entry = ((Dictionary)plugin_methods[name]).duplicate(true);
			if (!ObjectDB::get_instance(ObjectID((uint64_t)entry["provider"]))) continue;
			entry.erase("provider");
			entry["name"] = name;
			methods.push_back(entry);
		}
		out["methods"] = methods;
		return op_value(out);
	}

	if (kind == "plugin_read" || kind == "plugin_call" || kind == "plugin_action") {
		String name = op.member.c_str();
		if (!plugin_methods.has(name)) return op_error("NOT_FOUND", "plugin method not registered: " + name);
		Dictionary entry = plugin_methods[name];
		bool read_only = entry.get("read_only", false);
		bool external = String(entry.get("effect", "transaction")) == "external";
		if ((external && kind != "plugin_action") || (!external && kind == "plugin_action")) {
			return op_error("EFFECT_MISMATCH", "use pluginAction only for declared external actions");
		}
		if (!read_only && !external && kind != "plugin_call") return op_permission("plugin method requires write mode", "use godot.pluginCall / pluginAction in write or untracked mode");
		String tier = entry.get("tier", "");
		if (!tier.is_empty() && (!index || !index->tier_unlocked(tier))) {
			return op_permission("plugin method requires tier: " + tier, index && index->policy_of(tier) == "deny" ? String("group '" + tier + "' policy is deny — not unlockable this session") : String("call godot.requestTier(\"" + tier + "\")"));
		}
		Object *provider = ObjectDB::get_instance(ObjectID((uint64_t)entry["provider"]));
		String method = entry.get("method", "");
		if (!provider || !provider->has_method(method)) return op_error("INVALID_HANDLE", "plugin provider unavailable");
		Array args;
		Variant input = op.args.is_empty() ? Variant(Dictionary()) : op.args[0];
		if (input.get_type() != Variant::DICTIONARY) return op_error("BAD_ARGS", "plugin arguments must be an object");
		args.push_back(input);
		if (external) {
			BridgeResult gate = begin_external(tx, name);
			if (!gate.ok) return gate;
		}
		if (!read_only && !external) {
			if (!tx) return op_error("NO_UNDO", "plugin mutation requires a transaction");
			String capture = entry.get("capture", "");
			String restore = entry.get("restore", "");
			if (!provider->has_method(capture) || !provider->has_method(restore)) {
				return op_error("NO_UNDO", "plugin capture/restore unavailable");
			}
			Variant snapshot = provider->callv(capture, args);
			if (snapshot.get_type() != Variant::DICTIONARY || !(bool)((Dictionary)snapshot).get("ok", false)) {
				String message = snapshot.get_type() == Variant::DICTIONARY ? String(((Dictionary)snapshot).get("message", "plugin capture failed")) : "plugin capture failed";
				return op_error("NO_UNDO", message);
			}
			Dictionary forward = simple_op("plugin_call", provider->get_instance_id());
			forward["m"] = method;
			forward["aa"] = args;
			Dictionary inverse = simple_op("plugin_restore", provider->get_instance_id());
			inverse["m"] = restore;
			Array undo_args;
			undo_args.push_back(((Dictionary)snapshot).get("state", Variant()));
			inverse["aa"] = undo_args;
			journal(tx.get(), forward, inverse);
		}
		Variant result = provider->callv(method, args);
		bool ok = result.get_type() == Variant::DICTIONARY && (bool)((Dictionary)result).get("ok", false);
		if (external) {
			Dictionary effect = tx->effects[0];
			effect["status"] = ok ? "completed" : "failed";
		}
		if (!ok) {
			String message = result.get_type() == Variant::DICTIONARY
					? String(((Dictionary)result).get("message", "plugin operation failed: " + name)) : "plugin operation failed: " + name;
			return op_error("PLUGIN_FAILED", message);
		}
		return op_value(to_wire(((Dictionary)result).get("result", Variant())));
	}

	if (kind == "load_resource") {
		if (!index || !index->member_allowed("ResourceLoader", "load")) {
			return op_permission("ResourceLoader.load not allowed; check filesystem tier", "call godot.requestTier(\"filesystem\")");
		}
		String path = op.member.c_str();
		PackedStringArray parts = path.substr(6).split("/");
		if (!path.begins_with("res://") || path.contains("\\") || path.simplify_path() != path ||
				parts.has("..") || parts.has(".") || parts.has("")) {
			return op_error("BAD_PATH", "resource path must be a canonical res:// path");
		}
		String extension = path.get_extension().to_lower();
		ResourceLoader *loader = ResourceLoader::get_singleton();
		if (!loader->get_recognized_extensions_for_type("").has(extension) ||
				loader->get_recognized_extensions_for_type("Script").has(extension)) {
			return op_permission("unsupported or script resource extension", "denied permanently by policy");
		}
		Ref<Resource> resource = loader->load(path);
		if (resource.is_null()) return op_error("LOAD_FAILED", "could not load: " + path);
		if (!index->class_allowed(resource->get_class())) return op_permission("resource class not allowed", remediation_for(index, resource->get_class(), "", false));
		return op_value(to_wire(resource));
	}

	if (kind == "create") {
		String cls = op.class_name.c_str();
		if (!index || !index->class_allowed(cls)) {
			String msg = "class not allowed: " + cls;
			if (index) {
				String t = index->tier_of(cls);
				if (!t.is_empty()) {
					msg += " (tier '" + t + "' locked — unlock via godot.requestTier(\"" + t + "\"))";
				}
			}
			return op_permission(msg, remediation_for(index, cls, "", true));
		}
		ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
		Dictionary entry = index->describe(cls);
		bool project_class = String(entry.get("kind", "")) == "script_class";
		if ((project_class || String(entry.get("source", "")) != "engine") && !op.untracked) return op_error("UNTRACKED_REQUIRED", "plugin and project constructors require mode untracked");
		if (!project_class && !op.args.is_empty()) return op_error("BAD_ARGS", "native ClassDB constructors take no arguments; use a static factory");
		if (!project_class && !cdb->can_instantiate(cls)) {
			return op_error("NOT_INSTANTIABLE", "cannot instantiate: " + cls);
		}
		// NB: instantiate() returns a Variant — for RefCounted classes the
		// ONLY valid lifetime is that Variant's ref. Casting it straight to
		// Object* loses the engine binding (observed: oid=0 wrapper that
		// crashes on any engine call).
		Variant created;
		if (project_class) {
			String path = entry.get("path", "");
			if (!path.begins_with("res://") || path.simplify_path() != path || path.contains("\\")) return op_error("BAD_PATH", "invalid project class path");
			ResourceLoader *loader = ResourceLoader::get_singleton();
			if (!loader->has_cached(path) && !index->tier_unlocked("filesystem")) return op_permission("loading an uncached project class requires filesystem permission", "call godot.requestTier(\"filesystem\")");
			Ref<Script> script(loader->has_cached(path) ? loader->get_cached_ref(path) : loader->load(path));
			if (script.is_null()) return op_error("LOAD_FAILED", "project script could not be loaded");
			Array constructor_args;
			for (const Variant &arg : op.args) constructor_args.push_back(arg);
			BridgeResult result = invoke_checked(script.ptr(), "new", constructor_args);
			if (!result.ok) return result;
			created = result.value;
		} else {
			PerfScope native_cost(PerfMetric::NATIVE_NS);
			created = cdb->instantiate(cls);
		}
		Object *obj = created.get_type() == Variant::OBJECT
				? created.get_validated_object() : nullptr;
		if (obj == nullptr) {
			return op_error("CREATE_FAILED", "instantiate returned null: " + cls);
		}
		// RefCounted results keep the Variant ref itself (correct refcount
		// semantics); plain Objects (nodes) are tracked for cleanup if
		// never adopted into the tree.
		if (Object::cast_to<RefCounted>(obj)) {
			retained_rc.push_back(Ref<RefCounted>(created));
		} else {
			retained_nodes.push_back(created);
		}
		uint64_t oid = (uint64_t)obj->get_instance_id();
		if (Node *node = Object::cast_to<Node>(obj); node && String(node->get_name()).is_empty()) node->set_name(String(obj->get_class()) + "_" + itos(oid));
		journal(tx.get(), simple_op("ref", oid), simple_op("free", oid));
		uint64_t hid = mint_handle(obj);
		Dictionary wire;
		wire["__godotHandle"] = reference_name(hid, "h");
		wire["class_name"] = obj->get_class();
		String script_class = index->script_class(obj);
		if (!script_class.is_empty()) wire["script_class"] = script_class;
		return op_value(wire);
	}

	if (kind == "scene_tree") {
		Node *root_node = nullptr;
		if (op.target) {
			root_node = Object::cast_to<Node>(resolve_handle(handles, op.target));
		} else {
			EditorInterface *ei = EditorInterface::get_singleton();
			if (ei) {
				root_node = ei->get_edited_scene_root();
			}
		}
		if (!root_node) {
			return op_error("NOT_FOUND", "no scene root (open a scene or pass a node handle)");
		}
		Dictionary opts = op.args.size() > 0 && op.args[0].get_type() == Variant::DICTIONARY
				? (Dictionary)op.args[0] : Dictionary();
		int max_depth = (int)(int64_t)opts.get("depth", 32);
		int budget = (int)(int64_t)opts.get("maxNodes", 2000);
		Dictionary tree;
		bool truncated = false;
		scene_tree_walk(root_node, 0, max_depth, budget, tree, truncated);
		if (truncated) {
			tree["truncated"] = true;
		}
		return op_value(tree);
	}

	// Handle-targeted ops
	Object *target = resolve_handle(handles, op.target);
	if (target == nullptr) {
		return op_error("INVALID_HANDLE", "handle expired or unknown");
	}

	if (kind == "instantiate_scene") {
		if (!index || !index->member_allowed("PackedScene", "instantiate")) {
			return op_permission("PackedScene.instantiate not allowed", remediation_for(index, "PackedScene", "instantiate", true));
		}
		PackedScene *scene = Object::cast_to<PackedScene>(target);
		if (!scene) return op_error("BAD_TARGET", "instantiateScene requires a PackedScene handle");
		Node *node = scene->instantiate();
		if (!node) return op_error("CREATE_FAILED", "scene instantiation failed");
		retained_nodes.push_back(node);
		journal(tx.get(), simple_op("ref", node->get_instance_id()), simple_op("free", node->get_instance_id()));
		return op_value(to_wire(node));
	}

	// Signal wiring (spec godot-editor-bridge): connect/disconnect need a
	// Callable, which JS can't express — the bridge builds it here.
	// op.member = signal name; args = [to_handle, method, flags?, binds?].
	if (kind == "connect" || kind == "disconnect") {
		StringName sig = StringName(String(op.member.c_str()));
		Object *to = nullptr;
		if (op.args.size() > 0) {
			Variant tov = op.args[0];
			to = tov.get_type() == Variant::OBJECT ? (Object *)tov : nullptr;
		}
		if (to == nullptr) {
			return op_error("BAD_TARGET", String(kind) + " needs a target node handle as arg0");
		}
		StringName method = op.args.size() > 1
				? StringName(String(op.args[1])) : StringName();
		if (index) { index->observe_script(to); index->refresh(); }
		if (!index || !index->class_allowed(target->get_class()) || !member_authorized(index, to, method)) return op_permission("signal source or callback is not authorized", remediation_for_obj(index, target, method, true));
		if (kind == "connect" && !op.untracked && (!index->method_is_const(to->get_class(), method) || index->member_declared(index->script_class(to), method))) return op_error("UNTRACKED_REQUIRED", "callbacks with unknown effects require mode untracked");
		Callable cb(to, method);
		if (kind == "connect") {
			if (!target->has_signal(sig)) {
				return op_error("BAD_SIGNAL", "no such signal on " + target->get_class() + ": " + String(sig));
			}
			int flags = op.args.size() > 2 ? (int)(int64_t)op.args[2] : 0;
			if (op.args.size() > 3) {
				Array binds = op.args[3];
				if (!binds.is_empty()) {
					cb = cb.bindv(binds);
				}
			}
			Error err = target->connect(sig, cb, flags);
			if (err != OK) {
				return op_error("CONNECT_FAILED", "connect failed: " + itos((int)err));
			}
			uint64_t oid = (uint64_t)target->get_instance_id();
			Dictionary f = simple_op("callv", oid), i = simple_op("callv", oid);
			Array fa; fa.push_back(sig); fa.push_back(cb); fa.push_back(flags);
			f["m"] = "connect"; f["aa"] = fa;
			Array ia; ia.push_back(sig); ia.push_back(cb);
			i["m"] = "disconnect"; i["aa"] = ia;
			journal(tx.get(), f, i);
			return op_value(Variant());
		}
		if (!target->is_connected(sig, cb)) {
			return op_error("NOT_CONNECTED", "signal not connected: " + String(sig));
		}
		target->disconnect(sig, cb);
		uint64_t oid = (uint64_t)target->get_instance_id();
		Dictionary f = simple_op("callv", oid), i = simple_op("callv", oid);
		Array fa; fa.push_back(sig); fa.push_back(cb);
		f["m"] = "disconnect"; f["aa"] = fa;
		Array ia; ia.push_back(sig); ia.push_back(cb); ia.push_back(op.args.size() > 2 ? op.args[2] : Variant(0));
		i["m"] = "connect"; i["aa"] = ia;
		journal(tx.get(), f, i);
		return op_value(Variant());
	}

	if (kind == "get") {
		String prop = op.member.c_str();
		if (!member_authorized(index, target, prop, true)) {
			return op_permission(member_denied_msg(index, target, "property", prop), remediation_for_obj(index, target, prop, false));
		}
		PerfScope native_cost(PerfMetric::NATIVE_NS);
		Variant v = target->get(prop);
		native_cost.stop();
		return encoded_result(v);
	}

	if (kind == "set") {
		String prop = op.member.c_str();
		if (!member_authorized(index, target, prop, true)) {
			return op_permission(member_denied_msg(index, target, "property", prop), remediation_for_obj(index, target, prop, true));
		}
		Variant arg = op.args.size() > 0 ? op.args[0] : Variant();
		uint64_t oid = (uint64_t)target->get_instance_id();
		PerfScope native_cost(PerfMetric::NATIVE_NS);
		Variant prior = target->get(prop);
		target->set(prop, arg);
		native_cost.stop();
		Dictionary f = simple_op("set", oid), i = simple_op("set", oid);
		f["prop"] = prop; f["val"] = arg;
		i["prop"] = prop; i["val"] = prior;
		journal(tx.get(), f, i);
		return op_value(Variant());
	}

	if (kind == "call") {
		String method = op.member.c_str();
		if (!member_authorized(index, target, method)) {
			return op_permission(member_denied_msg(index, target, "method", method), remediation_for_obj(index, target, method, true));
		}
		// Journal-or-reject (spec editor-transactions): a mutating call we
		// cannot invert is refused at dispatch instead of applied unjournaled.
		if (op.write && !op.untracked && !index->method_reversible(target->get_class(), method)) {
			// set_x(v) with an authorized property x is the same mutation
			// as `set` — route it through the journaled set path so both
			// spellings behave identically.
			if (method.begins_with("set_") && op.args.size() >= 1) {
				String prop = method.substr(4);
				if (member_authorized(index, target, prop, true)) {
					Variant arg = op.args[0];
					uint64_t poid = (uint64_t)target->get_instance_id();
					Variant prior = target->get(prop);
					target->set(prop, arg);
					Dictionary f = simple_op("set", poid), i = simple_op("set", poid);
					f["prop"] = prop; f["val"] = arg;
					i["prop"] = prop; i["val"] = prior;
					journal(tx.get(), f, i);
					return op_value(Variant());
				}
			}
			return op_error("NO_UNDO", "call has no journaled inverse; use mode untracked after granting tier untracked: " + method);
		}
		Array args;
		for (int i = 0; i < op.args.size(); i++) {
			args.push_back(op.args[i]);
		}
		uint64_t oid = (uint64_t)target->get_instance_id();

		// Capture prior state for journalable calls before mutating.
		Object *child = args.size() > 0 && args[0].get_type() == Variant::OBJECT
				? (Object *)args[0] : nullptr;
		uint64_t child_oid = child ? (uint64_t)child->get_instance_id() : 0;
		Node *child_node = child ? Object::cast_to<Node>(child) : nullptr;
		Node *target_node = Object::cast_to<Node>(target);
		Object *prior_owner = (method == "set_owner" && target_node)
				? (Object *)target_node->get_owner() : nullptr;
		uint64_t prior_owner_oid = prior_owner ? (uint64_t)prior_owner->get_instance_id() : 0;
		Array warns;
		// Unique-name guard: a node added under a parent must not clash with
		// an existing sibling — including names saved by earlier sessions.
		// Resolve BEFORE callv so both the live op and its journal entry
		// carry the final name.
		if (method == "add_child" && target_node && child_node) {
			StringName base = child_node->get_name();
			if (base == StringName() || String(base).begins_with("@")) {
				// Per-session salt: ObjectIDs restart every editor session, so
				// an oid-only suffix collides with names saved by earlier runs
				// (and with the engine's own autogen pattern it persists to
				// disk). ticks^pid makes a clash across sessions implausible.
				static const String salt = String::num_uint64(
						(Time::get_singleton()->get_ticks_usec() ^
						 (uint64_t)OS::get_singleton()->get_process_id()) & 0xFFFFFF,
						16);
				base = StringName(String(child_node->get_class()) + "_" + salt + "_" + itos(child_oid));
			}
			StringName nm = base;
			for (int suffix = 2; target_node->get_node_or_null(NodePath(String(nm))) != nullptr; suffix++) {
				nm = StringName(String(base) + "_" + itos(suffix));
			}
			child_node->set_name(nm);

			// Semantic lint (non-fatal): Node3D and CanvasItem (Node2D/
			// Control) live in separate transform spaces. Mixing them is
			// legal — HUDs over 3D are normal — but adding the wrong kind
			// under the wrong space silently breaks layout/transform math.
			// The engine won't complain, so the hint goes to the agent.
			if (child_node->is_class("Node3D") && target_node->is_class("CanvasItem")) {
				Dictionary w;
				w["code"] = "MIXED_SPACE";
				w["message"] = String("Node3D '") + String(child_node->get_name()) +
						"' under CanvasItem '" + String(target_node->get_name()) +
						"' — 3D transforms don't compose under a 2D/UI parent (usual fix: a plain Node or Node3D root)";
				warns.push_back(w);
			} else if (child_node->is_class("CanvasItem") && target_node->is_class("Node3D")) {
				Dictionary w;
				w["code"] = "MIXED_SPACE";
				w["message"] = String("CanvasItem '") + String(child_node->get_name()) +
						"' under Node3D '" + String(target_node->get_name()) +
						"' — the 2D child renders in its own canvas space, ignoring the parent's 3D transform";
				warns.push_back(w);
			}
		}
		Object *prior_parent = (method == "reparent" && target_node)
				? (Object *)target_node->get_parent() : nullptr;
		String prior_name = (method == "set_name" && target_node)
				? String(target_node->get_name()) : String();
		static const char *tf_mutators[] = {
				"look_at", "look_at_from_position", "translate", "translate_object_local",
				"rotate", "rotate_object_local", "rotate_x", "rotate_y", "rotate_z",
				"scale_object_local", "global_rotate", "global_scale", "global_translate",
				"orthonormalize"};
		bool tf_mutator = false;
		for (const char *m : tf_mutators) {
			if (method == m) {
				tf_mutator = true;
			}
		}
		Variant prior_tf;
		if (tf_mutator) {
			prior_tf = target->get("global_transform");
		}
		bool visibility_call = method == "show" || method == "hide";
		bool group_call = method == "add_to_group" || method == "remove_from_group";
		bool focus_call = method == "grab_focus" || method == "release_focus";
		Variant prior_visible;
		if (visibility_call) {
			prior_visible = target->get("visible");
		}
		int64_t prior_index = (method == "move_child" && child_node)
				? (int64_t)child_node->get_index() : -1;
		Node *sibling_parent = (method == "add_sibling" && target_node)
				? target_node->get_parent() : nullptr;

		BridgeResult invoked = invoke_checked(target, StringName(method), args);
		if (!invoked.ok) return invoked;
		Variant v = invoked.value;

		// Journal the structural calls we can safely invert.
		if (method == "add_child" && child_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "add_child"; f["a0"] = Variant(child);
			i["m"] = "remove_child"; i["a0"] = Variant(child);
			journal(tx.get(), f, i);
		} else if (method == "remove_child" && child_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "remove_child"; f["a0"] = Variant(child);
			i["m"] = "add_child"; i["a0"] = Variant(child);
			journal(tx.get(), f, i);
		} else if (method == "set_owner" && target_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "set_owner"; f["a0"] = args.size() > 0 ? args[0] : Variant();
			i["m"] = "set_owner";
			i["a0"] = prior_owner_oid ? Variant(ObjectDB::get_instance(ObjectID(prior_owner_oid))) : Variant();
			journal(tx.get(), f, i);
		} else if (method == "reparent" && target_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "reparent"; f["a0"] = args.size() > 0 ? args[0] : Variant();
			i["m"] = "reparent";
			i["a0"] = prior_parent ? Variant(prior_parent) : Variant();
			journal(tx.get(), f, i);
		} else if (method == "set_name" && target_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "set_name"; f["a0"] = args.size() > 0 ? args[0] : Variant();
			i["m"] = "set_name"; i["a0"] = prior_name;
			journal(tx.get(), f, i);
		} else if (tf_mutator &&
				(prior_tf.get_type() == Variant::TRANSFORM3D || prior_tf.get_type() == Variant::TRANSFORM2D)) {
			Dictionary f = simple_op("callv", oid), i = simple_op("set", oid);
			f["m"] = method; f["aa"] = args;
			i["prop"] = "global_transform"; i["val"] = prior_tf;
			journal(tx.get(), f, i);
		} else if (visibility_call && prior_visible.get_type() == Variant::BOOL) {
			Dictionary f = simple_op("callv", oid), i = simple_op("set", oid);
			f["m"] = method; f["aa"] = args;
			i["prop"] = "visible"; i["val"] = prior_visible;
			journal(tx.get(), f, i);
		} else if (group_call && args.size() > 0) {
			Dictionary f = simple_op("callv", oid), i = simple_op("callv", oid);
			f["m"] = method; f["aa"] = args;
			i["m"] = method == "add_to_group" ? "remove_from_group" : "add_to_group";
			i["aa"] = args;
			journal(tx.get(), f, i);
		} else if (method == "move_child" && child_node && prior_index >= 0) {
			Dictionary f = simple_op("callv", oid), i = simple_op("callv", oid);
			f["m"] = "move_child"; f["aa"] = args;
			i["m"] = "move_child";
			Array ia; ia.push_back(Variant(child)); ia.push_back(prior_index);
			i["aa"] = ia;
			journal(tx.get(), f, i);
		} else if (method == "add_sibling" && sibling_parent && child_node) {
			Dictionary f = simple_op("callv", oid);
			Dictionary i = simple_op("call", (uint64_t)sibling_parent->get_instance_id());
			f["m"] = "add_sibling"; f["aa"] = args;
			i["m"] = "remove_child"; i["a0"] = Variant(child);
			journal(tx.get(), f, i);
		} else if (focus_call) {
			Dictionary f = simple_op("callv", oid), i = simple_op("callv", oid);
			f["m"] = method; f["aa"] = args;
			i["m"] = method == "grab_focus" ? "release_focus" : "grab_focus";
			i["aa"] = args;
			journal(tx.get(), f, i);
		}
		BridgeResult r = encoded_result(v);
		r.warnings = warns;
		return r;
	}

	return op_error("BAD_OP", "unknown op kind");
}

void Bridge::refresh_catalog() {
	if (!index) return;
	Variant scripts = plugin_discovery.is_valid() ? plugin_discovery.call() : Variant(Array());
	script_metadata = scripts.get_type() == Variant::ARRAY ? (Array)scripts : Array();
	Dictionary operations;
	for (const Variant &name : plugin_methods.keys()) {
		Dictionary definition = plugin_methods[name];
		Object *provider = ObjectDB::get_instance(ObjectID((uint64_t)definition["provider"]));
		if (!provider) continue;
		Dictionary entry = definition.duplicate(true);
		entry.erase("provider");
		bool available = provider->has_method(String(definition.get("method", "")));
		if (!(bool)definition.get("read_only", false) && String(definition.get("effect", "transaction")) != "external") {
			available = available && provider->has_method(String(definition.get("capture", ""))) &&
					provider->has_method(String(definition.get("restore", "")));
		}
		entry["available"] = available;
		operations[name] = entry;
	}
	index->publish_catalog(script_metadata, operations);
}

void Bridge::drain(double budget_ms) {
	bool removed = false;
	for (const Variant &name : plugin_methods.keys()) {
		Dictionary entry = plugin_methods[name];
		if (!ObjectDB::get_instance(ObjectID((uint64_t)entry["provider"]))) {
			plugin_methods.erase(name);
			removed = true;
		}
	}
	if (removed) refresh_catalog();
	auto start = std::chrono::steady_clock::now();
	while (true) {
		std::shared_ptr<PendingBridge> pb;
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (in_queue.empty()) {
				return;
			}
			pb = in_queue.front();
			in_queue.pop_front();
			inflight[pb->op.id] = pb;
		}

		if (pb->queued_ns) Performance::add(PerfMetric::QUEUE_WAIT_NS, Performance::now() - pb->queued_ns);
		if (pb->cancel && pb->cancel->load()) {
			pb->result = op_error("CANCELLED", "operation cancelled");
		} else if (pb->op.kind == "request_tier") {
			// Tier gate: parked (not done) while the confirmation dialog
			// is open; settled by resolve_tier().
			String tier = pb->op.member.c_str();
			String policy = index ? index->policy_of(tier) : "ask";
			if (!index || !index->has_tier(tier)) {
				pb->result = op_error("BAD_TIER", "unknown permission tier: " + tier);
			} else if (index->tier_unlocked(tier)) {
				Dictionary d;
				d["granted"] = true;
				d["tier"] = tier;
				d["already_unlocked"] = true;
				pb->result = op_value(d);
			} else if (policy == "allow") {
				index->unlock_tier(tier);
				refresh_catalog();
				Dictionary d;
				d["granted"] = true;
				d["tier"] = tier;
				pb->result = op_value(d);
			} else if (policy == "deny" || policy == "write") {
				Dictionary d;
				d["granted"] = false;
				d["tier"] = tier;
				d["policy"] = policy;
				pb->result = op_value(d);
			} else if (policy == "off") {
				// Dormant group: classes are already callable; nothing to grant.
				Dictionary d;
				d["granted"] = true;
				d["tier"] = tier;
				d["policy"] = "off";
				pb->result = op_value(d);
			} else if (tier_request_cb) {
				{
					std::lock_guard<std::mutex> lock(mutex);
					tier_waiters[tier.utf8().get_data()].push_back(pb);
				}
				tier_request_cb(tier);
				continue; // stays inflight until resolve_tier
			} else {
				pb->result = op_error("NO_UI", "no channel to confirm tier: " + tier);
			}
		} else {
			pb->result = execute_op(pb->op, pb->tx);
			if (pb->result.ok && pb->result.write && pb->tx && !pb->tx->untracked) {
				record_change(handles, pb->tx.get(), pb->op);
			}
		}
		pb->done.store(true);
		{
			std::lock_guard<std::mutex> lock(mutex);
			inflight.erase(pb->op.id);
		}

		double us = std::chrono::duration<double, std::micro>(
				std::chrono::steady_clock::now() - start)
				.count();
		if (us >= budget_ms * 1000.0) {
			return;
		}
	}
}

void Bridge::resolve_tier(const String &tier, bool granted) {
	std::vector<std::shared_ptr<PendingBridge>> parked;
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto it = tier_waiters.find(tier.utf8().get_data());
		if (it == tier_waiters.end()) {
			return;
		}
		parked.swap(it->second);
		tier_waiters.erase(it);
	}
	if (granted) refresh_catalog();
	for (auto &pb : parked) {
		if (pb->cancel && pb->cancel->load()) {
			pb->result = op_error("CANCELLED", "operation cancelled");
		} else {
			Dictionary d;
			d["granted"] = granted;
			d["tier"] = tier;
			pb->result = op_value(d);
		}
		pb->done.store(true);
		std::lock_guard<std::mutex> lock(mutex);
		inflight.erase(pb->op.id);
	}
}

void Bridge::reset() {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto &[tier, parked] : tier_waiters) {
		for (auto &pb : parked) {
			pb->result = op_error("CANCELLED", "session reset");
			pb->done.store(true);
			inflight.erase(pb->op.id);
		}
	}
	tier_waiters.clear();
	in_queue.clear();
	inflight.clear();
	handles.clear();
	object_handles.clear();
	values.clear();
	retained_rc.clear();
	for (int i = 0; i < retained_nodes.size(); i++) {
		Variant v = retained_nodes[i];
		if (v.get_type() == Variant::OBJECT) {
			Object *o = v.get_validated_object();
			if (o) {
				// best-effort free of unadopted temp nodes only — a node
				// already parented into the scene belongs to the tree.
				Node *n = Object::cast_to<Node>(o);
				if (n == nullptr || n->get_parent() == nullptr) {
					memdelete(o);
				}
			}
		}
	}
	retained_nodes.clear();
}

} // namespace godot_cm
