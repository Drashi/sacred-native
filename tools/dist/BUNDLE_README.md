# Sacred Native (Linux ARM64)

A native port of Sacred Gold. It needs **your own installed copy** of the game (the directory containing `Sacred.exe`);
no game data is part of this download and the installation is only ever read.

## Run

    ./sacred-launcher

The launcher window finds your installation (or lets you choose its folder), checks that it is Sacred Gold 2.28
(English, GOG or Steam) and starts the game with **Play**. Its **Settings** page holds this port's options (display
mode, screen size, frame rate limit, the original Sacred look, render scale, HD textures), each with a line on what
it does; a change is written into the profile's `Settings.cfg` at once. A new profile starts fullscreen at the
display's size, at the display's refresh rate, with render scale 2x and HD textures 2x.

**First start:** this download contains no code of the game either. The first time you press Play the launcher
translates the game from your own `Sacred.exe`, `granny.dll` and `tincat2.dll` and compiles it (a few minutes, with a
progress bar; about 5 minutes on 6 ARM cores). The result is kept in
`${XDG_DATA_HOME:-~/.local/share}/Sacred Native/game-module` and reused; it is built again only after an update.
The bundle carries what this needs: the translator and its recipe (`source/`: addresses and lengths of the game's
instructions, no byte of the game) and a Python (`python/`, its standard library only). It compiles with your system's C
compiler (`cc`, gcc or clang; `CC=...` picks another) and, when there is none, with the bundled Zig (`zig/`).

From a terminal the same happens with

    ./sacred-gold "/path/to/Sacred Gold"

The path is remembered. Without an argument the launcher tries `$SACRED_GOLD_DIR`, the remembered path and common
locations (`~/GOG Games`, `~/Games`, Wine `drive_c`, Steam, `/opt`), and otherwise prints how to pass it.

* Profile (settings, saves): `${XDG_DATA_HOME:-~/.local/share}/Sacred Native/Sacred Gold` (`--profile DIR` or `$SACRED_PROFILE` to change).
* First run seeds `Settings.cfg` there from the installation's file with `FULLSCREEN : 0`, `SOUND : 1`, `SHOWMOVIE : 1`
  (a minimal file if the installation has none). The launcher's Settings page edits the keys below in it, or edit it
  by hand (the game keeps keys it does not know when it writes the file).
* `GFX32 : 1` and `COMPAT_VIDEO : 1` are set at every start (the engine needs both: 32-bit display modes, films
  scaled to the window); other values for them are replaced.
* Fullscreen: `--fullscreen` (remembered until `--windowed`), or Alt/Option+Enter in the game window. `FULLSCREEN : 1`
  in `Settings.cfg` is converted to `--fullscreen`: the engine has no exclusive DirectDraw fullscreen, so the game runs
  windowed in a window that covers the display. Without `HD_WIDTH`/`HD_HEIGHT` a fullscreen start sets the game
  screen to the display's size (`SACRED_HD_AUTO=0` keeps 1024x768).
* `AUTO_PICKUP : 1` / `SACRED_AUTO_PICKUP=1` (off by default): the hero picks up the loot lying next to it while
  standing or walking, without a click. What it takes follows the game's own pick-up setting in the in-game
  Options (gold, gold and uniques, or everything). Items you drop stay where they are; an item that does not fit
  is tried again once the hero has walked away from it. Single player only, and not on horseback.
