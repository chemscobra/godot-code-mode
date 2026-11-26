#include "bridge.hpp"

#include "api_index.hpp"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace godot_cm {

void BridgePump::_bind_methods() {
	ClassDB::bind_method(D_METHOD("pump"), &BridgePump::pump);
}

void BridgePump::pump() {
	if (bridge) {
		bridge->drain();
	}
}

uint64_t Bridge::mint_handle(Object *obj) {
	uint64_t id = next_handle++;
	handles[id] = obj->get_instance_id();
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

// Convert a Variant result into wire form: objects become handle dicts,
// arrays and dictionaries are converted recursively.
Variant Bridge::to_wire(const Variant &v) {
	if (v.get_type() == Variant::OBJECT) {
		Object *obj = v;
		if (obj == nullptr) {
			return Variant();
		}
		uint64_t id = mint_handle(obj);
		Dictionary h;
		h["__godotHandle"] = "h" + String::num_uint64(id);
		return h;
	}
	if (v.get_type() == Variant::ARRAY) {
		Array a = v;
		Array out;
		for (int i = 0; i < a.size(); i++) {
			out.push_back(to_wire(a[i]));
		}
		return out;
	}
	if (v.get_type() == Variant::DICTIONARY) {
		Dictionary d = v;
		Dictionary out;
		for (const Variant &k : d.keys()) {
			out[k] = to_wire(d[k]);
		}
		return out;
	}
	return v;
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
static Variant unwrap_arg(const std::unordered_map<uint64_t, ObjectID> &handles, const Variant &v) {
	if (v.get_type() == Variant::DICTIONARY) {
		Dictionary d = v;
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
			out[k] = unwrap_arg(handles, d[k]);
		}
		return out;
	}
	if (v.get_type() == Variant::ARRAY) {
		Array a = v;
		Array out;
		for (int i = 0; i < a.size(); i++) {
			out.push_back(unwrap_arg(handles, a[i]));
		}
		return out;
	}
	return v;
}

// Permission check: index entries already merge inherited members, so the
// object's own class entry is the complete grant set for it. (Walk a few
// native ancestors too: a non-indexed intermediate class shouldn't hide
// members granted by its bases.)
static bool member_authorized(ApiIndex *index, Object *obj, const String &member) {
	if (index == nullptr || obj == nullptr) {
		return false;
	}
	ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
	String cls = obj->get_class();
	for (int guard = 0; !cls.is_empty() && guard < 64; guard++) {
		if (index->member_allowed(cls, member)) {
			return true;
		}
		cls = cdb->get_parent_class(cls);
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
	if (!tx) {
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
	if (k == "call" || k == "callv") {
		Array args = d.has("aa") ? (Array)d["aa"] : Array();
		if (!d.has("aa")) {
			args.push_back(d["a0"]);
		}
		obj->callv(StringName(String(d["m"])), args);
		return true;
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
		} else if (k == "call" || k == "callv") {
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

BridgeResult Bridge::execute_op(const BridgeOp &op, const std::shared_ptr<TxState> &tx) {
	const String kind = op.kind.c_str();

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

	if (kind == "save_scene") {
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
		if (write_locked.load()) {
			return op_error("TX_LOCKED", "writes locked: prior rollback left partial state");
		}
		if (tx) {
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
			if (sub.kind.empty() || sub.kind == "batch") {
				r = op_error("BAD_OP", "invalid or nested batch element");
			} else if (sub.write && !op.write_mode) {
				r = op_error("PERMISSION", "write op in read mode");
			} else {
				r = execute_op(sub, tx);
				for (int w = 0; w < r.warnings.size(); w++) {
					Dictionary wd = r.warnings[w];
					wd["element"] = i;
					warns.push_back(wd);
				}
				if (r.ok && sub.write && tx) {
					Dictionary ch;
					ch["op"] = ek;
					ch["member"] = sub.member.c_str();
					tx->changes.push_back(ch);
				}
			}
			Dictionary rd;
			rd["ok"] = r.ok;
			if (r.ok) {
				rd["result"] = r.value;
			} else {
				Dictionary e;
				e["code"] = r.error_code;
				e["message"] = r.error_message;
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
			return op_error("PERMISSION", msg);
		}
		ClassDBSingleton *cdb = ClassDBSingleton::get_singleton();
		if (!cdb->can_instantiate(cls)) {
			return op_error("NOT_INSTANTIABLE", "cannot instantiate: " + cls);
		}
		// NB: instantiate() returns a Variant — for RefCounted classes the
		// ONLY valid lifetime is that Variant's ref. Casting it straight to
		// Object* loses the engine binding (observed: oid=0 wrapper that
		// crashes on any engine call).
		Variant created = cdb->instantiate(cls);
		Object *obj = created.get_type() == Variant::OBJECT
				? created.get_validated_object() : nullptr;
		if (obj == nullptr) {
			return op_error("CREATE_FAILED", "instantiate returned null: " + cls);
		}
		// RefCounted results keep the Variant ref itself (correct refcount
		// semantics); plain Objects (nodes) are tracked for cleanup if
		// never adopted into the tree.
		if (cdb->is_parent_class(cls, "RefCounted")) {
			retained_rc.push_back(Ref<RefCounted>(created));
		} else {
			retained_nodes.push_back(created);
		}
		uint64_t oid = (uint64_t)obj->get_instance_id();
		journal(tx.get(), simple_op("ref", oid), simple_op("free", oid));
		uint64_t hid = mint_handle(obj);
		Dictionary wire;
		wire["__godotHandle"] = "h" + String::num_uint64(hid);
		return op_value(wire);
	}

	// Handle-targeted ops
	Object *target = resolve_handle(handles, op.target);
	if (target == nullptr) {
		return op_error("INVALID_HANDLE", "handle expired or unknown");
	}

	// Signal wiring (spec godot-editor-bridge): connect/disconnect need a
	// Callable, which JS can't express — the bridge builds it here.
	// op.member = signal name; args = [to_handle, method, flags?, binds?].
	if (kind == "connect" || kind == "disconnect") {
		StringName sig = StringName(String(op.member.c_str()));
		Object *to = nullptr;
		if (op.args.size() > 0) {
			Variant tov = unwrap_arg(handles, op.args[0]);
			to = tov.get_type() == Variant::OBJECT ? (Object *)tov : nullptr;
		}
		if (to == nullptr) {
			return op_error("BAD_TARGET", String(kind) + " needs a target node handle as arg0");
		}
		StringName method = op.args.size() > 1
				? StringName(String(op.args[1])) : StringName();
		Callable cb(to, method);
		if (kind == "connect") {
			if (!target->has_signal(sig)) {
				return op_error("BAD_SIGNAL", "no such signal on " + target->get_class() + ": " + String(sig));
			}
			int flags = op.args.size() > 2 ? (int)(int64_t)op.args[2] : 0;
			if (op.args.size() > 3) {
				Array binds = op.args[3];
				for (int b = 0; b < binds.size(); b++) {
					binds[b] = unwrap_arg(handles, binds[b]);
				}
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
		if (!member_authorized(index, target, prop)) {
			return op_error("PERMISSION", member_denied_msg(index, target, "property", prop));
		}
		Variant v = target->get(prop);
		return op_value(to_wire(v));
	}

	if (kind == "set") {
		String prop = op.member.c_str();
		if (!member_authorized(index, target, prop)) {
			return op_error("PERMISSION", member_denied_msg(index, target, "property", prop));
		}
		Variant arg = op.args.size() > 0 ? unwrap_arg(handles, op.args[0]) : Variant();
		uint64_t oid = (uint64_t)target->get_instance_id();
		Variant prior = target->get(prop);
		target->set(prop, arg);
		Dictionary f = simple_op("set", oid), i = simple_op("set", oid);
		f["prop"] = prop; f["val"] = arg;
		i["prop"] = prop; i["val"] = prior;
		journal(tx.get(), f, i);
		return op_value(Variant());
	}

	if (kind == "call") {
		String method = op.member.c_str();
		if (!member_authorized(index, target, method)) {
			return op_error("PERMISSION", member_denied_msg(index, target, "method", method));
		}
		// Journal-or-reject (spec editor-transactions): a mutating call we
		// cannot invert is refused at dispatch instead of applied unjournaled.
		if (op.write) {
			static const char *journaled[] = {
					"add_child", "remove_child", "set_owner", "reparent", "set_name",
					"look_at", "look_at_from_position"};
			bool invertible = false;
			for (const char *j : journaled) {
				if (method == j) {
					invertible = true;
				}
			}
			if (!invertible) {
				return op_error("NO_UNDO", "call has no journaled inverse: " + method);
			}
		}
		Array args;
		for (int i = 0; i < op.args.size(); i++) {
			args.push_back(unwrap_arg(handles, op.args[i]));
		}
		uint64_t oid = (uint64_t)target->get_instance_id();

		// Capture prior state for journalable calls before mutating.
		Object *child = args.size() > 0 && args[0].get_type() == Variant::OBJECT
				? (Object *)args[0] : nullptr;
		uint64_t child_oid = child ? (uint64_t)child->get_instance_id() : 0;
		Node *child_node = child ? Object::cast_to<Node>(child) : nullptr;
		Object *prior_owner = (method == "set_owner" && child_node)
				? (Object *)child_node->get_owner() : nullptr;
		uint64_t prior_owner_oid = prior_owner ? (uint64_t)prior_owner->get_instance_id() : 0;
		Node *target_node = Object::cast_to<Node>(target);
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
		Variant prior_tf;
		if (method == "look_at" || method == "look_at_from_position") {
			prior_tf = target->get("global_transform");
		}

		Variant v = target->callv(StringName(method), args);

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
		} else if (method == "set_owner" && child_node) {
			Dictionary f = simple_op("call", oid), i = simple_op("call", oid);
			f["m"] = "set_owner"; f["a0"] = args.size() > 1 ? args[1] : Variant();
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
		} else if ((method == "look_at" || method == "look_at_from_position") &&
				prior_tf.get_type() == Variant::TRANSFORM3D) {
			Dictionary f = simple_op("callv", oid), i = simple_op("set", oid);
			f["m"] = method; f["aa"] = args;
			i["prop"] = "global_transform"; i["val"] = prior_tf;
			journal(tx.get(), f, i);
		}
		BridgeResult r = op_value(to_wire(v));
		r.warnings = warns;
		return r;
	}

	return op_error("BAD_OP", "unknown op kind");
}

void Bridge::drain(double budget_ms) {
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
				Dictionary d;
				d["granted"] = true;
				d["tier"] = tier;
				pb->result = op_value(d);
			} else if (policy == "deny") {
				Dictionary d;
				d["granted"] = false;
				d["tier"] = tier;
				d["policy"] = "deny";
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
			if (pb->result.ok && pb->op.write && pb->tx) {
				Dictionary ch;
				ch["op"] = pb->op.kind.c_str();
				ch["member"] = pb->op.member.c_str();
				pb->tx->changes.push_back(ch);
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
	retained_rc.clear();
	for (int i = 0; i < retained_nodes.size(); i++) {
		Variant v = retained_nodes[i];
		if (v.get_type() == Variant::OBJECT) {
			Object *o = v;
			if (o && ObjectDB::get_instance(o->get_instance_id())) {
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
