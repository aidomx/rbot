use alias

clean as c
output as o

root = .

# SIGINT dikendalikan rbot, lalu diteruskan ke child build
foreground = false 

c.build = false
c.compdb = true

sources = src

# MMD: dep file <obj>.d; MP: phony target (GNU/Clang)
flags = Wall, Wextra, O2, MMD, MP 

std = gnu11

# build: version.h & embedded.h (dihasilkan saat build)
headers = include, I., build 

compiler = gcc, clang

progress.bar = true
progress.error = always

o.binaryName = rbot
o.binaryDir = bin
o.buildDir = build
# auto generate compile_commands.json
o.compileCommands = auto 
