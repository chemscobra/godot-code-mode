#include "api_index.hpp"

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
	if (owner == "Node") return method == "add_child" || method == "remove_child" || method == "set_owner" || method == "reparent" || method == "set_name";
	return (owner == "Node3D" || owner == "Node2D") && (method == "look_at" || method == "look_at_from_position");
}

static Dictionary reflected_type(const Dictionary &raw, bool returns = false) {
	Dictionary result;
	result["name"] = raw.get("name", "");
	String cls = raw.get("class_name", "");
	Variant type = raw.get("type", (int)Variant::NIL);
	result["type"] = !cls.is_empty() ? cls : type.get_type() == Variant::INT ? Variant::get_type_name((Variant::Type)(int)type) : String(type);
	if (type.get_type() == Variant::INT && (int)type == Variant::NIL) result["type"] = ((int)raw.get("usage", 0) & PROPERTY_USAGE_NIL_IS_VARIANT) ? "Variant" : returns ? "void" : "Variant";
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
		int d = i - (args.size() - defaults.size());
		if (d >= 0) arg["default"] = plain_metadata(defaults[d]);
		arguments.push_back(arg);
	}
	result["arguments"] = arguments;
	int flags = raw.get("flags", 0);
	result["is_const"] = bool(flags & METHOD_FLAG_CONST);
	result["is_static"] = bool(flags & METHOD_FLAG_STATIC);
	return result;
}

void ApiIndex::observe_script(Object *object) {
	if (!object) return;
	Ref<Script> script = Object::cast_to<Script>(object) ? Ref<Script>(Object::cast_to<Script>(object)) : Ref<Script>(object->get_script());
	for (int depth = 0; script.is_valid() && depth < 64; depth++, script = script->get_base_script()) {
		if (script->get_path().begins_with("res://")) observed_scripts[script->get_path()] = true;
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

bool ApiIndex::refresh() {
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
	PackedStringArray native_names = cdb->get_class_list();
	EditorInterface *editor = EditorInterface::get_singleton();
	Array pending = editor ? Array(editor->get_open_scene_roots()) : Array();
	for (int i = 0; i < pending.size() && i < 4096; i++) {
		Node *node = Object::cast_to<Node>((Object *)pending[i]);
		if (!node) continue;
		observe_script(node);
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
				observe_script(script.ptr());
				String global_name = script->get_global_name();
				info["class"] = global_name.is_empty() ? "script:" + path : global_name;
				Ref<Script> base = script->get_base_script();
				String base_name = base.is_valid() ? String(base->get_global_name()) : String();
				info["base"] = base.is_valid() ? base_name.is_empty() ? "script:" + base->get_path() : base_name : String(script->get_instance_base_type());
				Array methods;
				for (const Variant &method : script->get_script_method_list()) methods.push_back(reflected_method(method));
				Array properties;
				for (const Variant &property : script->get_script_property_list()) properties.push_back(reflected_type(property));
				info["methods"] = methods;
				info["properties"] = properties;
			}
		}
		scripts[i] = info;
	}
	PackedStringArray singleton_names = Engine::get_singleton()->get_singleton_list();
	uint64_t next_revision = (((uint64_t)Variant(native_names).hash() << 32) ^ scripts.hash()) + Variant(singleton_names).hash();
	if (loaded && !dirty && next_revision == revision) return true;
	dirty = false;
	Dictionary cfg = policy_document;
	bool deny_mode = String(cfg.get("mode", "")) == "denylist";
	Dictionary allow = cfg.get("classes", Dictionary());
	// EditorInterface etc. live under "singletons" or "classes" depending on the dump.
	Array all;
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
		Dictionary sig;
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
	loaded = true;
	revision = next_revision;
	{
		std::lock_guard<std::mutex> lock(catalog_mutex);
		catalog = std::make_shared<ApiCatalog>(index);
	}
	UtilityFunctions::print("[godot-cm] api index loaded: ", index.size(), " classes");
	return true;
}

Array ApiCatalog::classes() const {
	Array out;
	Array names = index.keys();
	for (int i = 0; i < names.size(); i++) {
		Dictionary c;
		String n = names[i];
		c["name"] = n;
		c["inherits"] = ((Dictionary)index[n]).get("inherits", "");
		Dictionary entry = index[n];
		for (const char *key : {"kind", "source", "path", "metadata_status", "tier", "singletons", "construct_mode"}) {
			if (entry.has(key)) c[key] = entry[key];
		}
		out.push_back(c);
	}
	return out;
}

Variant ApiCatalog::describe(const String &name) const {
	if (!index.has(name)) {
		return Variant();
	}
	return index[name];
}

