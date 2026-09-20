cmake_minimum_required(VERSION 3.16)
project(rbot C)

# Mirror Buildfile: std: gnu11
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # ON -> -std=gnu11 (bukan -std=c11)

# Mirror Buildfile: output.compileCommands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Mirror Buildfile: output.binaryDir: bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)

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
