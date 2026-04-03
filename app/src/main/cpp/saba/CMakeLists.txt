cmake_minimum_required(VERSION 3.22.1)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

project(saba)

# ─── Force all desktop / test options OFF ────────────────────────────────────
set(SABA_ENABLE_VIEWER        OFF CACHE BOOL "" FORCE)
set(SABA_ENABLE_TEST          OFF CACHE BOOL "" FORCE)
set(SABA_USE_GLSLANG          OFF CACHE BOOL "" FORCE)
set(SABA_INSTALL              OFF CACHE BOOL "" FORCE)
set(SABA_FORCE_GLFW_BUILD     OFF CACHE BOOL "" FORCE)
set(SABA_ENABLE_EXAMPLE_VULKAN OFF CACHE BOOL "" FORCE)

# ─── Do NOT call find_package(OpenGL) or find_package(Bullet) ────────────────
# Android uses system GLESv3/EGL; Bullet3 is built via FetchContent below.

# ─── Bullet3 via FetchContent ─────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    bullet3
    GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
    GIT_TAG        3.25
    GIT_SHALLOW    TRUE
)
set(BUILD_BULLET2_DEMOS     OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET3           OFF CACHE BOOL "" FORCE)
set(BUILD_CLSOCKET          OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS         OFF CACHE BOOL "" FORCE)
set(BUILD_EGL               OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS            OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS     OFF CACHE BOOL "" FORCE)
set(BUILD_PYBULLET          OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS        OFF CACHE BOOL "" FORCE)
set(USE_GLUT                OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS            OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(bullet3)

set(BULLET_INCLUDE_DIRS "${bullet3_SOURCE_DIR}/src")
set(BULLET_LIBRARIES BulletDynamics BulletCollision LinearMath)

# ─── C++ standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Include paths ────────────────────────────────────────────────────────────
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/glm/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/stb/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/spdlog/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyobjloader/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/json/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyxfileloader/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/external/tinyddsloader/include)
include_directories(${BULLET_INCLUDE_DIRS})

# ─── External deps bundled with saba ──────────────────────────────────────────
add_subdirectory(external)

# ─── Saba static library (src/ only) ─────────────────────────────────────────
add_subdirectory(src)

# Expose Bullet headers to Saba's own sources and to consumers
target_include_directories(Saba PUBLIC ${BULLET_INCLUDE_DIRS})
target_link_libraries(Saba PRIVATE ${BULLET_LIBRARIES})

# Android graphics libs instead of desktop OpenGL
find_library(GLES3_LIB GLESv3)
find_library(EGL_LIB   EGL)
if(GLES3_LIB AND EGL_LIB)
    target_link_libraries(Saba PUBLIC ${GLES3_LIB} ${EGL_LIB})
endif()
