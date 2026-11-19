# In-game menu -> Quit: what the original does and what the port provides

Static trace of `Sacred.exe` (addresses are VAs), cross-checked against
the log of a play session that ended with Quit.

## Verdict: Quit ends the process

There is no "return to the main menu" teardown on this path. The original unwinds to `WinMain` and the CRT calls
`ExitProcess`.

1. The shutdown trigger is a window close. `DefWindowProcA(WM_CLOSE)` destroys the main window, `WM_DESTROY` reaches the
   game's window procedure (`0x812bf0`, case `0x812f12`: log string `message: WM_DESTROY`) and calls
   `PostQuitMessage(0)` (`0x812f20`). `WM_CLOSE` (case `0x812efb`, log string `message: WM_CLOSE`) first runs the one-shot
   shutdown routine `0x817580`, then falls through to `DefWindowProcA`.
2. The message loop at `0x817f4d` is the textbook `GetQueueStatus(0xff)` / `GetMessageA` / `TranslateMessage` /
   `DispatchMessageA` / `WaitMessage` loop. `GetMessageA` returns 0 on `WM_QUIT`, the loop ends, and the code after it
   (`0x817fbf`..) releases the mutex (`ReleaseMutex`, `CloseHandle` on `[0x182edb8]`), writes `Settings.cfg` and returns
   from `WinMain`; the CRT then reaches `ExitProcess` (`0x84daae`).
3. The play log shows exactly this order after the final click: `SDL_AUDIO_CLOSE`, `SDL_AUDIO_STOP`, `CreateFileA
   Settings.cfg` (write), `bin\wea.bin`, `PROCESS_EXIT code=0`, `SDL_TEARDOWN windows_destroyed=0` (the native window
   was already destroyed by the guest's close, not by teardown), `PLAY_EXIT 0`.

Not established statically: the identity of the UI handler that raises the close. The only literal `PostMessageA(hwnd,
WM_CLOSE, ...)` site is the busy dialog (`0x720eda`, `cUI_BusyDlg: sending WM_CLOSE...`); the Quit button reaches the same
`WM_CLOSE` through a path that is not a direct import call (UI event objects are dispatched through vtables). The shutdown
effects below do not depend on it.

## Shutdown routine `0x817580` (runs once, guarded by `[0xa1cf90]`)

It calls ten game-internal subsystem shutdown functions (`0x60d740`, `0x6a16b0`, `0x756fb0`, ...), deletes the D3D/DDraw
display object `[0xcdca1c]` through its virtual destructor, `HeapDestroy([0x182ee58])`, and
finally, only when the OS class returned by `0x66e670` is below 8 (Win9x/NT4-era detection) and `0x81c360` (display-mode
changed predicate) is true, `ChangeDisplaySettingsA(NULL, 0)`.

## Every external call on the path and its port status

| Call | Where | Port |
|---|---|---|
| `DefWindowProcA(WM_CLOSE)` | tail of the window procedure | `menu.cpp` `case 0x10`: `destroy_window`, which delivers `WM_DESTROY` and `WM_NCDESTROY` to the guest first (as Windows does) |
| `PostQuitMessage(0)` | `0x812f20` | `input.cpp`: sets the quit flag; `GetMessageA` returns 0 once the queue is drained |
| `GetQueueStatus`, `GetMessageA`, `TranslateMessage`, `DispatchMessageA`, `WaitMessage` | loop `0x817f4d` | implemented (`input.cpp`); `TranslateMessage` now also queues the control-character `WM_CHAR` |
| `Sleep`, `CloseHandle`, `ReleaseMutex`, `HeapDestroy`, `DeleteCriticalSection` | shutdown | implemented (`startup.cpp`, `virtual.cpp`, `threads.cpp`) |
| `ShowWindow` | `0x813c9f` etc. | implemented (`menu.cpp`) |
| `IDirectDraw7::Release`, `SetCooperativeLevel` | display object destructor | implemented (`com.def`) |
| `IDirectDraw7::RestoreDisplayMode` (slot 19) | the deactivate path `0x812edb` (`- restoreDisplayMode`, then `ShowWindow(hwnd, SW_MINIMIZE)`) and display object teardown | **was missing (strict trap); now a documented no-op returning DD_OK**, because the port never leaves the desktop display mode. Test: `menu_window_callbacks` |
| `ChangeDisplaySettingsA(NULL, 0)` | `0x8176f5` | not implemented, **unreachable**: `GetVersionExA` reports Windows 6.1, which the classifier at `0x66e330` maps to class `0xb` (>= 8), so the `< 8` guard at `0x8176e0` skips it. It stays a strict trap should the reported OS profile ever change |
| Miles `_AIL_stop_sample`, `_AIL_end_sample`, `_AIL_release_sample_handle`, `_AIL_close_stream`, `_AIL_waveOutClose`, `_AIL_shutdown` | subsystem shutdowns (`0x6a16b0` is the audio one) | implemented; NULL handles follow the original per `miles-null-handles.md` |

A recorded session exercised all of this once end to end and ended with `PROCESS_EXIT code=0`, so no other import on
the path is unimplemented for the windowed profile.

## Follow-ups (not changed here)

- `SetFocus` and `SetForegroundWindow` are still unimplemented; the activation path in the window
  procedure calls `SetFocus` (`0x812da0`), reached on window activation, not on Quit.
- Which UI object raises the close was not traced to the button handler.
