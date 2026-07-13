# pixel-goo

[![version](https://img.shields.io/github/v/release/lczyk/pixel-goo?label=version&color=blue)](https://github.com/lczyk/pixel-goo/releases)
[![homebrew tap](https://img.shields.io/github/v/release/lczyk/pixel-goo?label=homebrew%20tap&color=orange)](https://github.com/lczyk/homebrew-pixel-goo)

Goo-like particle system simulation running on the GPU.

Work in progress, hence isn't actually doing the thing it's supposed to yet. The GPU computing works though and it compiles fine (at least on a Mac).

The idea is to make something similar to Sebastian Lague's [Ant and Slime Simulations](https://www.youtube.com/watch?v=X-iSQQgOd1A) - particles following one another's trails, but **without** looking at [the code](https://github.com/SebLague/Slime-Simulation).

## Install

Via [Homebrew](https://brew.sh) (macOS and Linuxbrew):

```bash
brew install lczyk/pixel-goo/goo
```

This installs the `goo` binary (plus `goo-macwp`, the desktop-wallpaper variant, on macOS).

If Homebrew refuses to load the tap with a trust error, it's the third-party tap gate (`HOMEBREW_REQUIRE_TAP_TRUST`). Trust it once and retry:

```bash
brew trust --tap lczyk/pixel-goo
```

## Build from source

```bash
./nob.c          # show usage (default target)
./nob.c build    # compile
./nob.c run      # build and run
./nob.c clean    # remove the build dir
```

## Autoformat

for C:

```bash
find *.c | xargs -L1 clang-format -style="{BasedOnStyle: llvm, IndentWidth: 4, ColumnLimit: 0}" -i
find *.h | xargs -L1 clang-format -style="{BasedOnStyle: llvm, IndentWidth: 4, ColumnLimit: 0}" -i
```

## ToDo's

- [x] automagically include shader files at compile time
- [x] fullscreen mode is still a bit janky
- [x] no acceleration and velocity shaders
- [x] no velocity double buffer
- [x] fix segfault buggs
- [x] ? add frame rendering
- [ ] trail buffer colormap sampling
- [ ] screen rendering shader could blend between density and trail colormap
- [ ] ? better lerp in screen rendering shader
