#include "spike_check.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace godot_cm {

void SpikeRunner::_bind_methods() {
	ClassDB::bind_method(D_METHOD("run_checks"), &SpikeRunner::run_checks);
	ClassDB::bind_method(D_METHOD("bench_dispatch", "ops"), &SpikeRunner::bench_dispatch);
	ClassDB::bind_method(D_METHOD("drain_queue"), &SpikeRunner::drain_queue);
}

static bool report(const String &name, bool ok) {
	UtilityFunctions::print(String("[godot-cm spike] ") + (ok ? "PASS " : "FAIL ") + name);
	return ok;
}

void SpikeRunner::run_checks() {
	if (done) {
		return;
	}
	done = true;

	bool ok = true;

	ClassDBSingleton *class_db = ClassDBSingleton::get_singleton();
	ok &= report("ClassDB has EditorInterface", class_db && class_db->class_exists("EditorInterface"));
	ok &= report("ClassDB has EditorUndoRedoManager", class_db && class_db->class_exists("EditorUndoRedoManager"));

	EditorInterface *ei = EditorInterface::get_singleton();
	ok &= report("EditorInterface singleton", ei != nullptr);
	if (ei != nullptr) {
		EditorUndoRedoManager *undo = ei->get_editor_undo_redo();
		ok &= report("EditorUndoRedoManager instance", undo != nullptr);

		Node *scene_root = ei->get_edited_scene_root();
		UtilityFunctions::print(String("[godot-cm spike] edited scene root: ") +
				(scene_root ? String(scene_root->get_name()) : String("<none>")));
	}

	UtilityFunctions::print(String("[godot-cm spike] result: ") + (ok ? "ALL PASS" : "FAILURES"));
}

// --- 1.4: worker->main->worker round-trip benchmark -------------------------
// Prototype of the async bridge: a worker thread posts ops into a queue that
// the main thread drains under a per-frame budget (~2 ms), matching D3/D9.

namespace {
std::mutex q_mutex;
std::condition_variable q_cv;
std::deque<int> in_queue;
int acked = 0;
constexpr double FRAME_BUDGET_US = 2000.0;
} // namespace

void SpikeRunner::drain_queue() {
	auto frame_start = std::chrono::steady_clock::now();
	int processed_this_frame = 0;
	{
		std::lock_guard<std::mutex> lock(q_mutex);
		while (!in_queue.empty()) {
			in_queue.pop_front();
			acked++;
			processed_this_frame++;
			double elapsed_us = std::chrono::duration<double, std::micro>(
					std::chrono::steady_clock::now() - frame_start)
					.count();
			if (elapsed_us >= FRAME_BUDGET_US) {
				break;
			}
		}
	}
	q_cv.notify_all();
	drain_calls++;
	if (processed_this_frame > 0 && acked >= ops_expected) {
		UtilityFunctions::print(String("[godot-cm bench] frames used: ") +
				String::num_int64(drain_calls));
	}
}

void SpikeRunner::bench_dispatch(int64_t p_ops) {
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree == nullptr) {
		UtilityFunctions::print("[godot-cm bench] FAIL no SceneTree main loop");
		return;
	}
	if (draining) {
		return;
	}
	draining = true;
	ops_expected = p_ops;
	UtilityFunctions::print(String("[godot-cm bench] starting ops=") + String::num_int64(p_ops));

	Callable drain_cb(this, "drain_queue");
	Error err = tree->connect("process_frame", drain_cb);
	UtilityFunctions::print(String("[godot-cm bench] connect result=") + String::num_int64(err));

	Ref<SpikeRunner> self(this); // keep alive across the bench
	std::thread worker([p_ops]() {
		double total_us = 0.0;
		double max_us = 0.0;
		for (int i = 0; i < p_ops; i++) {
			auto t0 = std::chrono::steady_clock::now();
			{
				std::lock_guard<std::mutex> lock(q_mutex);
				in_queue.push_back(i);
			}
			std::unique_lock<std::mutex> lock(q_mutex);
			q_cv.wait(lock, [&] { return acked > i; });
			double us = std::chrono::duration<double, std::micro>(
					std::chrono::steady_clock::now() - t0)
					.count();
			total_us += us;
			if (us > max_us) {
				max_us = us;
			}
		}
		UtilityFunctions::print(String("[godot-cm bench] ops=") + String::num_int64(p_ops) +
				" avg_rt=" + String::num(total_us / p_ops, 1) + "us" +
				" max_rt=" + String::num(max_us, 1) + "us");
	});
	worker.detach();
}

} // namespace godot_cm
