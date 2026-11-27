#ifndef GODOT_CM_PERFORMANCE_HPP
#define GODOT_CM_PERFORMANCE_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot_cm {

enum class PerfMetric {
	REFRESH_CALLS, GLOBAL_DISCOVERY, SCENE_NODES, SCRIPT_REFLECTIONS, CATALOG_PUBLICATIONS,
	CACHE_HITS, CACHE_MISSES, JSON_ENCODINGS, JSON_BYTES, PAGE_ITEMS, VALUE_RETAINS, VALUE_RELEASES,
	REQUEST_NS, CONTEXT_NS, QUEUE_WAIT_NS, CATALOG_NS, MAINTENANCE_NS, VALIDATION_NS,
	CONVERSION_NS, NATIVE_NS, ENCODE_NS, PAGE_NS, FINAL_RESPONSE_NS, PUMP_NS, DICTIONARY_SKIP_NS,
	STARTUP_NS, JSON_NS, PUMP_MAX_NS, JS_DECODE_NS, MAINTENANCE_DISCOVERY, MAINTENANCE_REFLECTIONS, CATALOG_LIVE, CATALOG_PEAK, SCRIPT_CACHE_ENTRIES, WORKER_RESULT_REUSE, WORKER_RESULT_PARSE, COUNT
};

class Performance {
	inline static std::array<std::atomic<uint64_t>, (size_t)PerfMetric::COUNT> metrics{};
	inline static std::array<std::atomic<uint64_t>, (size_t)PerfMetric::COUNT> exclusive{};
	inline static std::mutex report_mutex;

public:
	inline static thread_local bool maintenance = false;
	struct Snapshot {
		std::array<uint64_t, (size_t)PerfMetric::COUNT> values{}, exclusive{};
	};
	static bool enabled() {
		static const bool active = [] { const char *value = std::getenv("GODOT_CM_PROFILE"); return value && std::strcmp(value, "1") == 0; }();
		return active;
	}
	static uint64_t now() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
	static void add(PerfMetric metric, uint64_t amount = 1) {
		if (!enabled()) return;
		metrics[(size_t)metric].fetch_add(amount, std::memory_order_relaxed);
		if (maintenance && metric == PerfMetric::GLOBAL_DISCOVERY) metrics[(size_t)PerfMetric::MAINTENANCE_DISCOVERY].fetch_add(amount, std::memory_order_relaxed);
		if (maintenance && metric == PerfMetric::SCRIPT_REFLECTIONS) metrics[(size_t)PerfMetric::MAINTENANCE_REFLECTIONS].fetch_add(amount, std::memory_order_relaxed);
	}
	static void maximum(PerfMetric metric, uint64_t value) {
		if (!enabled()) return;
		auto &counter = metrics[(size_t)metric];
		uint64_t prior = counter.load(std::memory_order_relaxed);
		while (value > prior && !counter.compare_exchange_weak(prior, value, std::memory_order_relaxed)) {}
	}
	static void set(PerfMetric metric, uint64_t value) {
		if (enabled()) metrics[(size_t)metric].store(value, std::memory_order_relaxed);
	}
	static void catalog_created() {
		if (enabled()) maximum(PerfMetric::CATALOG_PEAK, metrics[(size_t)PerfMetric::CATALOG_LIVE].fetch_add(1, std::memory_order_relaxed) + 1);
	}
	static void catalog_destroyed() {
		if (enabled()) metrics[(size_t)PerfMetric::CATALOG_LIVE].fetch_sub(1, std::memory_order_relaxed);
	}
	static void add_exclusive(PerfMetric metric, uint64_t amount) {
		if (enabled()) exclusive[(size_t)metric].fetch_add(amount, std::memory_order_relaxed);
	}
	static Snapshot snapshot() {
		Snapshot result{};
		if (enabled()) for (size_t i = 0; i < result.values.size(); i++) {
			result.values[i] = metrics[i].load(std::memory_order_relaxed);
			result.exclusive[i] = exclusive[i].load(std::memory_order_relaxed);
		}
		return result;
	}
	static void report(const char *kind, const Snapshot &before) {
		if (!enabled()) return;
		static const char *names[] = {
			"refresh_calls", "global_discovery", "scene_nodes", "script_reflections", "catalog_publications",
			"cache_hits", "cache_misses", "json_encodings", "json_bytes", "page_items", "value_retains", "value_releases",
			"request_inclusive_ns", "context_inclusive_ns", "queue_wait_ns", "catalog_inclusive_ns", "maintenance_inclusive_ns", "validation_inclusive_ns",
			"conversion_inclusive_ns", "native_inclusive_ns", "encode_inclusive_ns", "page_inclusive_ns", "final_response_inclusive_ns", "pump_inclusive_ns", "dictionary_skip_ns",
			"startup_inclusive_ns", "json_inclusive_ns", "process_pump_max_ns", "js_passive_decode_inclusive_ns", "maintenance_global_discovery", "maintenance_script_reflections", "catalog_live", "catalog_peak", "script_cache_entries", "worker_result_reuse", "worker_result_parse"
		};
		static_assert(sizeof(names) / sizeof(names[0]) == (size_t)PerfMetric::COUNT);
		Snapshot after = snapshot();
		godot::Dictionary data, values;
		data["schema_version"] = 1;
		data["kind"] = kind;
		data["timing_semantics"] = "inclusive; cross-thread intervals can overlap";
		for (size_t i = 0; i < after.values.size(); i++) {
			values[names[i]] = (int64_t)(i == (size_t)PerfMetric::PUMP_MAX_NS || i == (size_t)PerfMetric::CATALOG_LIVE || i == (size_t)PerfMetric::CATALOG_PEAK || i == (size_t)PerfMetric::SCRIPT_CACHE_ENTRIES ? after.values[i] : after.values[i] - before.values[i]);
			godot::String name = names[i];
			if (name.ends_with("_inclusive_ns")) values[name.replace("_inclusive_ns", "_exclusive_ns")] = (int64_t)(after.exclusive[i] - before.exclusive[i]);
		}
		data["metrics"] = values;
		std::lock_guard<std::mutex> lock(report_mutex);
		godot::UtilityFunctions::print("[godot-cm-perf] ", godot::JSON::stringify(data));
	}
	static godot::String json(const godot::Variant &value);
};

