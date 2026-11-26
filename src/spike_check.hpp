#ifndef GODOT_CM_SPIKE_CHECK_HPP
#define GODOT_CM_SPIKE_CHECK_HPP

#include <godot_cpp/classes/ref_counted.hpp>

namespace godot_cm {

// Task 1.3/1.4 spike: verify editor-only APIs are reachable from a
// GDExtension on Godot 4.7.2, and measure worker->main->worker round-trip
// latency for the async bridge (invoked from the EditorPlugin context,
// once the editor is fully up).
class SpikeRunner : public godot::RefCounted {
	GDCLASS(SpikeRunner, godot::RefCounted)

	bool done = false;
	bool draining = false;
	int ops_done = 0;
	int64_t drain_calls = 0;
	int64_t ops_expected = 0;

	void drain_queue();

protected:
	static void _bind_methods();

public:
	void run_checks();
	void bench_dispatch(int64_t p_ops);
};

} // namespace godot_cm

#endif
