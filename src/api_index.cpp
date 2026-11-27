#include "api_index.hpp"
#include "value_codec.hpp"
#include "performance.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace godot_cm {

ApiCatalog::ApiCatalog() { Performance::catalog_created(); }
ApiCatalog::~ApiCatalog() { Performance::catalog_destroyed(); }

static Variant parse_json_file(const String &path) {
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ);
	if (f.is_null()) {
		return Variant();
	}
	String text = f->get_as_text();
	Ref<JSON> j;
	j.instantiate();
	if (j->parse(text) != OK) {
		return Variant();
	}
	return j->get_data();
}

static Variant plain_metadata(const Variant &value, int depth = 0) {
	if (depth > 32) return Variant();
	if (value.get_type() == Variant::ARRAY) {
		Array result;
		for (const Variant &item : (Array)value) result.push_back(plain_metadata(item, depth + 1));
		return result;
	}
	if (value.get_type() == Variant::DICTIONARY) {
		Dictionary source = value, result;
		for (const Variant &key : source.keys()) {
			if (key.get_type() == Variant::STRING || key.get_type() == Variant::STRING_NAME) result[String(key)] = plain_metadata(source[key], depth + 1);
		}
		return result;
	}
	if (value.get_type() == Variant::STRING_NAME) return String(value);
	return value.get_type() <= Variant::STRING ? value : Variant();
}

static bool reversible_method(const String &owner, const String &method) {
	if (owner == "Node") {
		return method == "add_child" || method == "remove_child" || method == "set_owner" || method == "reparent" || method == "set_name" ||
				method == "add_to_group" || method == "remove_from_group" || method == "move_child" || method == "add_sibling";
	}
	if (owner == "Control") {
		return method == "grab_focus" || method == "release_focus";
	}
	if (owner == "Node3D" || owner == "CanvasItem") {
		if (method == "show" || method == "hide") return true; // inverse: set visible
	}
	return (owner == "Node3D" || owner == "Node2D") && (method == "look_at" || method == "look_at_from_position" ||
			method == "translate" || method == "translate_object_local" || method == "rotate" || method == "rotate_object_local" ||
			method == "rotate_x" || method == "rotate_y" || method == "rotate_z" || method == "scale_object_local" ||
			method == "global_rotate" || method == "global_scale" || method == "global_translate" || method == "orthonormalize");
}

static Dictionary collection_type(const String &name) {
	Dictionary result;
	if (name.is_empty()) return result;
	for (int i = 0; i < Variant::VARIANT_MAX; i++) {
		if (name == Variant::get_type_name((Variant::Type)i) || (i == Variant::NIL && name == "Variant")) {
			result["variant_type"] = i;
			result["type"] = name;
			return result;
		}
	}
	if (name.is_valid_identifier()) {
		result["variant_type"] = (int)Variant::OBJECT;
		result["type"] = name;
		result["class_name"] = name;
	}
	return result;
}

static Dictionary reflected_type(const Dictionary &raw, bool returns = false) {
	Dictionary result;
	result["name"] = raw.get("name", "");
	String cls = raw.get("class_name", "");
	Variant raw_type = raw.get("type", Variant());
	bool known = raw_type.get_type() == Variant::INT;
	int type = known ? (int)raw_type : (int)Variant::NIL;
	int usage = raw.get("usage", 0);
	int hint = raw.get("hint", 0);
	String hint_string = raw.get("hint_string", "");
	result["variant_type"] = known ? Variant(type) : Variant();
	result["class_name"] = cls;
	result["usage"] = usage;
	result["hint"] = hint;
	result["hint_string"] = hint_string;
	result["metadata_status"] = known ? "declared" : "unknown";
	result["type"] = type == Variant::OBJECT && !cls.is_empty() ? cls : Variant::get_type_name((Variant::Type)type);
	if (type == Variant::NIL) result["type"] = (usage & PROPERTY_USAGE_NIL_IS_VARIANT) || !returns ? "Variant" : "void";
	if (type == Variant::ARRAY && hint == PROPERTY_HINT_ARRAY_TYPE) {
		Dictionary element = collection_type(hint_string);
		if (!element.is_empty()) result["element_type"] = element;
	}
	if (type == Variant::DICTIONARY && hint == PROPERTY_HINT_DICTIONARY_TYPE) {
		PackedStringArray parts = hint_string.split(";");
		if (parts.size() == 2) {
			Dictionary key = collection_type(parts[0]), value = collection_type(parts[1]);
			if (!key.is_empty()) result["key_type"] = key;
			if (!value.is_empty()) result["value_type"] = value;
		}
	}
	return result;
}

static Dictionary reflected_method(const Dictionary &raw) {
	Dictionary result;
	result["name"] = raw.get("name", "");
	result["return_value"] = reflected_type(raw.get("return", Dictionary()), true);
	Array arguments;
	Array args = raw.get("args", Array());
	Array defaults = raw.get("default_args", Array());
	for (int i = 0; i < args.size(); i++) {
		Dictionary arg = reflected_type(args[i]);
		arg["index"] = i;
		int d = i - (args.size() - defaults.size());
		if (d >= 0) {
			try { arg["default"] = ValueCodec().encode(defaults[d]); arg["default_status"] = "encoded"; }
			catch (const ValueFailure &) { arg["default"] = Variant(); arg["default_status"] = "unavailable"; }
		}
		arguments.push_back(arg);
	}
	result["arguments"] = arguments;
	int flags = raw.get("flags", 0);
	result["is_const"] = bool(flags & METHOD_FLAG_CONST);
	result["is_static"] = bool(flags & METHOD_FLAG_STATIC);
	result["is_vararg"] = bool(flags & METHOD_FLAG_VARARG);
	return result;
}

