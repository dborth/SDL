Wii (libogc2)
=============

SDL3 port for the Nintendo Wii/GameCube, built with devkitPPC and
[libogc2](https://github.com/extremscorner/libogc2).
Plain libogc (the `wii-dev` group) is not supported: its portlibs are not ABI
compatible with libogc2.

Building
--------

Use a devkitPro install with libogc2 (the `ghcr.io/extremscorner/libogc2`
container has everything), then:

```
cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Wii.cmake \
  -DDKP_OGC_PLATFORM_LIBRARY=libogc2 \
  -DCMAKE_BUILD_TYPE=Release \
  -DSDL_LIBICONV=OFF -DSDL_SYSTEM_ICONV=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake --build build
cmake --install build
```

The default install prefix is `$DEVKITPRO/libogc2/wii`, which is already on the
toolchain's search path. Only a static library is built.
