# Mouse input: what Sacred expects and how the port delivers it

## What the game does

- It reads the cursor only inside its `WM_MOUSEMOVE` handler (`0x8131cf`), through `GetCursorPos` + `ScreenToClient`,
  never from lParam, and draws its own cursor (`SetCursor(NULL)`).
- It imports none of `SetCapture`, `ReleaseCapture`, `GetCapture`, `ClipCursor`, `ShowCursor`, `GetKeyState`, and no
  DirectInput.
- After a right-click on an enemy (special skill) it calls `SetCursorPos` twice: the current position, then a fixed
  displacement (+31,+23 in the recorded case).

## Input path in the port

SDL event pump (`WindowHost::pump`, window thread) -> `events_` and `input_` (position and buttons, read by
`GetCursorPos`/`GetAsyncKeyState`) -> guest thread `Menu::translate_events` (from `PeekMessageA`, `GetMessageA`,
`WaitMessage`, `GetQueueStatus`) -> `messages_` -> `fetch_message` returns the oldest matching message.

## Rules that came out of real bugs

- **One pending `WM_MOUSEMOVE` per window**, as on Windows: a new motion overwrites a queued one instead of appending.
  Appending built a backlog of stale positions whenever the guest polled slower than the event rate (combat on the
  software renderer), and the cursor lagged by seconds. Test: `menu_window_callbacks`.
- **Button flags come from a snapshot taken when the SDL event was pumped**, not from the host state at translate time.
- **An event with SDL window id 0** (a release outside every window) goes to the window that last received input, so the
  guest never keeps a button that never goes up.
- **On `FocusLost`** the matching `WM_*BUTTONUP` is synthesized before `WM_ACTIVATE(0)`.
- **Virtual cursor, no host warp** (`platform/virtual_cursor.h`). `SetCursorPos` used to call `SDL_WarpMouseGlobal`.
  Under GNOME/Wayland with the game on Xwayland the warp produces no motion events and clears SDL's mouse focus, so
  `GetCursorPos` froze until the pointer re-entered the window. Now `SetCursorPos` moves only the guest-visible cursor
  (converted with the same window origin `GetCursorPos` adds back), updates `input_` and queues one `WM_MOUSEMOVE`.
  Host motion is applied as deltas and clamped to the client area. No warp of any kind is used: the game draws its own
  cursor, any warp can drop pointer focus under Wayland, and macOS suppresses events after `CGWarpMouseCursorPosition`.
  Test: `menu_virtual_cursor` (`tests/menu/cursor_tests.cpp`)
  (30 s of motion with the game's warp sequence every 60 steps, guest position checked after every step).
- **The edges resynchronise guest and host.** Without a warp, every `SetCursorPos` displacement stays as an offset
  between the guest cursor and the host pointer (the game's +31,+23 after a special skill adds up towards the bottom
  right over a long session). In fullscreen the host pointer stops at the screen edge, so the top and left of the game
  became unreachable until the pointer was pushed against the opposite edges (issue #19). A host pointer at or past an
  edge of the client area that is not moving away from it now puts the guest cursor on that edge. Test:
  `menu_virtual_cursor`.
- While a QA script runs, real pointer events are dropped and only injected ones count, because replayed absolute
  positions cannot be mixed with real deltas.

## QA-only logging

`SACRED_QA_INPUT_LOG=1` writes to stderr:

- `QA_INPUT host_event kind= window= x= y= value= queue=` for every translated host event;
- `QA_INPUT coalesced WM_MOUSEMOVE ...` when a pending motion was overwritten;
- `QA_INPUT peek WM_MOUSEMOVE x= y= age_ms= remove= queue=` when the guest receives a motion;
- `QA_INPUT GetCursorPos x= y= window= local=` for every cursor query;
- `QA_INPUT sdl_state ...` (SDL focus, grab, button masks, window flags) on focus, enter/leave, minimize/restore and
  button transitions;
- `QA_INPUT unmapped host_event ...`, `QA_INPUT synthesized button release on focus loss bit=`,
  `QA_INPUT GetAsyncKeyState vk= down= host_buttons=`, `QA_INPUT SetCursor(NULL)`, `QA_INPUT SetCursorPos x= y=`.

`SACRED_QA_IO_LOG=1` logs every guest `CreateFileA`
(`QA_IO CreateFileA guest=<name> host=<path> access= disposition= -> ok|<errno text>|directory refused`).
The flags do not change game behaviour.
