#include "api_index.hpp"
#include "bridge.hpp"
#include "js_worker.hpp"
#include "mcp_service.hpp"
#include "spike_check.hpp"

#include "third_party/httplib.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {

void initialize_godot_cm(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
	ClassDB::register_class<godot_cm::BridgePump>();
	ClassDB::register_class<godot_cm::McpService>();
	ClassDB::register_class<godot_cm::SpikeRunner>();
}

void uninitialize_godot_cm(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_EDITOR) {
		return;
	}
}

} // namespace

extern "C" {

GDExtensionBool GDE_EXPORT godot_cm_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_godot_cm);
	init_obj.register_terminator(uninitialize_godot_cm);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_EDITOR);
	return init_obj.init();
}
}