Array ApiCatalog::find(const String &query) const {
	Array out;
	String q = query.to_lower();
	Array names = index.keys();
	for (int i = 0; i < names.size(); i++) {
		String n = names[i];
		if (n.to_lower().contains(q)) {
			out.push_back(n);
			continue;
		}
		// match members too
		Dictionary e = index[n];
		bool hit = String(e.get("path", "")).to_lower().contains(q) || String(e.get("source", "")).to_lower().contains(q);
		for (const char *field : {"methods", "properties"}) {
			Array members = e.get(field, Array());
			for (const Variant &item : members) {
				if (String(((Dictionary)item).get("name", "")).to_lower().contains(q)) { hit = true; break; }
			}
		}
		Array keys = ((Dictionary)e.get("constants", Dictionary())).keys();
		keys.append_array(e.get("singletons", Array()));
		for (const Variant &key : keys) {
			if (String(key).to_lower().contains(q)) { hit = true; break; }
		}
		if (hit) {
			out.push_back(n);
		}
	}
	return out;
}

static String ts_type(const Variant &t) {
	String s = String(t);
	if (s.is_empty() || s == "void") {
		return "void";
	}
	if (s == "int" || s == "float") {
		return "number";
	}
	if (s == "bool") {
		return "boolean";
	}
	if (s == "String" || s == "StringName" || s == "NodePath") {
		return "string";
	}
	if (s.begins_with("Array") || s == "PackedStringArray") {
		return "any[]";
	}
	if (s == "Dictionary") {
		return "Record<string, any>";
	}
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
	out += "declare namespace api {\n";
	out += "\tfunction classes(): { name: string; inherits: string; kind: string; source: string; path?: string; metadata_status?: string; singletons: string[]; construct_mode: string }[];\n";
	out += "\tfunction describe(name: string): {\n";
	out += "\t\tinherits: string;\n";
	out += "\t\tmethods: { name: string; arguments: any[]; return: any }[];\n";
	out += "\t\tproperties: { name: string; type: string }[];\n";
	out += "\t} | undefined;\n";
	out += "\tfunction find(query: string): string[];\n";
	out += "}\n\n";

	Array names = index.keys();
	for (int i = 0; i < names.size(); i++) {
		String cls = names[i];
		if (!cls.is_valid_identifier()) continue;
		Dictionary e = index[cls];
		out += "declare namespace " + cls + " {\n";
		Array ms = e.get("methods", Array());
		for (int m = 0; m < ms.size(); m++) {
			Dictionary md = ms[m];
			String sig = "\tfunction " + String(md.get("name", "")) + "(";
			Array args = md.get("arguments", Array());
			for (int a = 0; a < args.size(); a++) {
				if (a) {
					sig += ", ";
				}
				Dictionary ad = args[a];
				sig += String(ad.get("name", "a" + String::num_int64(a))) + "?: " + ts_type(ad.get("type", "Variant"));
			}
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
			out += "\tconst " + String(pd.get("name", "")) + ": " + ts_type(pd.get("type", "Variant")) + ";\n";
		}
		out += "}\n\n";
	}

	out += "declare namespace godot {\n";
	out += "\tconst mode: \"read\" | \"write\" | \"untracked\";\n";
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
	}
}

void ApiIndex::unlock_all_tiers() {
	for (const Variant &t : tier_classes.keys()) {
		unlocked[t] = true;
	}
}

void ApiIndex::lock_tier(const String &tier) {
	unlocked.erase(tier);
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
	if (!index.has(class_name)) return Dictionary();
	Array members = ((Dictionary)index[class_name]).get(property ? "properties" : "methods", Array());
	for (const Variant &item : members) {
		Dictionary info = item;
		if (String(info.get("name", "")) == member) return info;
	}
	return Dictionary();
}

bool ApiIndex::method_reversible(const String &class_name, const String &method) const {
	Dictionary info = member_info(class_name, method);
	return String(info.get("source", "")) == "engine" && reversible_method(info.get("owner", ""), method);
}

bool ApiIndex::property_reversible(const String &class_name, const String &property) const {
	return String(member_info(class_name, property, true).get("source", "")) == "engine";
}

bool ApiIndex::method_is_const(const String &class_name, const String &method) const {
	if (!index.has(class_name)) return false;
	Array methods = ((Dictionary)index[class_name]).get("methods", Array());
	for (const Variant &item : methods) {
		Dictionary info = item;
		if (String(info.get("name", "")) == method) return (bool)info.get("is_const", false) && !(bool)info.get("script", false);
	}
	return false;
}

bool ApiIndex::member_declared(const String &class_name, const String &member) const {
	if (!index.has(class_name)) return false;
	Dictionary entry = index[class_name];
	for (const char *key : {"methods", "properties"}) {
		Array members = entry.get(key, Array());
		for (const Variant &item : members) {
			Dictionary info = item;
			if (String(info.get("name", "")) == member && (bool)info.get("script", false)) return true;
		}
	}
	return false;
}

bool ApiIndex::member_allowed(const String &class_name, const String &member) const {
	if (!class_allowed(class_name)) {
		return false;
	}
	Dictionary e = index[class_name];
	Array ms = e.get("methods", Array());
	for (int i = 0; i < ms.size(); i++) {
		if (String(((Dictionary)ms[i]).get("name", "")) == member) {
			return true;
		}
	}
	Array ps = e.get("properties", Array());
	for (int i = 0; i < ps.size(); i++) {
		if (String(((Dictionary)ps[i]).get("name", "")) == member) {
			return true;
		}
	}
	return false;
}

} // namespace godot_cm
