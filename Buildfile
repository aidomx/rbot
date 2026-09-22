root: .

clean:
  - buildDir: false
  - compileCommands: false

version: "0.1.1"

sources:
  - src

flags:
  - Wall
  - Wextra

std: gnu11

headers:
  - include
  - I.

compiler:
  - gcc
  - clang

progress:
  bar: true
  error: always

output:
  - binaryName: rbot
  - binaryDir: bin
  - buildDir: build
  - compileCommands: auto # compile_commands.json
