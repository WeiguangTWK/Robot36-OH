cmake_minimum_required(VERSION 3.5.0)
project(Robot36NativeStage0)

set(NATIVE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

if(DEFINED PACKAGE_FIND_FILE)
    include(${PACKAGE_FIND_FILE})
endif()

include_directories(
    ${NATIVE_ROOT}
    ${NATIVE_ROOT}/common
    ${NATIVE_ROOT}/plugin
    ${NATIVE_ROOT}/renderer
)

add_library(entry SHARED
    napi_init.cpp
    core/sstv_core.cpp
    plugin/plugin_manager.cpp
    runtime/decoder_runtime.cpp
    renderer/robot36_renderer.cpp
)

target_link_libraries(entry PUBLIC
    libhilog_ndk.z.so
    libace_napi.z.so
    libace_ndk.z.so
    libnative_window.so
    libnative_drawing.so
    libnative_vsync.so
)
