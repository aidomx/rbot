root: .

foreground: false # SIGINT dikendalikan rbot, lalu diteruskan ke child build

clean:
  - build: false
  - compdb: true

sources:
  - src

flags:
  - Wall
  - Wextra
  - O2

std: gnu11

headers:
  - include
  - I.
  - build # build/version.h & build/embedded.h (dihasilkan saat build)

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