Dictionary ApiIndex::inspect_script(Object *object) {
	Script *script = Object::cast_to<Script>(object);
	if (!script || !script->get_path().begins_with("res://")) return Dictionary();
	Performance::add(PerfMetric::SCRIPT_REFLECTIONS);
	String path = script->get_path(), global_name = script->get_global_name();
	Ref<Script> base = script->get_base_script();
	String base_name = base.is_valid() ? String(base->get_global_name()) : String();
	Array raw_methods = script->get_script_method_list(), raw_properties = script->get_script_property_list();
	Dictionary stamp;
	stamp["identity"] = (int64_t)script->get_instance_id();
	stamp["class"] = global_name;
	stamp["base"] = base.is_valid() ? base_name.is_empty() ? "script:" + base->get_path() : base_name : String(script->get_instance_base_type());
	Array method_stamps, defaults;
	for (const Variant &method : raw_methods) {
		Dictionary raw = ((Dictionary)method).duplicate();
		Array values = raw.get("default_args", Array());
		raw.erase("default_args");
		method_stamps.push_back(plain_metadata(raw));
		Array encoded;
		for (const Variant &value : values) {
			try { encoded.push_back(ValueCodec().encode(value)); }
			catch (const ValueFailure &) { encoded.push_back(Variant()); }
		}
		defaults.push_back(encoded);
	}
	stamp["methods"] = method_stamps;
	stamp["properties"] = plain_metadata(raw_properties);
	stamp["defaults"] = defaults;
	if (script_descriptors.has(path)) {
		Dictionary cached = script_descriptors[path];
		if (Variant(cached["stamp"]) == Variant(stamp)) {
			Performance::add(PerfMetric::CACHE_HITS);
			return cached["info"];
		}
	}
	Performance::add(PerfMetric::CACHE_MISSES);
	Dictionary info;
	info["class"] = global_name.is_empty() ? "script:" + path : global_name;
	info["path"] = path;
	info["base"] = stamp["base"];
	Array methods, properties;
	for (const Variant &method : raw_methods) methods.push_back(reflected_method(method));
	for (const Variant &property : raw_properties) properties.push_back(reflected_type(property));
	info["methods"] = methods;
	info["properties"] = properties;
	Dictionary cached;
	cached["stamp"] = stamp;
	cached["info"] = info;
	script_descriptors[path] = cached;
	Performance::set(PerfMetric::SCRIPT_CACHE_ENTRIES, script_descriptors.size());
	dirty = true;
	return info;
}

void ApiIndex::observe_script(Object *object, bool inspect) {
	if (!object) return;
	Ref<Script> script = Object::cast_to<Script>(object) ? Ref<Script>(Object::cast_to<Script>(object)) : Ref<Script>(object->get_script());
	for (int depth = 0; script.is_valid() && depth < 64; depth++, script = script->get_base_script()) {
		String path = script->get_path();
		if (!path.begins_with("res://")) continue;
		if (!observed_scripts.has(path)) { observed_scripts[path] = true; dirty = true; }
		if (inspect) inspect_script(script.ptr());
	}
}

String ApiIndex::script_class(Object *object) const {
	if (!object) return "";
	Ref<Script> script = Object::cast_to<Script>(object) ? Ref<Script>(Object::cast_to<Script>(object)) : Ref<Script>(object->get_script());
	if (script.is_null()) return "";
	String name = script->get_global_name();
	return name.is_empty() ? "script:" + script->get_path() : name;
}

std::shared_ptr<const ApiCatalog> ApiIndex::snapshot() const {
	std::lock_guard<std::mutex> lock(catalog_mutex);
	return catalog;
}

bool ApiIndex::load(const String &allowlist_path) {
	Variant allow_v = parse_json_file(allowlist_path);
	if (allow_v.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::printerr("[godot-cm] failed to load api index or allowlist");
		return false;
	}
	policy_document = allow_v;
	loaded = false;
	return refresh();
}

void ApiIndex::invalidate(bool native) {
	dirty = true;
	native_dirty = native_dirty || native;
	if (!native) {
		script_descriptors.clear();
		Performance::set(PerfMetric::SCRIPT_CACHE_ENTRIES, 0);
	}
}

