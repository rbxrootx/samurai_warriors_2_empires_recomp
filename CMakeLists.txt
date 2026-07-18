# samurai_warriors_2_empires - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(samurai_warriors_2_empires LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(SAMURAIWARRIORS2EMPIRES_SOURCES
    src/main.cpp
    src/hooks/function_map.cpp
    src/hooks/runtime_hooks.cpp
    src/native_renderer/sw2e_native_gpu_replay.cpp
    src/native_renderer/sw2e_native_renderer.cpp
)

if(WIN32)
    add_executable(samurai_warriors_2_empires WIN32 ${SAMURAIWARRIORS2EMPIRES_SOURCES})
else()
    add_executable(samurai_warriors_2_empires ${SAMURAIWARRIORS2EMPIRES_SOURCES})
endif()

target_include_directories(samurai_warriors_2_empires PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(REXSDK_DIR)
    target_include_directories(samurai_warriors_2_empires PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/rexglue-sdk/include
        ${REXSDK_DIR}/thirdparty/imgui
    )
endif()

rexglue_setup_target(samurai_warriors_2_empires)

if(WIN32)
    target_link_libraries(samurai_warriors_2_empires PRIVATE d3d11 dxgi d3dcompiler)
endif()
