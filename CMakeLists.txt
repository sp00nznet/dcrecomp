cmake_minimum_required(VERSION 3.16)
project(dcrecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Compiler flags
if(MSVC)
    add_compile_options(/W3 /fp:fast /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -O2 -ffast-math)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    endif()
endif()

# Include directories
set(DCRECOMP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${DCRECOMP_INCLUDE_DIR})

# ========== Find SDL2 (optional) ==========
find_package(SDL2 QUIET CONFIG)
if(NOT SDL2_FOUND)
    find_package(SDL2 QUIET)
endif()
if(SDL2_FOUND)
    message(STATUS "SDL2 found")
    add_compile_definitions(HAS_SDL2)
else()
    message(STATUS "SDL2 not found - building in headless mode")
endif()

# ========== Find OpenGL + GLEW ==========
find_package(OpenGL QUIET)
find_package(GLEW QUIET CONFIG)
if(OPENGL_FOUND AND GLEW_FOUND)
    message(STATUS "OpenGL + GLEW found")
    add_compile_definitions(HAS_OPENGL)
else()
    message(STATUS "OpenGL/GLEW not found - rendering disabled")
endif()

# ========== dcrecomp core library ==========
add_library(dcrecomp_core STATIC
    src/recompiler/sh4_cpu.c
    src/recompiler/sh4_task.c
    src/hal/dc_hardware.c
    src/hal/dc_bios.c
    src/hal/naomi_io.c
    src/hal/arm7.c
    src/hal/pvr2_ta.c
    src/hal/pvr2_render.c
    src/platform/platform_sdl2.c
)

target_include_directories(dcrecomp_core PUBLIC ${DCRECOMP_INCLUDE_DIR})

if(MSVC)
    target_compile_options(dcrecomp_core PRIVATE /O2 /Zi)
    # dbghelp resolves frame addresses to recompiled function names
    target_link_libraries(dcrecomp_core PUBLIC dbghelp)
    target_link_options(dcrecomp_core PUBLIC /DEBUG)
endif()

# Link SDL2/OpenGL
if(SDL2_FOUND)
    if(TARGET SDL2::SDL2)
        target_link_libraries(dcrecomp_core PUBLIC SDL2::SDL2)
    else()
        target_include_directories(dcrecomp_core PUBLIC ${SDL2_INCLUDE_DIRS})
    endif()
endif()
if(OPENGL_FOUND AND GLEW_FOUND)
    target_link_libraries(dcrecomp_core PUBLIC OpenGL::GL GLEW::GLEW)
endif()

# Export include directory for consuming projects
set(DCRECOMP_INCLUDE_DIR ${DCRECOMP_INCLUDE_DIR} PARENT_SCOPE)

# Print summary
message(STATUS "")
message(STATUS "=== dcrecomp - Dreamcast/Naomi Recompilation Framework ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "SDL2: ${SDL2_FOUND}")
message(STATUS "OpenGL: ${OPENGL_FOUND}")
message(STATUS "")

# Self-check for the ARM7 core: barrel-shifter edge cases, flags, banking, FIQ.
# Built with everything else so a break shows up at once; run with ctest.
add_executable(arm7_selftest tests/arm7_selftest.c)
target_include_directories(arm7_selftest PRIVATE include)
enable_testing()
add_test(NAME arm7 COMMAND arm7_selftest)
