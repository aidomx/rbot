use workspace

# alias singkat
projects.rupamod as mod
projects.rupa as rupa

projects = rupamod, rupa, ruka

compiler = gcc, clang
flags = Wall, Wextra, MMD, MP
headers = include, build, I.
std = gnu11

# B. rupamod — proyek kemasan (archive)
mod.output.binary = false
mod.archive.src = .
mod.archive.pattern = .rp
mod.archive.name = rupa_modules
mod.archive.with.tar = true
mod.archive.with.ext = gz
mod.archive.dir = dist

# A. rupa
rupa.sources = src
rupa.exclude = main.c
rupa.headers = ../ruka/include

rupa.output as rupaout
rupa.library as rupalib

rupaout.binaryName = rupa
rupaout.libraryName = rupa
rupaout.libDir = lib
rupaout.libraryShared = true

# C. ruka — embed arsip jadi dari rupamod
ruka.sources = src
ruka.exclude = main.c
ruka.output as rukaout
ruka.library as rukalib

rukaout.binaryName = ruka
rukaout.libraryName = ruka
rukaout.libDir = lib
rukaout.libraryShared = true

ruka.embedded.modules as rukamod

ruka.depends_on = rupamod

rukamod.file = ../rupamod/dist/rupa_modules.tar.gz
rukamod.variable = MODULES
ruka.headers = ../rupa/include

rukalib.linux = rupa
rupalib.linux = ruka
