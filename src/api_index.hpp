#ifndef GODOT_CM_API_INDEX_HPP
#define GODOT_CM_API_INDEX_HPP

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <functional>
#include <memory>
#include <mutex>

namespace godot_cm {

// Immutable, allowlist-filtered API metadata index (design D4, spec api-discovery).
// Rebuilt from ClassDB + observed scripts + registered operations; published as
// one shared snapshot per request. Qualified ids: native:<class>, script:<path>,
// operation:<name>; ambiguous short names return candidate ids.
class ApiCatalog {
	godot::Dictionary entries;
	godot::Dictionary aliases;
	friend class ApiIndex;

public:
	ApiCatalog();
	~ApiCatalog();
	godot::Array classes() const;                             // [{id, name, inherits, kind, ...}]
	godot::Variant describe(const godot::String &name) const; // full entry, id or unique name
	godot::Array find(const godot::String &query) const;      // name/member/description matches
};

class ApiIndex : public ApiCatalog {
	godot::Dictionary policy_document;
	godot::String policy_path;
	uint64_t policy_hash = 0;
	uint64_t last_policy_error = 0;
	godot::Dictionary observed_scripts;
	godot::Dictionary script_descriptors;
	godot::Array native_descriptors;
	godot::Variant cached_native_names, cached_singletons, cached_scripts;
	godot::Dictionary method_descriptors, property_descriptors;
	bool native_dirty = true;
	godot::Dictionary inspect_script(godot::Object *script);
	uint64_t revision = 0;
	mutable std::mutex catalog_mutex;
	std::shared_ptr<const ApiCatalog> catalog;
	godot::Dictionary index; // class_name -> {inherits, methods[], properties[]}
	godot::Dictionary tiered;   // class_name -> group name (gated until unlocked)
	godot::Dictionary tier_classes; // group name -> [class names] (for UI)
	godot::Dictionary unlocked; // group name -> true
	godot::Dictionary tier_policy;  // group name (or "*") -> policy
	godot::Dictionary group_protected; // group name -> true (dock never offers ask/allow)
	godot::Dictionary group_why; // group name -> author note from the policy file
	godot::Dictionary group_default; // group name -> shipped default policy
	godot::Dictionary denied_write;    // class_name -> true (reads allowed, writes denied)
	godot::Dictionary class_group;     // class_name -> group name producing its gate
	uint64_t last_policy_reload = 0;   // ticks_msec of last applied reload
	godot::String last_reload_status = "never"; // "never" | "applied" | "parse_error"
	bool loaded = false;
	bool dirty = true;
	godot::Dictionary denied_classes;  // passive deny metadata for godot.permissions
	godot::Dictionary denied_members;  // class -> {member: true}
	godot::Dictionary denied_global;   // meta-methods blocked everywhere
	godot::Dictionary restricted;      // class -> {methods[], properties[]}
	bool denylist_mode = false;
	godot::Array script_metadata;
	godot::Dictionary operation_metadata;
	void reload_policy();

public:
	std::function<void(bool applied)> policy_reload_cb; // fired on hot-reload apply/fail (main thread)
	void publish_catalog(const godot::Array &scripts, const godot::Dictionary &operations);
	void refresh_catalog();
	bool load(const godot::String &allowlist_path);
	bool is_loaded() const { return loaded; }
	bool refresh(bool reconcile = false);
	void invalidate(bool native = true);
	void observe_script(godot::Object *object, bool inspect = true);
	std::shared_ptr<const ApiCatalog> snapshot() const;
	godot::Variant describe(const godot::String &name) const; // direct index lookup (native + script)
	bool method_is_const(const godot::String &class_name, const godot::String &method) const;
	godot::Dictionary member_info(const godot::String &class_name, const godot::String &member, bool property = false) const;
	bool method_reversible(const godot::String &class_name, const godot::String &method) const;
	bool property_reversible(const godot::String &class_name, const godot::String &property) const;
	bool member_declared(const godot::String &class_name, const godot::String &member) const;
	godot::String script_class(godot::Object *object) const;

	bool class_allowed(const godot::String &class_name) const;
	bool member_allowed(const godot::String &class_name, const godot::String &member) const;

	// Permission tiers: classes index normally but stay inert until the
	// user unlocks the tier (per session, via confirmation dialog).
	bool has_tier(const godot::String &tier) const;
	bool tier_unlocked(const godot::String &tier) const;
	void unlock_tier(const godot::String &tier);
	void unlock_all_tiers();
	void lock_tier(const godot::String &tier);
	godot::Array tier_class_names(const godot::String &tier) const;
	godot::Array unlocked_tiers() const;
	// Policy for a locked tier: "allow" (grant silently), "ask" (dialog),
	// or "deny" (refuse silently). Default "ask".
	godot::String policy_of(const godot::String &tier) const;
	void set_tier_policy(const godot::String &tier, const godot::String &policy);
	godot::Array tiers() const;
	// Tier a class belongs to ("" if none / not locked).
	godot::String tier_of(const godot::String &class_name) const;

	// Unified permission groups: deny/write/ask/allow/off per named class set.
	// `deny` = blocked entirely, `write` = reads only, `ask`/`allow` = tier gate,
	// `off` = dormant. Legacy tiers/deny_classes migrate into this model.
	bool class_denied(const godot::String &class_name) const;      // hard deny incl. ancestors
	bool class_write_denied(const godot::String &class_name) const; // write/untracked ops only
	bool member_denied_on(const godot::String &class_name, const godot::String &member, godot::String &denied_on) const;
	bool global_member_denied(const godot::String &member) const;
	godot::String denied_group(const godot::String &class_name) const; // group that denies it ("" if flat)
	bool group_is_protected(const godot::String &group) const;
	godot::Array groups() const;
	godot::Dictionary group_info(const godot::String &group) const;
	bool group_has_state(const godot::String &group, const godot::String &state) const;
	// Persist a group's policy into the policy file (class lists untouched);
	// the hot-reload then re-applies it. Returns false on unknown group/state/IO error.
	bool save_group_policy(const godot::String &group, const godot::String &state);
	bool reset_group_policies();
	bool allow_all_groups(bool include_protected = false);
	godot::Dictionary policy_status() const;

	// Passive permission introspection: never grants, never executes.
	// {allowed, reason, tier?, policy?, locked?} for one class/member, or a
	// tier/policy summary when class_name is empty.
	godot::Dictionary permission_info(const godot::String &class_name,
			const godot::String &member) const;
	godot::Dictionary permissions_summary() const;

	// Task 3.5: emit TypeScript declarations of exactly this index.
	bool generate_dts(const godot::String &path) const;
};

} // namespace godot_cm

#endif
