Wii and GameCube (libogc2)
==========================

SDL3 port for the Nintendo Wii and GameCube, built with devkitPPC and
[libogc2](https://github.com/extremscorner/libogc2).
Plain libogc (the `wii-dev` group) is not supported: its portlibs are not ABI
compatible with libogc2.

Building
--------

Use a devkitPro install with libogc2 (the `ghcr.io/extremscorner/libogc2`
container has everything). For the Wii:

```
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake \
  -DDKP_OGC_PLATFORM_LIBRARY=libogc2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_LIBICONV=OFF -DSDL_SYSTEM_ICONV=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build build
cmake --install build
```

For the GameCube use `GameCube.cmake` as the toolchain file. Install prefixes
are `$DEVKITPRO/libogc2/wii` and `$DEVKITPRO/libogc2/gamecube`; both are
already on the toolchain's search path. Only a static library is built.

`Catnip.cmake` defines both presets, so `catnip build` and `catnip install`
build and install the two consoles in one go.

CI and packages
---------------

`.github/workflows/ogc.yml` builds Wii and GameCube on every push and pull
request to `ogc-port`. Pushes also update the rolling `ogc-latest` pre-release
with install tarballs and a pacman package built from `packaging/PKGBUILD`.