bool ApiIndex::refresh(bool reconcile) {
	PerfScope cost(PerfMetric::CATALOG_NS);
	Performance::add(PerfMetric::REFRESH_CALLS);
	if (loaded && !dirty && !reconcile) {
		Performance::add(PerfMetric::CACHE_HITS);
		return true;
	}
	Performance::add(PerfMetric::GLOBAL_DISCOVERY);
	for (const Variant &path : script_descriptors.keys()) if (!ResourceLoader::get_singleton()->has_cached(path)) { script_descriptors.erase(path); dirty = true; }
	for (const Variant &path : observed_scripts.keys()) if (!ResourceLoader::get_singleton()->has_cached(path)) { observed_scripts.erase(path); dirty = true; }
	Performance::set(PerfMetric::SCRIPT_CACHE_ENTRIES, script_descriptors.size());
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
	PackedStringArray native_names = cdb->get_class_list();
	EditorInterface *editor = EditorInterface::get_singleton();
	Array pending = editor ? Array(editor->get_open_scene_roots()) : Array();
	for (int i = 0; i < pending.size() && i < 4096; i++) {
		Node *node = Object::cast_to<Node>((Object *)pending[i]);
		if (!node) continue;
		Performance::add(PerfMetric::SCENE_NODES);
		observe_script(node, false);
		pending.append_array(node->get_children());
	}
	Array scripts = ProjectSettings::get_singleton()->get_global_class_list();
	Dictionary paths;
	for (const Variant &item : scripts) paths[((Dictionary)item).get("path", "")] = true;
	for (const Variant &path : observed_scripts.keys()) {
		if (paths.has(path)) continue;
		Dictionary info;
		info["class"] = "script:" + String(path);
		info["path"] = path;
		scripts.push_back(info);
	}
	for (int i = 0; i < scripts.size(); i++) {
		Dictionary info = ((Dictionary)scripts[i]).duplicate();
		String path = info.get("path", "");
		if (ResourceLoader::get_singleton()->has_cached(path)) {
			Ref<Script> script(ResourceLoader::get_singleton()->get_cached_ref(path));
			if (script.is_valid()) {
				observe_script(script.ptr(), false);
				Dictionary reflected = inspect_script(script.ptr());
				for (const Variant &key : reflected.keys()) info[key] = reflected[key];
			}
		}
		scripts[i] = info;
	}
	PackedStringArray singleton_names = Engine::get_singleton()->get_singleton_list();
	Dictionary aliases;
	for (const String &name : singleton_names) {
		Object *object = Engine::get_singleton()->get_singleton(name);
		if (!object) continue;
		String cls = object->get_class();
		Array names = aliases.get(cls, Array());
		names.push_back(name);
		aliases[cls] = names;
	}
	Array editor_alias;
	editor_alias.push_back("EditorInterface");
	aliases["EditorInterface"] = editor_alias;
	if (Variant(native_names) != cached_native_names || Variant(aliases) != cached_singletons) dirty = true;
	uint64_t next_revision = (((uint64_t)Variant(native_names).hash() << 32) ^ scripts.hash()) + Variant(singleton_names).hash();
	if (loaded && !dirty && next_revision == revision && Variant(scripts) == cached_scripts) {
		Performance::add(PerfMetric::CACHE_HITS);
		return true;
	}
	Performance::add(PerfMetric::CACHE_MISSES);
	dirty = false;
	Dictionary cfg = policy_document;
	bool deny_mode = String(cfg.get("mode", "")) == "denylist";
	Dictionary allow = cfg.get("classes", Dictionary());
	// EditorInterface etc. live under "singletons" or "classes" depending on the dump.
	Array all;
	if (native_dirty || Variant(native_names) != cached_native_names || Variant(aliases) != cached_singletons) {
		Dictionary declared_methods;
		Dictionary method_flags;
		auto methods_for = [&](const String &name) -> Array {
			if (!declared_methods.has(name)) {
				Array methods = cdb->class_get_method_list(name, true);
				Dictionary flags;
				for (const Variant &method : methods) {
					Dictionary info = method;
					flags[info.get("name", "")] = info.get("flags", 0);
				}
				declared_methods[name] = methods;
				method_flags[name] = flags;
			}
			return declared_methods[name];
		};
		for (const String &name : native_names) {
			Dictionary cls;
			cls["name"] = name;
			cls["inherits"] = cdb->get_parent_class(name);
			cls["kind"] = "native_class";
			cls["singletons"] = aliases.get(name, Array());
			auto api_type = cdb->class_get_api_type(name);
			cls["source"] = api_type == ClassDBSingleton::API_EXTENSION || api_type == ClassDBSingleton::API_EDITOR_EXTENSION ? "extension" : "engine";
			Array methods;
			for (const Variant &method : methods_for(name)) methods.push_back(reflected_method(method));
			cls["methods"] = methods;
			Array properties;
			for (const Variant &property : cdb->class_get_property_list(name, true)) {
				Dictionary raw = property;
				if ((int)raw.get("usage", 0) & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_CATEGORY | PROPERTY_USAGE_SUBGROUP)) continue;
				Dictionary property_info = reflected_type(raw);
				String getter = cdb->class_get_property_getter(name, raw.get("name", ""));
				property_info["is_const"] = false;
				String owner = name;
				for (int guard = 0; !getter.is_empty() && !owner.is_empty() && guard < 64; guard++) {
					methods_for(owner);
					Dictionary flags = method_flags[owner];
					if (flags.has(getter)) {
						property_info["is_const"] = bool((int)flags[getter] & METHOD_FLAG_CONST);
						break;
					}
					owner = cdb->get_parent_class(owner);
				}
				properties.push_back(property_info);
			}
			cls["properties"] = properties;
			Dictionary constants;
			for (const String &key : cdb->class_get_integer_constant_list(name)) constants[key] = cdb->class_get_integer_constant(name, key);
			cls["constants"] = constants;
			all.push_back(cls);
		}
		native_descriptors = all.duplicate();
		cached_native_names = native_names;
		cached_singletons = aliases;
		native_dirty = false;
	} else all = native_descriptors.duplicate();
	for (const Variant &item : scripts) {
		Dictionary info = item;
		Dictionary cls = info.duplicate();
		cls["name"] = info.get("class", "");
		cls["inherits"] = info.get("base", "");
		cls["kind"] = "script_class";
		cls["source"] = info.get("language", "project_script");
		cls["metadata_status"] = info.has("methods") ? "cached_members" : "class_only";
		all.push_back(cls);
	}

	// Full dump map for ancestor lookups: an allowlisted class with "*"
	// covers members it inherits, even from non-allowlisted bases.
	Dictionary all_map;
	for (int i = 0; i < all.size(); i++) {
		Dictionary c = all[i];
		all_map[c.get("name", "")] = c;
	}

	// Denylist mode: index everything except denied classes. Meta-methods
	// that bypass member checks are always blocked, no matter the config.
	Dictionary deny_classes;
	Dictionary deny_global;
	Dictionary deny_members;
	Dictionary restrict_cfg;
	for (const char *name : {"McpService", "SpikeRunner", "BridgePump"}) deny_classes[name] = true;
	tiered = Dictionary();
	tier_classes = Dictionary();
	if (deny_mode) {
		Array dc = cfg.get("deny_classes", Array());
		for (int i = 0; i < dc.size(); i++) deny_classes[dc[i]] = true;
		Array dg = cfg.get("deny_global_members", Array());
		for (int i = 0; i < dg.size(); i++) deny_global[dg[i]] = true;
		for (const char *m : {"call", "callv", "callp", "call_deferred",
				"set", "get", "set_indexed", "get_indexed", "notification",
				"propagate_notification", "propagate_call", "emit_signal",
				"free", "reference", "unreference"}) {
			deny_global[m] = true;
		}
		deny_members = cfg.get("deny_members", Dictionary());
		restrict_cfg = cfg.get("restrict", Dictionary());
		denylist_mode = deny_mode;
		denied_classes = deny_classes;
		denied_global = deny_global;
		denied_members = deny_members;
		restricted = restrict_cfg;
		// Tiers: classes that index normally but stay inert until unlocked.
		Dictionary tiers_cfg = cfg.get("tiers", Dictionary());
		for (const Variant &t : tiers_cfg.keys()) {
			String tier = t;
			Array cls_list = tiers_cfg[t];
			tier_classes[tier] = cls_list;
			for (int i = 0; i < cls_list.size(); i++) {
				tiered[cls_list[i]] = tier;
			}
		}
		if (!loaded) tier_policy = cfg.get("tier_policy", Dictionary());
		// Persistent grants + trust mode: no dialog, no friction.
		if (!loaded) {
			Array pre = cfg.get("unlocked_tiers", Array());
			for (int i = 0; i < pre.size(); i++) unlocked[pre[i]] = true;
			if (cfg.get("auto_grant_tiers", false)) {
				for (const Variant &t : tier_classes.keys()) unlocked[t] = true;
			}
		}
	}
	Array untracked_notice;
	untracked_notice.push_back("Native and project-script execution without undo or rollback; code may have arbitrary side effects, including file and network access.");
	tier_classes["untracked"] = untracked_notice;

	Dictionary method_signatures, property_signatures;
	auto signature_for = [&](const String &owner, const Dictionary &cls, const Dictionary &member, bool property) -> Dictionary {
		String name = member.get("name", "");
		Dictionary &cache = property ? property_signatures : method_signatures;
		String key = owner + String(":") + name;
		if (cache.has(key)) return cache[key];
		Dictionary sig = member.duplicate();
		bool script = String(cls.get("kind", "")) == "script_class";
		String source = cls.get("source", "engine");
		bool constant = member.get("is_const", false);
		sig["name"] = name;
		sig["script"] = script;
		sig["source"] = source;
		sig["is_const"] = constant;
		if (property) {
			sig["type"] = member.get("type", "");
			sig["read_mode"] = constant && !script ? "read" : "untracked";
			sig["write_mode"] = source == "engine" ? "write" : "untracked";
		} else {
			sig["return"] = member.get("return_value", member.get("return_type", Variant()));
			sig["arguments"] = member.get("arguments", Array());
			sig["is_static"] = member.get("is_static", false);
			sig["owner"] = owner;
			sig["mode"] = constant && !script ? "read" : source == "engine" && reversible_method(owner, name) ? "write" : "untracked";
		}
		cache[key] = sig;
		return sig;
	};
	index = Dictionary();
	for (int i = 0; i < all.size(); i++) {
		Dictionary cls = all[i];
		String name = cls.get("name", "");
		bool denied = name.is_empty();
		String ancestor = name;
		for (int guard = 0; !ancestor.is_empty() && all_map.has(ancestor) && guard < 64; guard++) {
			denied = denied || deny_classes.has(ancestor);
			if (!tiered.has(name) && tiered.has(ancestor)) tiered[name] = tiered[ancestor];
			ancestor = ((Dictionary)all_map[ancestor]).get("inherits", "");
		}
		if (denied || (String(cls.get("kind", "")) != "script_class" && !cdb->class_exists(name))) continue;
		if (deny_mode ? deny_classes.has(name) : !allow.has(name)) {
			continue;
		}
		// Effective member deny = own list + all ancestors' (a member denied
		// on a base must stay denied on every descendant, or the inherited
		// merge below would re-expose it).
		Dictionary cls_deny;
		if (deny_mode) {
			String b = name;
			for (int guard = 0; !b.is_empty() && guard < 64 && all_map.has(b); guard++) {
				if (deny_members.has(b)) {
					Array dm = deny_members[b];
					for (int d = 0; d < dm.size(); d++) cls_deny[dm[d]] = true;
				}
				b = ((Dictionary)all_map[b]).get("inherits", "");
			}
		}
		Dictionary perms;
		String restriction = name;
		for (int guard = 0; !restriction.is_empty() && !restrict_cfg.has(restriction) && all_map.has(restriction) && guard < 64; guard++) {
			restriction = ((Dictionary)all_map[restriction]).get("inherits", "");
		}
		if (deny_mode) {
			if (restrict_cfg.has(restriction)) {
				perms = restrict_cfg[restriction];
			} else {
				Dictionary all_star;
				all_star["methods"] = "*";
				all_star["properties"] = "*";
				perms = all_star;
			}
		} else {
			perms = allow[name];
		}
		Variant methods_perm = perms.get("methods", Array());
		Variant props_perm = perms.get("properties", Array());
		bool all_methods = methods_perm.get_type() == Variant::STRING && (String)methods_perm == "*";
		bool all_props = props_perm.get_type() == Variant::STRING && (String)props_perm == "*";

		Dictionary entry;
		entry["inherits"] = cls.get("inherits", "");
		entry["kind"] = cls.get("kind", "native_class");
		entry["source"] = cls.get("source", "engine");
		entry["singletons"] = cls.get("singletons", Array());
		entry["construct_mode"] = String(entry["source"]) == "engine" ? "write" : "untracked";
		entry["constants"] = cls.get("constants", Dictionary());
		entry["tier"] = tiered.get(name, "");
		if (cls.has("path")) entry["path"] = cls["path"];
		if (cls.has("metadata_status")) entry["metadata_status"] = cls["metadata_status"];
		Array methods;
		Array cls_methods = cls.get("methods", Array());
		for (int m = 0; m < cls_methods.size(); m++) {
			Dictionary md = cls_methods[m];
			String mname = md.get("name", "");
			if (!all_methods) {
				Array allowed = methods_perm;
				if (!allowed.has(mname)) {
					continue;
				}
			}
			if (deny_global.has(mname) || cls_deny.has(mname)) {
				continue;
			}
			methods.push_back(signature_for(name, cls, md, false));
		}
		entry["methods"] = methods;

		Array props;
		Array cls_props = cls.get("properties", Array());
		Array members = cls.get("members", Array()); // some dumps use "members"
		if (members.size() > 0 && cls_props.size() == 0) {
			cls_props = members;
		}
		for (int p = 0; p < cls_props.size(); p++) {
			Dictionary pd = cls_props[p];
			String pname = pd.get("name", "");
			if (!all_props) {
				Array allowed = props_perm;
				if (!allowed.has(pname)) {
					continue;
				}
			}
			if (deny_global.has(pname) || cls_deny.has(pname)) {
				continue;
			}
			props.push_back(signature_for(name, cls, pd, true));
		}
		entry["properties"] = props;

		// Inherited coverage for "*": merge ancestor-declared members so
		// member_allowed sees them without requiring per-base entries.
		if (all_methods || all_props) {
			String base = cls.get("inherits", "");
			for (int guard = 0; !base.is_empty() && guard < 64 && all_map.has(base); guard++) {
				Dictionary b = all_map[base];
				if (all_methods) {
					Array bm = b.get("methods", Array());
					for (int m = 0; m < bm.size(); m++) {
						Dictionary md = bm[m];
						String bmname = md.get("name", "");
						if (deny_global.has(bmname) || cls_deny.has(bmname)) {
							continue;
						}
						methods.push_back(signature_for(base, b, md, false));
					}
				}
				if (all_props) {
					Array bp = b.get("properties", Array());
					Array bmembers = b.get("members", Array());
					if (bmembers.size() > 0 && bp.size() == 0) {
						bp = bmembers;
					}
					for (int p = 0; p < bp.size(); p++) {
						Dictionary pd = bp[p];
						String bpname = pd.get("name", "");
						if (deny_global.has(bpname) || cls_deny.has(bpname)) {
							continue;
						}
						props.push_back(signature_for(base, b, pd, true));
					}
				}
				base = b.get("inherits", "");
			}
			entry["methods"] = methods;
			entry["properties"] = props;
		}
		index[name] = entry;
	}
	method_descriptors = Dictionary();
	property_descriptors = Dictionary();
	for (const Variant &name : index.keys()) {
		Dictionary entry = index[name];
		for (const char *kind : {"methods", "properties"}) {
			Dictionary resolved;
			for (const Variant &item : (Array)entry[kind]) {
				Dictionary member = item;
				Variant key = member["name"];
				if (!resolved.has(key)) resolved[key] = member;
			}
			(kind[0] == 'm' ? method_descriptors : property_descriptors)[name] = resolved;
		}
	}
	loaded = true;
	revision = next_revision;
	cached_scripts = scripts;
	refresh_catalog();
	Performance::add(PerfMetric::CATALOG_PUBLICATIONS);
	UtilityFunctions::print("[godot-cm] api index loaded: ", index.size(), " classes");
	return true;
}

