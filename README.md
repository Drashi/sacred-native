# Sacred Native

A native port of **Sacred Gold** for Linux ARM64 and macOS on Apple Silicon. Play your copy of the game without Windows,
Wine or an emulator, at any screen size and frame rate.

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to the owners of the Sacred
trademark or the game's original developers and publishers. "Sacred" is their trademark.

## You need your own copy of the game

Sacred Native contains **no game code and no game data**. It needs an installed **Sacred Gold 2.28** (English, the GOG or
Steam release): the folder that holds `Sacred.exe`, `granny.dll` and `tincat2.dll`. On macOS or Linux you can copy that
folder from a Windows installation. The installation is only read, never changed.

On the first start the launcher translates the game's own `Sacred.exe`, `granny.dll` and `tincat2.dll` into native code
on your machine (a few minutes, once per update). What ships is the runtime that replaces Windows, DirectDraw/Direct3D,
the Miles sound system and DirectShow, and a translation recipe that lists where the game's instructions are, without
any byte of the game.

## Download and play

1. Download the bundle for your system from the releases page and unpack it.
   - **macOS:** remove the download quarantine once: `xattr -dr com.apple.quarantine sacred-native-macos-arm64`
     (the binaries are signed ad hoc, not notarized).
2. Start `./sacred-launcher`. It finds your installation (or lets you pick its folder), checks the version, and
   starts the game with **Play**. The first Play builds the game module; later starts are immediate.
3. From a terminal the same works with `./sacred-gold "/path/to/Sacred Gold"`.

Settings and saves live in your profile: `~/Library/Application Support/Sacred Native/Sacred Gold` on macOS,
`~/.local/share/Sacred Native/Sacred Gold` on Linux. The log is `sacred-native.log` in the same folder.

## HD and widescreen

The launcher's **Settings** page holds the port's options, each with a short explanation. A new profile starts
fullscreen at the display's size and refresh rate with sharper rendering and textures already on.

- **Any resolution.** The game runs at the window's or display's size instead of 1024x768, including wide screens.
  Menus and loading screens designed for 4:3 are scaled to the screen height; the main menu and character select extend
  their sky and water to the full width, and the bundle's menu pictures fill the sides of the other screens.
- **Render scale** (1-4x) draws the 3D scene at a multiple of the window size and filters it down.
- **HD textures** (2-4x) upscales the game's own textures in the background; texture packs can replace them.
- **Frame rate limit** up to 240 FPS, or the display's refresh rate. Movement and game logic keep their speed.
- **Original Sacred look**: the classic UI skin, menu art and menu music that Sacred Gold ships but does not use.

The **Gameplay** section holds **Auto pick-up** (off by default): the hero picks up the loot it walks past, filtered
by the game's own pick-up setting from the in-game Options.

All options are keys in the profile's `Settings.cfg` too; the bundle's `README.md` lists them.

## Building from source

See [CONTRIBUTING.md](CONTRIBUTING.md). In short, with the game installed:

```sh
export SACRED_ASSETS="/path/to/Sacred Gold"
tools/build_macos_deps.sh                         # macOS only: SDL2, FreeType, pkg-config in build/
tools/bootstrap.sh                                # Python venv, LGPL FFmpeg, generated C
cmake -S . -B build/game -DSACRED_BUILD_GAME=ON && cmake --build build/game -j
build/game/sacred-native --assets "$SACRED_ASSETS"
```

`tools/dist/make_bundle.sh` builds the same bundle the releases carry.

## How it works

`docs/ARCHITECTURE.md` describes the translator, the runtime and the CPU semantics. In short: every x86 instruction
of the game becomes a few lines of C over an explicit CPU state, compiled by your C compiler. The x87 FPU keeps its
80-bit registers; the Windows, DirectDraw, Miles and COM APIs the game imports are implemented natively on SDL2,
OpenGL, FFmpeg and FreeType.

## License

Copyright (C) 2026 Sacred Native contributors.

Sacred Native is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later
version (`GPL-3.0-or-later`, full text in `LICENSE`). It is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY. Third-party components keep their own licenses; they and the LGPL relinking notes are listed in
`THIRD_PARTY_NOTICES.md`.
