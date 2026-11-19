# How Sacred reads Esc

Window procedure `Sacred.exe` 0x812bf0 (jump tables at 0x813a28/0x813a3c), game state in `[0x17e7d34]`:

- `WM_KEYDOWN` (0x812f59): tracks Shift/Ctrl/Alt in `[0x182ee70]`. Only when the state is 5 (text entry) it calls
  `GetKeyboardState` + `ToAscii`/`ToUnicode` and posts a key event carrying that character. In every other state it
  returns without producing any event.
- `WM_CHAR` (0x81306d): outside state 5 it posts a key event with the character (0x1B for Esc). This is how Esc reaches
  the in-game menu.
- `WM_KEYUP` (0x8130be): posts a key-up event for every virtual key except 0x1B and 0x3E8.
- `WM_SYSKEYDOWN/UP` fall through to `DefWindowProcA`.
- The intro-movie loop at 0x6a1402 polls `GetAsyncKeyState(VK_ESCAPE)` and `VK_SPACE` to skip.

The message loop (0x817f4d) calls `TranslateMessage` on every message, so `WM_CHAR` 0x1B exists only if
`TranslateMessage` creates it. SDL text input never produces Esc, Backspace, Tab, Enter or Ctrl combinations, and
`TranslateMessage` used to be a no-op, so the play log's `KeyDown value=27` events (three of them) never became a
`WM_CHAR`. `TranslateMessage` now queues `WM_CHAR` for `WM_KEYDOWN` whose US-layout translation is a control
character (below 0x20 or 0x7F), at the front of the queue, with the key's lParam; printable characters stay with SDL text
input so they are not duplicated. `ToAscii` already returned 0x1B for VK_ESCAPE and `GetAsyncKeyState(27)` reads the host key
bitset. `GetKeyState` is not imported by Sacred.