Variant ApiIndex::describe(const String &name) const {
	return index.has(name) ? index[name] : Variant();
	UtilityFunctions::print("[godot-cm] api index loaded: ", index.size(), " classes");
	return true;
}


static String ts_type(const Variant &t) {
	String s = String(t);
	if (s.is_empty() || s == "void") {
		return "void";
	}
	if (s == "Nil") return "null";
	if (s == "int") return "GodotInt";
	if (s == "float") return "GodotFloat";
	for (int type = 0; type < Variant::VARIANT_MAX; type++) if (s == Variant::get_type_name((Variant::Type)type) && !ValueCodec::fields(type).empty()) return "Godot" + s + " | GodotValueReference<\"" + s + "\">";
	if (s == "RID" || s == "Callable" || s == "Signal") return "GodotValueReference<\"" + s + "\">";
	if (s.begins_with("Packed")) return "GodotPacked<\"" + s + "\">";
	if (s == "bool") {
		return "boolean";
	}
	if (s == "String") return "string";
	if (s == "StringName" || s == "NodePath") return "string | { $t: \"" + s + "\"; value: string }";
	if (s.begins_with("Array")) return "GodotArray";
	if (s == "Dictionary") return "GodotDictionary";
	if (s == "Variant") {
		return "any";
	}
	// Objects cross as opaque handles.
	return "GodotHandle";
}

