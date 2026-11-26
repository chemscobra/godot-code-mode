#ifndef GODOT_CM_BRIDGE_HPP
#define GODOT_CM_BRIDGE_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot_cm {

class ApiIndex;

// One editor operation requested from JS (design D3/D7, spec godot-editor-bridge).
struct BridgeOp {
	uint64_t id = 0;
	std::string kind;       // "create" | "get" | "set" | "call" | "editor_scene_root"
	uint64_t target = 0;    // handle id of the target object (0 = none)
	std::string class_name; // for create
	std::string member;     // property or method name
	godot::Array args;      // Variant args; handle args as {"$handle": id}
	bool write = false;     // mutating op (requires mode=write)
	godot::Array elements;  // kind=="batch": [{kind,target,class_name,member,args,write}]
	bool write_mode = false; // caller session permits writes (batch element gating)
	bool untracked = false;
};

struct BridgeResult {
	bool ok = false;
	bool dispatched = false;
	bool write = false;
	godot::Variant value;               // Object* results arrive as {"$handle": id}
	godot::String error_code;
	godot::String error_message;
	godot::Array warnings;              // non-fatal hints, folded into execute diagnostics
};

// Per-execution transaction state shared by all ops of one `execute` call
// (spec editor-transactions): change log, inverse journal for undo/rollback,
// and the editor undo version seen at tx start for external-change detection.
struct TxState {
	godot::Array changes;        // human-readable change log (reported)
	godot::Array journal;        // entries {"f": fwd-op, "i": inverse-op}
	int64_t undo_version = -1;   // EditorUndoRedoManager version at first write
	std::atomic<bool> conflict{false};
	bool untracked = false;
	godot::Array effects;
};

// Shared worker<->main cell for one op.
struct PendingBridge {
	BridgeOp op;
	BridgeResult result;
	std::atomic<bool> done{false};
	std::shared_ptr<std::atomic<bool>> cancel; // shared request flag
	std::shared_ptr<TxState> tx;               // per-request transaction
};

// The bridge: ops queued by the JS worker are resolved on the main thread
// by drain() (called from SceneTree::process_frame via BridgePump).
class Bridge {
	mutable std::mutex mutex;
	std::deque<std::shared_ptr<PendingBridge>> in_queue;
	std::unordered_map<uint64_t, std::shared_ptr<PendingBridge>> inflight;
	std::unordered_map<uint64_t, godot::ObjectID> handles;
	std::unordered_map<uint64_t, uint64_t> object_handles;
	std::unordered_map<uint64_t, godot::Variant> values;
	uint64_t next_value = 1;
	uint64_t next_handle = 1;
	uint64_t next_op = 1;
	// Strong refs kept until session reset. RefCounted objects live in a C++
	// container: Variant() conversions on freshly instantiated refcounted
	// objects are unreliable. Unadopted nodes go in retained_nodes.
	std::vector<godot::Ref<godot::RefCounted>> retained_rc;
	godot::Array retained_nodes;
	std::atomic<bool> write_locked{false}; // set when rollback itself failed
	// request_tier ops parked while the user's confirmation dialog is open.
	std::unordered_map<std::string, std::vector<std::shared_ptr<PendingBridge>>> tier_waiters;

	uint64_t mint_handle(godot::Object *obj);
	godot::Variant to_wire(const godot::Variant &v);
	BridgeResult execute_op(const BridgeOp &op, const std::shared_ptr<TxState> &tx);
	BridgeResult dispatch_op(const BridgeOp &op, const std::shared_ptr<TxState> &tx);
	BridgeResult finish_tx(const std::shared_ptr<TxState> &tx, bool commit);

public:
	ApiIndex *index = nullptr;
	// Fired (main thread) when an op needs a tier unlock decision from the user.
	std::function<void(const godot::String &tier)> tier_request_cb;

	// Worker side: enqueue an op. Caller polls ->done.
	std::shared_ptr<PendingBridge> enqueue(const BridgeOp &op,
			std::shared_ptr<std::atomic<bool>> cancel,
			std::shared_ptr<TxState> tx);

	// Main thread: resolve queued ops under the per-frame budget.
	void drain(double budget_ms = 2.0);

	// Session end: invalidate handles, free unadopted temporaries.
	void reset();

	// Main thread: settle request_tier ops parked on `tier` after the user
	// answers the confirmation dialog.
	void resolve_tier(const godot::String &tier, bool granted);

	uint64_t make_handle_for(godot::Object *obj); // main thread only
};

// Bound pump object: lives on the main thread, drains the bridge each frame.
class BridgePump : public godot::RefCounted {
	GDCLASS(BridgePump, godot::RefCounted)

protected:
	static void _bind_methods();

public:
	Bridge *bridge = nullptr;
	uint64_t last_catalog_refresh = 0;
	void invalidate_catalog();
	void pump();
};

} // namespace godot_cm

#endif