class PerfMaintenance {
	bool previous;
public:
	PerfMaintenance() : previous(Performance::maintenance) { Performance::maintenance = true; }
	~PerfMaintenance() { Performance::maintenance = previous; }
};

class PerfScope {
	inline static thread_local PerfScope *current = nullptr;
	PerfScope *parent = nullptr;
	PerfMetric metric;
	uint64_t start, children = 0;
public:
	explicit PerfScope(PerfMetric metric, bool active = true) : metric(metric), start(active && Performance::enabled() ? Performance::now() : 0) {
		if (start) { parent = current; current = this; }
	}
	PerfScope(const PerfScope &) = delete;
	PerfScope &operator=(const PerfScope &) = delete;
	void stop() {
		if (!start) return;
		uint64_t elapsed = Performance::now() - start;
		Performance::add(metric, elapsed);
		Performance::add_exclusive(metric, elapsed > children ? elapsed - children : 0);
		if (metric == PerfMetric::PUMP_NS) Performance::maximum(PerfMetric::PUMP_MAX_NS, elapsed);
		if (parent) parent->children += elapsed;
		current = parent;
		start = 0;
	}
	~PerfScope() { stop(); }
};

inline godot::String Performance::json(const godot::Variant &value) {
	PerfScope cost(PerfMetric::JSON_NS);
	godot::String text = godot::JSON::stringify(value);
	if (enabled()) {
		add(PerfMetric::JSON_ENCODINGS);
		add(PerfMetric::JSON_BYTES, text.utf8().length());
	}
	return text;
}

class PerfReport {
	const char *kind;
	Performance::Snapshot before;
public:
	explicit PerfReport(const char *kind) : kind(kind), before(Performance::snapshot()) {}
	~PerfReport() { Performance::report(kind, before); }
};

}

#endif
