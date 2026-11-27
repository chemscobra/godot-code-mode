#ifndef GODOT_CM_JS_WORKER_HPP
#define GODOT_CM_JS_WORKER_HPP

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace godot_cm {

class ApiIndex;
class Bridge;
struct TxState;

struct ExecRequest {
	std::string code;
	std::string mode = "read"; // "read" | "write"
	bool search_only = false;  // search tool: api index only, no editor bindings
	bool verbose = false;      // report per-op change detail instead of a summary
	int64_t timeout_ms = 0;    // <=0: mode default (5s read / 60s write)
	std::shared_ptr<std::atomic<bool>> cancel;  // set by notifications/cancelled
	std::shared_ptr<TxState> tx;                // per-request transaction state
};

// Bounded JS execution on a dedicated worker thread (design D2/D3).
// Fresh QuickJS runtime+context per execution; no std/os/native modules.
// Result is the structured dict from the spec: ok/result/diagnostics/changes/truncated.
class JsWorker {
	struct Job {
		ExecRequest req;
		std::shared_ptr<godot::Dictionary> result;
		std::shared_ptr<std::condition_variable> done;
	};

	ApiIndex *api_index = nullptr;
	Bridge *bridge = nullptr;
	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::shared_ptr<Job>> queue;
	std::atomic<bool> quit{false};

	void run();
	godot::Dictionary execute_one(const ExecRequest &req);

public:
	// Blocks the caller until the job completes or the worker dies.
	godot::Dictionary submit(const ExecRequest &req);

	void start(ApiIndex *p_index = nullptr, Bridge *p_bridge = nullptr);
	void stop();
	~JsWorker();
};

} // namespace godot_cm

#endif