bool ApiIndex::generate_dts(const String &path) const {
	String out;
	out += "// Generated by godot-cm from the permission-filtered API index. Do not edit.\n";
	out += "declare type GodotHandle = { readonly __godotHandle: string; readonly class_name?: string; readonly script_class?: string };\n\n";
	out += "declare interface GodotTypeDescriptor { type: string; variant_type?: number | null; class_name?: string; usage?: number; hint?: number; hint_string?: string; metadata_status?: string; element_type?: GodotTypeDescriptor; key_type?: GodotTypeDescriptor; value_type?: GodotTypeDescriptor; }\n";
	out += R"TS(declare type GodotValueReference<T extends string = string> = { readonly __godotValue: string; readonly type: T };
declare type GodotInt = number | bigint | { $t: "int"; value: string };
declare type GodotFloat = number | { $t: "float"; value: number | "NaN" | "+Infinity" | "-Infinity" | "-0" };
declare type GodotFiniteFloat = number | { $t: "float"; value: number | "-0" };
declare type GodotArray<T = unknown> = T[] | { $t: "Array"; values: T[]; element_type?: GodotTypeDescriptor } | GodotValueReference<"Array">;
declare type GodotDictionary = Record<string, unknown> | { $t: "Dictionary"; entries: { key: unknown; value: unknown }[]; key_type?: GodotTypeDescriptor; value_type?: GodotTypeDescriptor } | GodotValueReference<"Dictionary">;
declare type GodotPacked<T extends string = string> = unknown[] | { $t: T; values: unknown[] } | GodotValueReference<T>;
declare interface GodotTypeHelp extends GodotTypeDescriptor { category: string; contextual: boolean; opaque_only: boolean; fields: (GodotTypeDescriptor & { name: string; required: boolean; default?: unknown })[]; explicit_tag: string | null; contextual_format: string; explicit_format: string | null; output_format: string; positional_components: false; max_depth: number; inline_elements: number; inline_bytes: number; max_value_references: number; max_value_bytes: number; notes: string; reference_operations?: string; }
declare interface GodotValueInfo { type: string; variant_type: number; size: number; readable: boolean; fields: string[]; accounted_bytes: number; retained_references: number; retained_accounted_bytes: number; reference_limit: number; accounted_byte_limit: number; }
declare interface GodotValuePage { type: string; variant_type: number; offset: number; total: number; count: number; next_offset: number | null; truncated: boolean; items?: unknown[]; entries?: { key: unknown; value: unknown }[]; fields?: Record<string, unknown>; }
declare interface GodotDiagnostic { code: string; message: string; phase: string; operation?: string; class?: string; member?: string; argument?: number; path?: string; expected?: string; received?: string; batch_index?: number; source?: { line?: number; column?: number; stack?: string; stack_truncated?: boolean }; omitted_characters?: number; }
declare interface GodotExecutionResult { ok: boolean; result: unknown; diagnostics: GodotDiagnostic[]; changes: unknown[]; mode: "read" | "write" | "untracked"; rollback_supported: boolean; effects: { op: string; class: string; member: string; status: string; rollbackable: false }[]; partial: boolean; checkpoints: { name: string; value: unknown }[]; truncated: boolean; result_omitted?: boolean; effects_omitted?: number; changes_omitted?: number; diagnostics_omitted?: number; }
declare function checkpoint(name: string, value: unknown): void;
)TS";
	for (int type = 0; type < Variant::VARIANT_MAX; type++) {
		std::vector<ValueField> fields = ValueCodec::fields(type);
		if (fields.empty()) continue;
		String name = Variant::get_type_name((Variant::Type)type);
		out += "declare interface Godot" + name + " { $t?: \"" + name + "\";";
		for (const ValueField &field : fields) {
			String field_type = field.type == Variant::FLOAT ? String("GodotFiniteFloat") : field.type == Variant::INT ? String("GodotInt") : "Godot" + Variant::get_type_name(field.type);
			out += " " + String(field.name) + (field.optional ? "?" : "") + ": " + field_type + ";";
		}
		out += " }\n";
	}
	out += "declare namespace api {\n";
	out += "\tfunction types(): GodotTypeHelp[];\n\tfunction describeType(name: string): GodotTypeHelp | undefined;\n";
	out += "\tfunction classes(): { name: string; inherits: string; kind: string; source: string; path?: string; metadata_status?: string; singletons: string[]; construct_mode: string }[];\n";
	out += "\tfunction describe(name: string): {\n";
	out += "\t\tinherits: string;\n";
	out += "\t\tmethods: { name: string; arguments: (GodotTypeDescriptor & { name: string; index: number; default?: unknown; default_status?: \"encoded\" | \"unavailable\" })[]; return: GodotTypeDescriptor; is_vararg: boolean; mode: string }[];\n";
	out += "\t\tproperties: (GodotTypeDescriptor & { name: string; read_mode: string; write_mode: string })[];\n";
	out += "\t} | undefined;\n";
	out += "\tfunction find(query: string): string[];\n";
	out += "}\n\n";

	Array names = index.keys();
	for (int i = 0; i < names.size(); i++) {
		String cls = names[i];
		if (!cls.is_valid_identifier()) continue;
		Dictionary e = index[cls];
		out += "declare interface GodotClass_" + cls + " {\n";
		Array ms = e.get("methods", Array());
		for (int m = 0; m < ms.size(); m++) {
			Dictionary md = ms[m];
			String sig = "\t" + JSON::stringify(String(md.get("name", ""))) + "(";
			Array args = md.get("arguments", Array());
			for (int a = 0; a < args.size(); a++) {
				if (a) {
					sig += ", ";
				}
				Dictionary ad = args[a];
				sig += "arg" + String::num_int64(a) + (ad.has("default") ? "?: " : ": ") + ts_type(ad.get("type", "Variant"));
			}
			if ((bool)md.get("is_vararg", false)) sig += (args.is_empty() ? String() : String(", ")) + "...extra: any[]";
			Variant rv = md.get("return", Variant());
			String ret = "void";
			if (rv.get_type() == Variant::DICTIONARY) {
				ret = ts_type(((Dictionary)rv).get("type", "Variant"));
			} else if (rv.get_type() == Variant::STRING && !String(rv).is_empty()) {
				ret = ts_type(rv);
			}
			out += sig + "): " + ret + ";\n";
		}
		Array ps = e.get("properties", Array());
		for (int p = 0; p < ps.size(); p++) {
			Dictionary pd = ps[p];
			out += "\treadonly " + JSON::stringify(String(pd.get("name", ""))) + ": " + ts_type(pd.get("type", "Variant")) + ";\n";
		}
		out += "}\n\n";
	}

	out += "declare namespace godot {\n";
	out += "\tconst mode: \"read\" | \"write\" | \"untracked\";\n";
	out += "\tnamespace values {\n\t\tfunction inspect(reference: GodotValueReference): Promise<GodotValueInfo>;\n\t\tfunction read(reference: GodotValueReference, options?: { offset?: number; limit?: number }): Promise<GodotValuePage>;\n\t\tfunction release(reference: GodotValueReference): Promise<{ released: true }>;\n\t}\n";
	out += "\tfunction create(className: string, ...constructorArgs: any[]): Promise<GodotHandle>;\n";
	out += "\tfunction getSingleton(name: string): Promise<GodotHandle>;\n";
	out += "\tfunction get(handle: GodotHandle, prop: string): Promise<any>;\n";
	out += "\tfunction set(handle: GodotHandle, prop: string, value: any): Promise<void>;\n";
	out += "\tfunction call(handle: GodotHandle | string, method: string, ...args: any[]): Promise<any>;\n";
	out += "\tfunction saveScene(): Promise<void>;\n";
	out += "\tfunction unlockWrites(): Promise<{ was_locked: boolean }>;\n";
	out += "\tfunction requestTier(tier: string): Promise<{ granted: boolean; tier: string; already_unlocked?: boolean }>;\n";
	out += "\tfunction unlockedTiers(): Promise<string[]>;\n";
	out += "\tfunction batch(ops: {\n";
	out += "\t\top: string; target?: GodotHandle; member?: string; prop?: string;\n";
	out += "\t\tmethod?: string; class?: string; args?: any[]; value?: any;\n";
	out += "\t}[]): Promise<{ ok: boolean; result?: any; error?: { code: string; message: string } }[]>;\n";
	out += "\tnamespace editor {\n";
	out += "\t\tfunction getEditedScene(): Promise<GodotHandle>;\n";
	out += "\t}\n";
	out += "}\n";

	Ref<FileAccess> f = FileAccess::open(path, FileAccess::WRITE);
	if (f.is_null()) {
		return false;
	}
	f->store_string(out);
	f->close();
	UtilityFunctions::print("[godot-cm] wrote API surface declarations to ", path);
	return true;
}

