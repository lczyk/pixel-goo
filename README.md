# pixel-goo

Goo-like particle system simulation running on the GPU.

Work in progress, hence isn't actually doing the thing it's supposed to yet. The GPU computing works though and it compiles fine (at least on a Mac).

The idea is to make something similar to Sebastian Lague's [Ant and Slime Simulations](https://www.youtube.com/watch?v=X-iSQQgOd1A) - particles following one another's trails, but **without** looking at [the code](https://github.com/SebLague/Slime-Simulation).

## Instructions

The build system is [nob](https://github.com/tsoding/nob.h), a single-header C build tool. Bootstrap it once with any C compiler, then it rebuilds itself when `nob.c` changes:

```bash
cc -o nob nob.c
./nob          # show usage (default target)
./nob build    # compile
./nob run      # build and run
./nob clean    # remove the build dir
```

The binary lands at `./bin/goo`.

By default, windowed mode uses a bordered window. Pass `--no-border` if you want
the old borderless look.

## Cross-compile on macos for windows

Not yet ported to nob (the old cmake + mingw flow was dropped in the move to nob, see the ToDo's). The mingw toolchain is still the way in:

```bash
brew install mingw-w64
```

`nob.c` already has a `_WIN32` link branch (`-lopengl32 -lgdi32`); what's missing is selecting the cross compiler and target platform when bootstrapping nob.

## Autoformat

for C:

```bash
find *.c | xargs -L1 clang-format -style="{BasedOnStyle: llvm, IndentWidth: 4, ColumnLimit: 0}" -i
find *.h | xargs -L1 clang-format -style="{BasedOnStyle: llvm, IndentWidth: 4, ColumnLimit: 0}" -i
```

for python:

```bash
black .
```

## ToDo's

- [x] automagically include shader files at compile time
- [ ] fullscreen mode is still a bit janky
- [x] no acceleration and velocity shaders
- [x] no velocity double buffer
- [x] fix segfault buggs
- [ ] ? add frame rendering (the old C++ `saveFrame` was dropped in the C migration)
- [ ] trail buffer colormap sampling
- [ ] screen rendering shader could blend between density and trail colormap
- [ ] ? better lerp in screen rendering shader
- [ ] port windows cross-compile to nob
