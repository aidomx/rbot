cmake_minimum_required(VERSION 3.16)
project(rbot C)

# Mirror Buildfile: std: gnu11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ON -> -std=gnu11 (bukan -std=c11)

# Mirror Buildfile: output.compileCommands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Put generated binaries next to the configured build tree instead of the source tree.
# This keeps the output consistent across Unix Makefiles and multi-config generators
# such as Visual Studio on GitHub-hosted Windows runners.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(config IN ITEMS RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
endforeach()

# Mirror Buildfile: sources: - src (scan rekursif semua .c di bawah src/)
file(GLOB_RECURSE RBOT_SOURCES CONFIGURE_DEPENDS src/*.c)

# Mirror Buildfile: output.binaryName: rbot
add_executable(rbot ${RBOT_SOURCES})

# Mirror Buildfile: headers: - include, - I.
target_include_directories(rbot PRIVATE
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}
)

# Mirror Buildfile: flags: - Wall, - Wextra
if(MSVC)
  target_compile_options(rbot PRIVATE /W4)
else()
  target_compile_options(rbot PRIVATE -Wall -Wextra)
endif()

# Windows: rbot butuh shell32 untuk fsRemoveTree (SHFileOperationA).
# MinGW/MSYS2 sudah link shell32 bawaan; MSVC perlu eksplisit.
if(WIN32)
  target_link_libraries(rbot PRIVATE shell32)
endif()