bool ApiIndex::has_tier(const String &tier) const {
	return tier_classes.has(tier);
}

bool ApiIndex::tier_unlocked(const String &tier) const {
	return unlocked.has(tier);
}

void ApiIndex::unlock_tier(const String &tier) {
	if (tier_classes.has(tier)) {
		unlocked[tier] = true;
		refresh_catalog();
	}
}

void ApiIndex::unlock_all_tiers() {
	for (const Variant &t : tier_classes.keys()) {
		unlocked[t] = true;
	}
	refresh_catalog();
}

void ApiIndex::lock_tier(const String &tier) {
	unlocked.erase(tier);
	refresh_catalog();
}

Array ApiIndex::tier_class_names(const String &tier) const {
	return tier_classes.get(tier, Array());
}

Array ApiIndex::unlocked_tiers() const {
	return unlocked.keys();
}

String ApiIndex::policy_of(const String &tier) const {
	String p = String(tier_policy.get(tier, tier_policy.get("*", "ask")));
	if (p != "allow" && p != "deny") {
		return "ask";
	}
	return p;
}

void ApiIndex::set_tier_policy(const String &tier, const String &policy) {
	if (policy != "allow" && policy != "ask" && policy != "deny") {
		return;
	}
	tier_policy[tier] = policy;
	refresh_catalog();
}

Array ApiIndex::tiers() const {
	return tier_classes.keys();
}

String ApiIndex::tier_of(const String &class_name) const {
	if (!tiered.has(class_name)) {
		return "";
	}
	String t = tiered[class_name];
	return unlocked.has(t) ? "" : t;
}

bool ApiIndex::class_allowed(const String &class_name) const {
	if (!index.has(class_name)) {
		return false;
	}
	// Tier-gated classes stay inert until the user unlocks the tier.
	if (tiered.has(class_name) && !unlocked.has(tiered[class_name])) {
		return false;
	}
	return true;
}

Dictionary ApiIndex::member_info(const String &class_name, const String &member, bool property) const {
	const Dictionary &descriptors = property ? property_descriptors : method_descriptors;
	if (!descriptors.has(class_name)) return Dictionary();
	return ((Dictionary)descriptors[class_name]).get(member, Dictionary());
}

bool ApiIndex::method_reversible(const String &class_name, const String &method) const {
	Dictionary info = member_info(class_name, method);
	return String(info.get("source", "")) == "engine" && reversible_method(info.get("owner", ""), method);
}

