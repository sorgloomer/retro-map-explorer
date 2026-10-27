# For IDEs only (CLion: syntax highlighting, navigation, inspections). The real build is `node scripts/build.mjs`, which
# runs em++ from the project-local Emscripten SDK; this file mirrors its flags and source discovery.
cmake_minimum_required(VERSION 3.16)
project(retro_map_explorer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# every .cpp under src/cpp, like build.mjs
file(GLOB_RECURSE RME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/cpp/*.cpp")

add_executable(rme ${RME_SOURCES})

# includes are relative to src/cpp (build.mjs: -I src/cpp)
target_include_directories(rme PRIVATE "${CMAKE_SOURCE_DIR}/src/cpp")

# <emscripten.h>, <emscripten/html5.h>, GLES3: from the emsdk sysroot when it is installed
set(EMSDK_SYSROOT "${CMAKE_SOURCE_DIR}/.tools/emsdk/upstream/emscripten/cache/sysroot")
if(EXISTS "${EMSDK_SYSROOT}/include/emscripten.h")
    target_include_directories(rme SYSTEM PRIVATE "${EMSDK_SYSROOT}/include")
else()
    message(WARNING "Emscripten headers not found in ${EMSDK_SYSROOT}: run the build once (see scripts/build.mjs) so the IDE can resolve <emscripten.h>")
endif()

# the host compiler does not define these, but the sources and the Emscripten headers test for them
target_compile_definitions(rme PRIVATE __EMSCRIPTEN__=1)

target_compile_options(rme PRIVATE -Wall -Wno-unused-function)
