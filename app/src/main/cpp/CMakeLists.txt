cmake_minimum_required(VERSION 3.22.1)
project("EndfieldOverlay" CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -O3
    -ffast-math
    -fvisibility=hidden
    -DNDEBUG
)

# ─── Saba (MMD loader) ────────────────────────────────────────────────────────
# Pulled as a Git submodule into app/src/main/cpp/saba/
set(SABA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/saba")

# All desktop/test features must be OFF before add_subdirectory
set(SABA_ENABLE_TEST          OFF CACHE BOOL "" FORCE)
set(SABA_ENABLE_VIEWER        OFF CACHE BOOL "" FORCE)
set(SABA_INSTALL              OFF CACHE BOOL "" FORCE)
set(SABA_USE_GLSLANG          OFF CACHE BOOL "" FORCE)
set(SABA_FORCE_GLFW_BUILD     OFF CACHE BOOL "" FORCE)
set(SABA_ENABLE_EXAMPLE_VULKAN OFF CACHE BOOL "" FORCE)

# Silence any residual find_package(OpenGL) / find_package(GLFW) calls that
# may survive in transitive CMake scripts within saba.
set(OpenGL_FOUND  TRUE)
set(OPENGL_FOUND  TRUE)
set(OPENGL_LIBRARIES "")
set(OPENGL_INCLUDE_DIR "")
set(glfw3_FOUND   TRUE)
set(GLFW_LIBRARIES "")
set(GLFW_INCLUDE_DIRS "")

add_subdirectory(${SABA_DIR} ${CMAKE_BINARY_DIR}/saba EXCLUDE_FROM_ALL)

# ─── Application native library ───────────────────────────────────────────────
set(ENDFIELD_SOURCES
    native-lib.cpp
    renderer/MMDRenderer.cpp
    renderer/ShaderProgram.cpp
    motion/VMDManager.cpp
)

add_library(endfield-native SHARED ${ENDFIELD_SOURCES})

# Note: Saba's src/CMakeLists.txt names the target "Saba" (capital S).
# Bullet headers are exposed transitively via Saba's PUBLIC include dirs.
target_include_directories(endfield-native PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/renderer
    ${CMAKE_CURRENT_SOURCE_DIR}/motion
    ${SABA_DIR}/src
    ${SABA_DIR}/external/glm/include
    ${SABA_DIR}/external/stb/include
)

find_library(log-lib     log)
find_library(android-lib android)
find_library(egl-lib     EGL)
find_library(gles3-lib   GLESv3)

# Link against "Saba" (capital S) — that is the CMake target name defined in
# saba/src/CMakeLists.txt. BulletDynamics etc. propagate transitively.
target_link_libraries(endfield-native
    Saba
    ${log-lib}
    ${android-lib}
    ${egl-lib}
    ${gles3-lib}
    z
)

target_compile_definitions(endfield-native PRIVATE
    ENDFIELD_VERSION_MAJOR=1
    ENDFIELD_VERSION_MINOR=0
    TARGET_FPS_NORMAL=60
    TARGET_FPS_POWER_SAVE=30
)