* `GAMEPAD : 1` / `SACRED_GAMEPAD=1` (off by default): play with a game controller (Xbox, PlayStation, Switch and
  others SDL knows). Mouse and keyboard keep working beside it. Buttons are named as on an Xbox pad; "LT +" means
  with the left trigger held.
  * In the world: the **left stick** walks toward where it points. **A** attacks the nearest enemy in the stick's
    direction (the right stick's when it is pushed) while held and goes on to the next one; with no enemy near it
    uses, opens, picks up or talks to the nearest thing, and with nothing near it attacks in place. **X** casts the
    active combat art at the nearest enemy. **Y, B, RB, RT**: combat art slots 1-4 (LT + A: slot 5). LT + X, Y, B,
    RB, RT: weapon slots 1-5. **D-pad**: healing potion (up), heal hirelings (down), antidote (left), concentration
    (right); LT + up / down: Undead Death / Mentor potion, LT + left / right: zoom out / in. **LB** held: stand
    still. **Start**: game menu (LT: options). **Back**: inventory (LT: world map). **L3** held: show item names
    (LT + L3: collect all). **R3**: overview map on / off. **LT + R3**: a free cursor in the world on / off.
  * Menus, the game menu, message boxes, the portal list and NPC dialogs: the **D-pad** or the **left stick** jumps
    between the entries and the cursor starts on the first one (a message box's OK, the first town); **A** chooses,
    **B** goes back (Esc), the right stick moves the cursor freely. **Start** is Enter in the start menu and closes
    the game menu.
  * Windows in game (inventory, traders, chests, maps): the **left stick** moves the cursor, the **D-pad** moves it
    by one inventory cell, **LB / RB** switch the inventory's tabs, **LT / RT** jump between the open windows
    (backpack, equipment, statistics; each keeps where the cursor was), the right stick scrolls. **A / X** are the
    left / right mouse button, **B** closes (Esc).
  * Movies: **A**, **B** or **Start** skips.
* `LEGACY_UI : 1` in `Settings.cfg` switches to the original Sacred look: its UI skin (bronze frames with vines,
  in the menus and in game), menu art (sunlit gate main menu, character select cave, dwarven hall behind the
  options) and menu music (`MUSIC_MENU.mp3`)
  instead of the Underworld skin, art and `MENU_ADDON.mp3`.
  The original character-select backdrop has no places for the two Underworld classes; a repainted 1024x768 PNG can
  replace it: `python3 tools/legacy_char_pak.py GAME BACKDROP.png PROFILE` writes `PROFILE/PAK/texture03.pak`
  (delete it to go back). The art is in every Sacred Gold installation; Gold only disabled it.
* Log: `<profile>/sacred-native.log` (previous run: `.log.1`). No QA, evidence or HLE-trace output unless requested:
  engine options go after `--`, e.g. `sacred-gold GAME -- --hle-trace` or `-- --evidence DIR`.

## Graphics

The renderer needs an OpenGL 3.3 core context. The game tries core 3.3, core 3.3 forward-compatible and core 4.1;
if the driver offers none of them it restarts itself once with Mesa software rendering (llvmpipe) and logs one line
starting with `SACRED_GL`. Software rendering is slow on weak CPUs; fix the GL driver for full speed.

Optional, off by default (`Settings.cfg` key, or the environment variable, which wins; a directory may be in quotes
and may start with `~/`, and a relative one is read from the working directory, so give a full path):

* `RENDER_SCALE : 2` / `SACRED_RENDER_SCALE=2` (1..4): the 3D scene and the screen are rendered at k times the game
  resolution and filtered down to the window (sharper models and edges; k*k times the GPU fill cost).
* `TEXTURE_PACK : DIR` / `SACRED_TEXTURE_PACK=DIR`: replacement textures. A PNG named `<w>x<h>_<FORMAT>_<hash>.png`
  (or `<hash>.png`, in any subdirectory) replaces the game texture with that content hash; it must be an integer
  multiple of the original size (e.g. 512x512 for a 256x256 texture), same factor on both axes.
* `TEXTURE_UPSCALE : 2` / `SACRED_TEXTURE_UPSCALE=2` (2..4): the same as a full pack built by
  `tools/textures/texture_pack_build.py` (Lanczos, atlas-safe, byte for byte the same images), made by the engine
  from the game's own textures when they are first used, on background threads (about 10 ms per texture), so no
  pack needs to be built or stored. A texture shows in its original size for the frame or two until its upscale is
  ready. Pack files win where both exist. The first start reads the game's texture paks for the list of textures
  to upscale (about 10 s in the background); the list is kept in the profile (`texture-upscale.index`). Use it with
  `RENDER_SCALE : 2`, which is what makes the larger textures visible.
* `SACRED_TEXTURE_DUMP=DIR` writes every distinct game texture once under that name, as the starting point for a pack.
* `MENU_BACKGROUNDS : DIR` / `SACRED_MENU_BACKGROUNDS=DIR` (HD mode): wide pictures for the screens the game draws
  at 1024x768 (menus, loading screens). In HD mode those screens are scaled to the window height; without pictures
  the sides stay black. One PNG per screen, named `options-sacred.png`, `options-underworld.png`, `saveload.png`
  (the save and load window, the same in both looks), `charselect-sacred.png`, `charselect-underworld.png`,
  `main-sacred.png`, `main-underworld.png` (the `-sacred` ones with `LEGACY_UI : 1`), `loading-<bitmap>.png` for
  `PAK/<bitmap>.bmp` (e.g. `loading-loadinguw00.png`) and `worldmap.png`: any height (768 or a multiple), wider than
  4:3, with the original screen unchanged in the middle; or as a pair `<screen>.sides.png` + `<screen>.mask.png`
  holding only the pixels that differ from the game's screen, which the engine fills in from your `texture.pak`
  (`tools/menu/split_backgrounds.py`; the bundle's `menu-backgrounds/` holds such pairs for every screen but the
  world map, and the launcher uses them whenever the screen size is not the original)
  (the picture may blend into its new sides near the old 4:3 border: the game's own background is taken from the
  picture too). The main menu and the character select are drawn on a canvas as wide as the window (up to 1836x768):
  their picture is the foreground, with alpha where the live sky and water show through (the sky and the water then
  reach across the whole window; where the picture paints the water under its holes as the game shows it, that
  painting continues the water at the sides); without one only the original middle is shown. The bundle's pictures
  were made for this project from the game's screens; to use your own, put them in another directory and name it
  here. The log lists what was found (`MENU_BACKGROUNDS ...`), and says so when
  the directory cannot be opened, holds no usable picture or links to pictures that are not there (a link with a
  relative target breaks when it is copied elsewhere: copy the pictures themselves), or when HD mode is off.
* `FPS_LIMIT : 120` / `SACRED_FPS_LIMIT=120` (30..240, or `auto` for the display's refresh rate): the frame rate cap,
  60 as in the original when unset. Movement, animation and game logic are timed, so they keep their speed; the
  frame-counted timeouts, the day/night clock and one weather drift are corrected for the new rate. A network game
  stays at 60. Only the game's own limiter paces the frames unless `Settings.cfg` sets `WAITRETRACE` (1..8: wait that many
  ticks of the frame rate clock per frame, which rounds every frame up to the next tick).
* `SHOW_FPS : 1` / `SACRED_SHOW_FPS=1`: a frame rate counter (frames presented per second, updated twice a second)
  in the top-left corner of the game's image; off by default, also on the launcher's Settings page. Screenshots
  taken by the runtime leave it out.

## Contents and dependencies

* `sacred-launcher` (window), `sacred-gold` (terminal), `sacred-native` (the runtime; it loads the game module built
  on your machine), `lib/` (see `lib/BUNDLED_LIBRARIES.txt`), `source/`, `python/`, `zig/`, `menu-backgrounds/`, `tools/`,
  `licenses/`.
* Bundled: FFmpeg (LGPL, WMV/WMA only), SDL2 (zlib), FreeType (FTL), the Bochs softfloat library (LGPL-2.1+), Zig (MIT)
  and CPython (PSF) for the first-start build.
* From the system: glibc, libstdc++, zlib and the usual desktop libraries (OpenGL/EGL, X11 or Wayland, PulseAudio or
  ALSA). Headless systems may need `libgl1 libpulse0 libasound2 libdecor-0-0 libsamplerate0`.

FFmpeg is linked dynamically: `lib/libav*.so.*` and `lib/libsw*.so.*` can be replaced by your own build of the same
sonames. Its source and the script that built these libraries are in `licenses/` (`ffmpeg-*.tar.xz`,
`ffmpeg-build.sh`).