bool ApiIndex::property_reversible(const String &class_name, const String &property) const {
	return String(member_info(class_name, property, true).get("source", "")) == "engine";
}

bool ApiIndex::method_is_const(const String &class_name, const String &method) const {
	Dictionary info = member_info(class_name, method);
	return (bool)info.get("is_const", false) && !(bool)info.get("script", false);
}

bool ApiIndex::member_declared(const String &class_name, const String &member) const {
	return (bool)member_info(class_name, member).get("script", false) || (bool)member_info(class_name, member, true).get("script", false);
}

bool ApiIndex::member_allowed(const String &class_name, const String &member) const {
	return class_allowed(class_name) && (!member_info(class_name, member).is_empty() || !member_info(class_name, member, true).is_empty());
}

static Variant catalog_data(const Variant &value, int depth = 0) {
	if (depth > 32) return Variant();
	switch (value.get_type()) {
		case Variant::NIL:
		case Variant::BOOL:
		case Variant::INT:
		case Variant::FLOAT:
		case Variant::STRING:
			return value;
		case Variant::STRING_NAME:
			return String(value);
		case Variant::ARRAY: {
			Array source = value;
			Array out;
			for (int i = 0; i < source.size(); i++) out.push_back(catalog_data(source[i], depth + 1));
			return out;
		}
		case Variant::DICTIONARY: {
			Dictionary source = value;
			Dictionary out;
			for (const Variant &key : source.keys()) {
				if (key.get_type() == Variant::STRING || key.get_type() == Variant::STRING_NAME) {
					out[String(key)] = catalog_data(source[key], depth + 1);
				}
			}
			return out;
		}
		default:
			return Variant();
	}
}

void ApiIndex::publish_catalog(const Array &scripts, const Dictionary &operations) {
	script_metadata = catalog_data(scripts);
	operation_metadata = catalog_data(operations);
	refresh_catalog();
}

static Dictionary metadata_type(const Dictionary &info, bool is_return = false) {
	Dictionary out;
	out["name"] = info.get("name", "");
	Variant type = info.get("type", "Variant");
	String class_name = info.get("class_name", "");
	String nil_type = is_return && !((int)info.get("usage", 0) & PROPERTY_USAGE_NIL_IS_VARIANT) ? "void" : "Variant";
	out["type"] = !class_name.is_empty() ? class_name : type.get_type() == Variant::INT
			? ((int)type == Variant::NIL ? nil_type : Variant::get_type_name((Variant::Type)(int)type))
			: String(type);
	return out;
}

// Rebuild the published catalog snapshot from `index` + registered script /
// operation metadata. Called by refresh() after index rebuild and by
// publish_catalog when plugin code registers new operations.
void ApiIndex::refresh_catalog() {
	auto next = std::make_shared<ApiCatalog>();
	auto add = [&](const String &id, const String &name, Dictionary entry) {
		if (next->entries.has(id)) return;
		entry["id"] = id;
		entry["name"] = name;
		next->entries[id] = entry;
		Array ids = next->aliases.get(name, Array());
		ids.push_back(id);
		next->aliases[name] = ids;
	};
	auto permissions = [&](const String &tier) {
		Dictionary out;
		out["tier"] = tier;
		out["locked"] = !tier.is_empty() && !tier_unlocked(tier);
		out["policy"] = tier.is_empty() ? String("allow") : policy_of(tier);
		return out;
	};
	for (const Variant &key : index.keys()) {
		String name = key;
		Dictionary entry = ((Dictionary)index[key]).duplicate();
		String kind = entry.get("kind", "native_class");
		entry["permissions"] = permissions(tiered.get(name, ""));
		Dictionary execution;
		execution["direct_access"] = class_allowed(name);
		execution["undo"] = "operation_dependent";
		entry["execution"] = execution;
		if (kind == "script_class") {
			add("script:" + String(entry.get("path", name)), name, entry);
		} else {
			add("native:" + name, name, entry);
		}
	}
	for (int i = 0; i < script_metadata.size(); i++) {
		if (script_metadata[i].get_type() != Variant::DICTIONARY) continue;
		Dictionary info = script_metadata[i];
		String name = info.get("class", info.get("name", ""));
		String path = info.get("path", "");
		if (name.is_empty()) continue;
		Dictionary entry;
		entry["kind"] = "script_class";
		entry["source"] = info.get("language", "GDScript");
		entry["path"] = path;
		entry["inherits"] = info.get("base", info.get("inherits", ""));
		entry["metadata_status"] = info.has("methods") ? "cached_members" : "class_only";
		Array methods;
		Array raw_methods = info.get("methods", Array());
		for (int m = 0; m < raw_methods.size(); m++) {
			if (raw_methods[m].get_type() != Variant::DICTIONARY) continue;
			Dictionary raw = raw_methods[m];
			Dictionary method;
			method["name"] = raw.get("name", "");
			Array args;
			Array raw_args = raw.get("args", raw.get("arguments", Array()));
			Array defaults = raw.get("default_args", raw.get("default_arguments", Array()));
			for (int a = 0; a < raw_args.size(); a++) {
				if (raw_args[a].get_type() != Variant::DICTIONARY) continue;
				Dictionary arg = metadata_type(raw_args[a]);
				int default_index = a - (raw_args.size() - defaults.size());
				if (default_index >= 0 && default_index < defaults.size()) {
					arg["default"] = defaults[default_index];
					arg["optional"] = true;
				}
				args.push_back(arg);
			}
			method["arguments"] = args;
			Variant ret = raw.get("return", raw.get("return_value", Dictionary()));
			method["return"] = ret.get_type() == Variant::DICTIONARY ? Variant(metadata_type(ret, true)) : ret;
			methods.push_back(method);
		}
		Array properties;
		Array raw_props = info.get("properties", Array());
		for (int p = 0; p < raw_props.size(); p++) {
			if (raw_props[p].get_type() == Variant::DICTIONARY) properties.push_back(metadata_type(raw_props[p]));
		}
		entry["methods"] = methods;
		entry["properties"] = properties;
		Dictionary execution;
		execution["direct_access"] = false;
		execution["reason"] = "register_plugin_operation";
		entry["execution"] = execution;
		add("script:" + (path.is_empty() ? name : path), name, entry);
	}
	for (const Variant &key : operation_metadata.keys()) {
		String name = key;
		Dictionary info = operation_metadata[key];
		Dictionary entry;
		bool read_only = info.get("read_only", false);
		bool external = String(info.get("effect", "transaction")) == "external";
		String tier = info.get("tier", "");
		entry["kind"] = "operation";
		entry["source"] = "registered";
		entry["inherits"] = "";
		entry["description"] = info.get("description", "");
		entry["arguments"] = info.get("arguments", Array());
		entry["signature_status"] = info.has("arguments") || info.has("input_schema") ? "declared" : "opaque_dictionary";
		entry["return"] = info.get("return", "Variant");
		Dictionary schema;
		schema["type"] = "object";
		entry["input_schema"] = info.get("input_schema", schema);
		entry["permissions"] = permissions(tier);
		Dictionary execution;
		execution["available"] = info.get("available", false);
		execution["invocable"] = (bool)info.get("available", false) && (tier.is_empty() || tier_unlocked(tier));
		execution["bridge"] = external ? "godot.pluginAction" : read_only ? "godot.pluginRead" : "godot.pluginCall";
		execution["name"] = name;
		execution["mode"] = read_only ? "read" : "write";
		execution["effect"] = external ? "external" : read_only ? "read" : "transaction";
		execution["undo"] = external ? "not_rollbackable" : read_only ? "not_applicable" : "capture_restore_contract";
		entry["execution"] = execution;
		add("operation:" + name, name, entry);
	}
	std::lock_guard<std::mutex> lock(catalog_mutex);
	catalog = std::move(next);
}

