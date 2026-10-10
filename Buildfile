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
lib.windows = shell32
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

release.name = rbot
release.version = 0.2.3
release.output = dist/{name}-v{version}.tar.gz

release.files = bin/rbot:bin/rbot, README.md:share/rbot/README.md, LICENSE:share/rbot/LICENSE, src/cmd.txt:share/rbot/cmd.txt

release.format = deb
release.checksum = sha256

release.deb.install_prefix = /usr/local
release.deb.maintainer = "Aidomx <aidomxdev@gmail.com>"
release.deb.description = "A simple builder for you"
