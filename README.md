# SDL3 for Nintendo Wii, GameCube and Wii U

This is a fork of [libsdl-org/SDL](https://github.com/libsdl-org/SDL) carrying SDL3 ports for three Nintendo consoles. The ports live on their own branches; **`main` is only a mirror of upstream**.

| Console | Branch | Toolchain | Build | Download |
| --- | --- | --- | --- | --- |
| Wii, GameCube | [`ogc-port`](https://github.com/dborth/SDL/tree/ogc-port) | devkitPPC + [libogc2](https://github.com/extremscorner/libogc2) | [![ogc build](https://github.com/dborth/SDL/actions/workflows/ogc.yml/badge.svg?branch=ogc-port)](https://github.com/dborth/SDL/actions/workflows/ogc.yml?query=branch%3Aogc-port) | [`ogc-latest`](https://github.com/dborth/SDL/releases/tag/ogc-latest) |
| Wii U | [`wut-port`](https://github.com/dborth/SDL/tree/wut-port) | devkitPPC + [wut](https://github.com/devkitPro/wut) | [![wut build](https://github.com/dborth/SDL/actions/workflows/wut.yml/badge.svg?branch=wut-port)](https://github.com/dborth/SDL/actions/workflows/wut.yml?query=branch%3Awut-port) | [`wut-latest`](https://github.com/dborth/SDL/releases/tag/wut-latest) |

Both track SDL `release-3.4.x` and are static-library only (no shared libs, no dynamic loading). They are developed against [Super Mario War](https://github.com/dborth/smw-wii), which boots and runs on them.

**Downloads are continuous builds.** Every push to a port branch updates its pre-release above. They are not certified releases; the commit each one was built from is in the release notes.

## Use it

**pacman package** (devkitPro pacman): download the `.pkg.tar.*` from the release, then

```sh
sudo dkp-pacman -U sdl3-libogc2-*.pkg.tar.zst      # Wii + GameCube
sudo dkp-pacman -U sdl3-wut-*.pkg.tar.zst  # Wii U
```

**Install tree:** the `sdl3-*.tar.gz` files contain the library, headers and CMake config under the devkitPro prefix:

```sh
sudo tar -xzf sdl3-wut.tar.gz -C /     # assumes DEVKITPRO=/opt/devkitpro
```

Then, in your project, with the console's devkitPro CMake toolchain file:

```cmake
find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(myapp PRIVATE SDL3::SDL3)
```

**From source:**

```sh
git clone --branch ogc-port --single-branch --depth 1 https://github.com/dborth/SDL.git   # Wii, GameCube
git clone --branch wut-port --single-branch --depth 1 https://github.com/dborth/SDL.git   # Wii U
```

Build instructions are in each branch: [`docs/README-ogc.md`](https://github.com/dborth/SDL/blob/ogc-port/docs/README-ogc.md) and [`docs/README-wiiu.md`](https://github.com/dborth/SDL/blob/wut-port/docs/README-wiiu.md). For a working example of consuming the ports as submodules, see [smw-wii](https://github.com/dborth/smw-wii).

## License and credits

SDL is zlib licensed; that license is unchanged on both branches. Thanks to the SDL project, devkitPro (devkitPPC, wut), Extrems and the libogc2 contributors, and the authors of the original SDL2 Wii U port.
