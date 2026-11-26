#ifndef GODOT_CM_MCP_SERVICE_HPP
#define GODOT_CM_MCP_SERVICE_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

#include "bridge.hpp"

namespace httplib {
class Server;
}

namespace godot_cm {

class ApiIndex;
class Bridge;
class BridgePump;
class JsWorker;

// Lifecycle + transport owner for the local MCP server.
// Driven by the EditorPlugin: start() on _enter_tree, stop() on _exit_tree.
class McpService : public godot::RefCounted {
	GDCLASS(McpService, godot::RefCounted)

	std::unique_ptr<httplib::Server> server;
	std::unique_ptr<JsWorker> js;
	std::unique_ptr<ApiIndex> api_index;
	std::unique_ptr<Bridge> bridge;
	godot::Ref<BridgePump> pump;
	std::thread *server_thread = nullptr;
	std::atomic<bool> running{false};
	int port = 0;
	std::string session_token;
	bool session_initialized = false;
	mutable std::mutex session_mutex;
	std::unordered_map<std::string, std::shared_ptr<std::atomic<bool>>> pending_execs;

	godot::Variant dispatch_rpc(const godot::Dictionary &request);
	godot::Variant handle_initialize(const godot::Variant &id, const godot::Dictionary &params);
	godot::Variant handle_tools_list(const godot::Variant &id);
	godot::Variant handle_tools_call(const godot::Variant &id, const godot::Dictionary &params);

protected:
	static void _bind_methods();

public:
	bool start(int p_port);
	void stop();
	bool is_running() const;
	int get_port() const;
	godot::String get_session_token() const;

	// Called from plugin.gd when the user answers the tier confirmation
	// dialog: unlocks the tier (if granted) and settles parked ops.
	void respond_tier_request(const godot::String &tier, bool granted);
	godot::Array get_unlocked_tiers() const;
	void set_auto_grant(bool enabled);
	godot::Array get_tiers() const;
	godot::Array get_tier_classes(const godot::String &tier) const;
	godot::String get_tier_policy(const godot::String &tier) const;
	void set_tier_policy(const godot::String &tier, const godot::String &policy);

	McpService() = default;
	~McpService() override;
};

} // namespace godot_cm

#endif