Array ApiCatalog::classes() const {
	Array out;
	for (const Variant &id : entries.keys()) {
		Dictionary entry = entries[id];
		Dictionary summary;
		for (const char *key : {"id", "name", "inherits", "kind", "source", "path", "metadata_status", "execution", "permissions"}) {
			if (entry.has(key)) summary[key] = entry[key];
		}
		out.push_back(summary);
	}
	return out;
}

Variant ApiCatalog::describe(const String &name) const {
	if (entries.has(name)) return entries[name];
	Array ids = aliases.get(name, Array());
	if (ids.is_empty()) return Variant();
	if (ids.size() == 1) return entries[ids[0]];
	Dictionary ambiguous;
	ambiguous["ambiguous"] = true;
	ambiguous["matches"] = ids;
	return ambiguous;
}

Array ApiCatalog::find(const String &query) const {
	Array out;
	String q = query.to_lower();
	for (const Variant &id : entries.keys()) {
		Dictionary entry = entries[id];
		String name = entry["name"];
		bool hit = name.to_lower().contains(q) || String(id).to_lower().contains(q) ||
				String(entry.get("description", "")).to_lower().contains(q);
		for (const char *key : {"methods", "properties"}) {
			Array members = entry.get(key, Array());
			for (int m = 0; m < members.size() && !hit; m++) {
				hit = String(((Dictionary)members[m]).get("name", "")).to_lower().contains(q);
			}
		}
		if (hit) {
			Array ids = aliases[name];
			out.push_back(ids.size() == 1 ? Variant(name) : id);
		}
	}
	return out;
}

Dictionary ApiIndex::permissions_summary() const {
	Dictionary out;
	Array ts;
	for (const Variant &t : tier_classes.keys()) {
		String tier = t;
		Dictionary td;
		td["name"] = tier;
		td["policy"] = policy_of(tier);
		td["unlocked"] = unlocked.has(tier);
		Array classes = tier_classes[tier];
		td["class_count"] = classes.size();
		Array examples;
		for (int i = 0; i < classes.size() && i < 8; i++) {
			examples.push_back(classes[i]);
		}
		td["class_examples"] = examples;
		ts.push_back(td);
	}
	out["tiers"] = ts;
	Array denied;
	for (const Variant &c : denied_classes.keys()) {
		denied.push_back(c);
	}
	denied.sort();
	out["denied_classes"] = denied;
	Array global;
	for (const Variant &m : denied_global.keys()) {
		global.push_back(m);
	}
	global.sort();
	out["denied_members_global"] = global;
	out["note"] = "Denied classes/members are never unlockable; tiered classes unlock via godot.requestTier(tier). Query a member with godot.permissions({class, member}).";
	return out;
}

// Explain why class/member is (not) callable. Passive: reads deny metadata
// captured at index load; grants nothing.
Dictionary ApiIndex::permission_info(const String &class_name, const String &member) const {
	Dictionary out;
	String cls = class_name;
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();

	// Class-level denial wins (deny_classes applies to descendants too).
	bool class_denied = false;
	if (denylist_mode) {
		for (String c = cls; !c.is_empty();) {
			if (denied_classes.has(c)) {
				class_denied = true;
				break;
			}
			c = cdb ? cdb->get_parent_class(c) : "";
		}
	}
	out["class"] = cls;
	out["class_allowed"] = !class_denied && index.has(cls);
	if (class_denied) {
		out["reason"] = "class_denied";
	}
	if (!class_denied && tiered.has(cls)) {
		String t = tiered[cls];
		Dictionary td;
		td["tier"] = t;
		td["locked"] = !unlocked.has(t);
		td["policy"] = policy_of(t);
		out["tier"] = td;
		if ((bool)td["locked"]) {
			out["reason"] = "tier_locked";
			out["unlock"] = "godot.requestTier(\"" + t + "\")";
		}
	}

	if (member.is_empty()) {
		out["allowed"] = (bool)out["class_allowed"] && !out.has("tier");
		return out;
	}
	out["member"] = member;
	if (class_denied) {
		out["allowed"] = false;
		return out;
	}
	if (denied_global.has(member)) {
		out["allowed"] = false;
		out["reason"] = "meta_method_denied";
		return out;
	}
	// Inherited member denials apply from the denying class down.
	for (String c = cls; !c.is_empty();) {
		if (denied_members.has(c) &&
				((Array)denied_members[c]).has(member)) {
			out["allowed"] = false;
			out["reason"] = "member_denied";
			out["denied_on"] = c;
			return out;
		}
		c = cdb ? cdb->get_parent_class(c) : "";
	}
	if (restricted.has(cls)) {
		Dictionary r = restricted[cls];
		Array methods = r.get("methods", Array());
		Array props = r.get("properties", Array());
		if (!methods.has(member) && !props.has(member)) {
			out["allowed"] = false;
			out["reason"] = "restricted_class";
			out["note"] = "this class is allow-only: only listed members are exposed";
			return out;
		}
	}
	bool allowed = member_allowed(cls, member);
	out["allowed"] = allowed;
	if (!allowed && !out.has("reason")) {
		out["reason"] = out.has("tier") ? "tier_locked" : "member_not_indexed";
	}
	return out;
}

} // namespace godot_cm
