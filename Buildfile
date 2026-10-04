use project

clean as c
library as lib
output as o

root = .

# SIGINT dikendalikan rbot, lalu diteruskan ke child build
foreground = false 

c.build = false
c.compdb = false

sources = src

# MMD: dep file <obj>.d; MP: phony target (GNU/Clang)
flags = Wall, Wextra, O2, MMD, MP 
lib.windows = ws2_32
std = gnu11

# build: version.h & embedded.h (dihasilkan saat build)
headers = include, I., build 

compiler = cl, gcc, clang

progress.bar = true
progress.error = always

o.binaryName = rbot
o.binaryDir = bin
o.buildDir = build
# auto generate compile_commands.json
o.compileCommands = auto 

pack.name = rbot
pack.version = 0.2.0
pack.output = dist/{name}-v{version}.tar.gz

pack.files = bin/rbot:bin/rbot, README.md:share/rbot/README.md, LICENSE:share/rbot/LICENSE

pack.format = deb
pack.checksum = sha256

pack.deb.install_prefix = /usr/local
pack.deb.description = "Rbot C build tool"
pack.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
pack.deb.description = "A simple builder for you"
pack.deb.architecture = arm64
