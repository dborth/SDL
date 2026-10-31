# smoke tests for the SDL3 Wii U port.
#
# Build SDL3 first, then:
#
#   /opt/devkitpro/portlibs/wiiu/bin/powerpc-eabi-cmake -S test/wiiu -B build-hello \
#       -DCMAKE_PREFIX_PATH=$DEVKITPRO/portlibs/wiiu
#   cmake --build build-hello
#
# This produces build-hello/sdl3-wiiu-hello.rpx (smoke test) and
# build-hello/sdl3-wiiu-threads.rpx (thread, sync and timer tests),
# build-hello/sdl3-wiiu-audio.rpx (audio tests),
# build-hello/sdl3-wiiu-joystick.rpx (joystick and gamepad test),
# build-hello/sdl3-wiiu-files.rpx (filesystem test),
# build-hello/sdl3-wiiu-video.rpx (video core, software renderer),
# build-hello/sdl3-wiiu-render.rpx (GX2 renderer),
# build-hello/sdl3-wiiu-touch.rpx (GamePad touch, text input),
# build-hello/sdl3-wiiu-usb.rpx (USB keyboard and mouse),
# build-hello/sdl3-wiiu-lifecycle.rpx (memory cycles, HOME menu).
cmake_minimum_required(VERSION 3.16)
project(sdl3-wiiu-hello LANGUAGES C)

# Built on its own against an installed SDL3, or in tree (-DSDL_WIIU_TOOLS=ON in
# the top-level build), where the SDL3::SDL3 target already exists.
if(NOT TARGET SDL3::SDL3)
  find_package(SDL3 REQUIRED CONFIG)
endif()

# The tools that do not use SDL video (hello, threads, audio, joystick, files)
# draw their output on screen with console.c, which also provides main() and
# calls tool_main(). The video, render, touch and usb tools draw with SDL, so
# they keep their own main().
function(wiiu_console_tool name source)
  add_executable(${name} ${source} console.c)
  target_link_libraries(${name} PRIVATE SDL3::SDL3)
  wut_create_rpx(${name})
endfunction()

wiiu_console_tool(sdl3-wiiu-hello main.c)

wiiu_console_tool(sdl3-wiiu-threads threads.c)

wiiu_console_tool(sdl3-wiiu-audio audio.c)

wiiu_console_tool(sdl3-wiiu-joystick joystick.c)

wiiu_console_tool(sdl3-wiiu-files files.c)

add_executable(sdl3-wiiu-video video.c)
target_link_libraries(sdl3-wiiu-video PRIVATE SDL3::SDL3)

wut_create_rpx(sdl3-wiiu-video)

# GX2 renderer
add_executable(sdl3-wiiu-render render.c)
target_link_libraries(sdl3-wiiu-render PRIVATE SDL3::SDL3)

wut_create_rpx(sdl3-wiiu-render)

# touch, mouse and text input test
add_executable(sdl3-wiiu-touch touch.c)
target_link_libraries(sdl3-wiiu-touch PRIVATE SDL3::SDL3)

wut_create_rpx(sdl3-wiiu-touch)

# USB keyboard and mouse test
add_executable(sdl3-wiiu-usb usbinput.c)
target_link_libraries(sdl3-wiiu-usb PRIVATE SDL3::SDL3)

wut_create_rpx(sdl3-wiiu-usb)

# memory cycles, SDL_GetSystemRAM() and the HOME menu
add_executable(sdl3-wiiu-lifecycle lifecycle.c)
target_link_libraries(sdl3-wiiu-lifecycle PRIVATE SDL3::SDL3)

wut_create_rpx(sdl3-wiiu-lifecycle)
