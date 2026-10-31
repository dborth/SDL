# Wii U GX2 shaders

`colorShader.*` and `textureShader.*` are taken unchanged from the SDL2 Wii U
port (`devkitPro/wut-packages`, `SDL2/SDL2-2.32.10.patch`, files under
`src/render/wiiu/shaders/`, by GaryOderNichts, zlib license like the rest of SDL).

* `*.vsh`, `*.psh`: Latte assembly sources.
* `*.inc`: the assembled GSH files as C arrays (`xxd -i`). These are what the
  driver includes, so building SDL does not need an assembler.
* `build_shaders.sh`: rebuilds the `.inc` files. It needs `latte-assembler` from
  decaf-emu (https://github.com/decaf-emu/decaf-emu) next to it.

Both programs multiply the vertex color by the sampled texel (texture shader) or
output it (color shader). The vertex shader multiplies the position with the
`u_projection` mat4 (the only uniform, GX2 uniform register offset 0, 16 floats).
Attributes: location 0 `a_position` (vec2), 1 `a_color` (vec4, UNORM8x4),
2 `a_texcoord` (vec2, texture shader only). The sampler is `u_texture`, location 0.
