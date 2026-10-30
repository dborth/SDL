# SDL3 for Nintendo Wii, GameCube and Wii U

This is a fork of [libsdl-org/SDL](https://github.com/libsdl-org/SDL) carrying SDL3 ports for three Nintendo consoles. The ports live on their own branches; **`main` is only a mirror of upstream**.

| Console | Branch | Toolchain | Build | Latest release |
| --- | --- | --- | --- | --- |
| Wii/GameCube | [`ogc-port`](https://github.com/dborth/SDL/tree/ogc-port) | devkitPPC + [libogc2](https://github.com/extremscorner/libogc2) | [![Wii/GameCube build](https://github.com/dborth/SDL/actions/workflows/ogc.yml/badge.svg?branch=ogc-port)](https://github.com/dborth/SDL/actions/workflows/ogc.yml?query=branch%3Aogc-port) | [`ogc-*` releases](https://github.com/dborth/SDL/releases?q=ogc-) |
| Wii U | [`wut-port`](https://github.com/dborth/SDL/tree/wut-port) | devkitPPC + [wut](https://github.com/devkitPro/wut) | [![Wii U build](https://github.com/dborth/SDL/actions/workflows/wut.yml/badge.svg?branch=wut-port)](https://github.com/dborth/SDL/actions/workflows/wut.yml?query=branch%3Awut-port) | [`wut-*` releases](https://github.com/dborth/SDL/releases?q=wut-) |

Both track SDL `release-3.4.x`. The ports are developed against [Super Mario War](https://github.com/dborth/smw-wii), which boots and runs on both.

## Use it

**Prebuilt:** download the `sdl3-*.tar.gz` for your console from the releases page and extract it over your devkitPro install (it contains the library, headers and CMake config):

```sh
sudo tar -xzf sdl3-wiiu-wut-<version>.tar.gz -C /     # assumes DEVKITPRO=/opt/devkitpro
```

Then, in your project, with the console's devkitPro CMake toolchain file:

```cmake
find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(myapp PRIVATE SDL3::SDL3)
```

**From source:**

```sh
git clone --branch ogc-port --single-branch --depth 1 https://github.com/dborth/SDL.git   # Wii/GameCube
git clone --branch wut-port --single-branch --depth 1 https://github.com/dborth/SDL.git   # Wii U
```

Build instructions are in each branch: [`docs/README-ogc.md`](https://github.com/dborth/SDL/blob/ogc-port/docs/README-ogc.md) and [`docs/README-wiiu.md`](https://github.com/dborth/SDL/blob/wut-port/docs/README-wiiu.md). For a working example of consuming either port as a submodule, see [smw-wii](https://github.com/dborth/smw-wii).

## License and credits

SDL is zlib licensed; that license is unchanged on both branches. Thanks to the SDL project, devkitPro (devkitPPC, wut), Extrems and the libogc2 contributors, and the authors of the original SDL2 Wii U port.

