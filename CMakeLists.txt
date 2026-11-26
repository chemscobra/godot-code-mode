cmake_minimum_required(VERSION 3.20)
project(godot_cm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build bindings against the extension API dumped from the local Godot 4.7.2.
set(GODOTCPP_CUSTOM_API_FILE "${CMAKE_SOURCE_DIR}/api/extension_api.json" CACHE FILEPATH "")
add_subdirectory(godot-cpp)

add_library(quickjs_ng STATIC
	quickjs/dtoa.c
	quickjs/libregexp.c
	quickjs/libunicode.c
	quickjs/quickjs.c
)
target_compile_definitions(quickjs_ng PRIVATE _GNU_SOURCE)
target_include_directories(quickjs_ng PUBLIC quickjs)

add_library(godot_cm SHARED
	src/api_index.cpp
	src/bridge.cpp
	src/value_codec.cpp
	src/js_worker.cpp
	src/mcp_service.cpp
	src/register_types.cpp
	src/spike_check.cpp
)

target_include_directories(godot_cm PRIVATE src)
target_link_libraries(godot_cm PRIVATE godot::cpp quickjs_ng)
if(WIN32)
	target_link_libraries(godot_cm PRIVATE ws2_32 winpthread)
endif()

if(WIN32)
	set(GODOT_CM_LIB_NAME "godot_cm.windows.editor.x86_64")
	set(GODOT_CM_PREFIX "")
elseif(APPLE)
	set(GODOT_CM_LIB_NAME "godot_cm.macos.editor")
	set(GODOT_CM_PREFIX "lib")
else()
	set(GODOT_CM_LIB_NAME "godot_cm.linux.editor.x86_64")
	set(GODOT_CM_PREFIX "lib")
endif()
set_target_properties(godot_cm PROPERTIES
	OUTPUT_NAME "${GODOT_CM_LIB_NAME}"
	PREFIX "${GODOT_CM_PREFIX}"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/addons/godot_cm/bin"
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/addons/godot_cm/bin"
)
