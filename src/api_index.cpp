#include "api_index.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
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

bool ApiIndex::load(const String &api_path, const String &allowlist_path) {
	Variant api_v = parse_json_file(api_path);
	Variant allow_v = parse_json_file(allowlist_path);
	if (api_v.get_type() != Variant::DICTIONARY || allow_v.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::printerr("[godot-cm] failed to load api index or allowlist");
		return false;
	}
	Dictionary cfg = allow_v;
	bool deny_mode = String(cfg.get("mode", "")) == "denylist";
	Dictionary allow = cfg.get("classes", Dictionary());
	Array api_classes = ((Dictionary)api_v).get("classes", Array());
	Array api_singletons = ((Dictionary)api_v).get("singletons", Array());
	// EditorInterface etc. live under "singletons" or "classes" depending on the dump.
	Array all;
	for (int i = 0; i < api_classes.size(); i++) all.push_back(api_classes[i]);
	for (int i = 0; i < api_singletons.size(); i++) all.push_back(api_singletons[i]);

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
		tier_policy = cfg.get("tier_policy", Dictionary());
		// Persistent grants + trust mode: no dialog, no friction.
		Array pre = cfg.get("unlocked_tiers", Array());
		for (int i = 0; i < pre.size(); i++) {
			unlocked[pre[i]] = true;
		}
		if (cfg.get("auto_grant_tiers", false)) {
			for (const Variant &t : tier_classes.keys()) {
				unlocked[t] = true;
			}
		}
	}

	index.clear();
	for (int i = 0; i < all.size(); i++) {
		Dictionary cls = all[i];
		String name = cls.get("name", "");
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
		if (deny_mode) {
			if (restrict_cfg.has(name)) {
				perms = restrict_cfg[name];
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
			Dictionary sig;
			sig["name"] = mname;
			sig["return"] = md.get("return_value", md.get("return_type", Variant()));
			sig["arguments"] = md.get("arguments", Array());
			methods.push_back(sig);
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
			Dictionary sig;
			sig["name"] = pname;
			sig["type"] = pd.get("type", "");
			props.push_back(sig);
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
						Dictionary sig;
						sig["name"] = md.get("name", "");
						sig["return"] = md.get("return_value", md.get("return_type", Variant()));
						sig["arguments"] = md.get("arguments", Array());
						methods.push_back(sig);
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
						Dictionary sig;
						sig["name"] = pd.get("name", "");
						sig["type"] = pd.get("type", "");
						props.push_back(sig);
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
	UtilityFunctions::print("[godot-cm] api index loaded: ", index.size(), " classes");
	return true;
}

Array ApiIndex::classes() const {
	Array out;
	Array names = index.keys();
	for (int i = 0; i < names.size(); i++) {
		Dictionary c;
		String n = names[i];
		c["name"] = n;
		c["inherits"] = ((Dictionary)index[n]).get("inherits", "");
		out.push_back(c);
	}
	return out;
}

Variant ApiIndex::describe(const String &name) const {
	if (!index.has(name)) {
		return Variant();
	}
	return index[name];
}

Array ApiIndex::find(const String &query) const {
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
		Array ms = e.get("methods", Array());
		bool hit = false;
		for (int m = 0; m < ms.size() && !hit; m++) {
			if (String(((Dictionary)ms[m]).get("name", "")).to_lower().contains(q)) {
				hit = true;
			}
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
	out += "declare type GodotHandle = { readonly __godotHandle: string };\n\n";
	out += "declare namespace api {\n";
	out += "\tfunction classes(): { name: string; inherits: string }[];\n";
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
	out += "\tconst mode: \"read\" | \"write\";\n";
	out += "\tfunction create(className: string): Promise<GodotHandle>;\n";
	out += "\tfunction get(handle: GodotHandle, prop: string): Promise<any>;\n";
	out += "\tfunction set(handle: GodotHandle, prop: string, value: any): Promise<void>;\n";
	out += "\tfunction call(handle: GodotHandle, method: string, ...args: any[]): Promise<any>;\n";
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
