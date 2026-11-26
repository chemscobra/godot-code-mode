#ifndef GODOT_CM_API_INDEX_HPP
#define GODOT_CM_API_INDEX_HPP

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_cm {

// Immutable, allowlist-filtered API metadata index (design D4, spec api-discovery).
// Built once at service start from extension_api.json + allowlist.json;
// read concurrently by the JS worker for api.* bindings and the allowlist check.
class ApiIndex {
	godot::Dictionary index; // class_name -> {inherits, methods[], properties[]}
	godot::Dictionary tiered;   // class_name -> tier name (gated until unlocked)
	godot::Dictionary tier_classes; // tier name -> [class names] (for UI)
	godot::Dictionary unlocked; // tier name -> true
	godot::Dictionary tier_policy;  // tier name (or "*") -> "allow"|"ask"|"deny"
	bool loaded = false;

public:
	bool load(const godot::String &extension_api_path, const godot::String &allowlist_path);
	bool is_loaded() const { return loaded; }

	godot::Array classes() const;                       // [{name, inherits}]
	godot::Variant describe(const godot::String &name) const; // filtered detail or nil
	godot::Array find(const godot::String &query) const;      // name substring matches

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

	// Task 3.5: emit TypeScript declarations of exactly this index.
	bool generate_dts(const godot::String &path) const;
};

} // namespace godot_cm

#endif
