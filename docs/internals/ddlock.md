# IDirectDrawSurface7::Lock flag sweep

Found through `RUN_FAILURE guest thread 5: IDirectDrawSurface7::Lock source_pc=401d88
args=2c873000,0,427ff64c,821,0` in manual play, when the runtime accepted only `rect == NULL`, `flags == DDLOCK_WAIT`,
`event == 0`.

## The failing lock

- Caller `0x401d88` is `CALL [ECX+0x64]` (vtable slot 25, `Lock`) with arguments `(surface, NULL, &DDSURFACEDESC2, 0x821, 0)`.
- The surface comes from the Sacred.exe surface factory `0x644ab0` (called at `0x401d48` with `type=1`, width, height,
  `flags=1` and an eight-dword `DDPIXELFORMAT`). For `type=1` it builds `DDSURFACEDESC2` with
  `dwFlags=0x1007` (CAPS|HEIGHT|WIDTH|PIXELFORMAT), `ddsCaps.dwCaps=0x1000` (`DDSCAPS_TEXTURE`) and
  `ddsCaps2=0x10` (`DDSCAPS2_TEXTUREMANAGE`). It is a managed texture that the guest fills with
  CPU writes: `create_surface` maps it to a colour surface with `DDSCAPS_SYSTEMMEMORY` and the
  guest-supplied packed format (RGB565 / ARGB1555 / ARGB4444 / ARGB8888 / XRGB8888).
- `0x821 = DDLOCK_WAIT (0x1) | DDLOCK_WRITEONLY (0x20) | DDLOCK_NOSYSLOCK (0x800)`, `rect == NULL` (whole surface).

## DDLOCK flag support (`runtime/ddraw/surfaces.cpp`, slot 25 / slot 32)

| Flag | Value | Behaviour |
|---|---|---|
| `SURFACEMEMORYPTR` | 0x0 | Default; `lpSurface` is always returned. |
| `WAIT` | 0x1 | Honoured; Lock is synchronous. |
| `EVENT` | 0x2 | **Rejected** (`GuestFault`), as is a non-zero event argument. |
| `READONLY` | 0x10 | Contents are readable; guest writes are dropped on Unlock (backing is restored, no upload). |
| `WRITEONLY` | 0x20 | Accepted; writes reach the surface on Unlock. Contents are still synchronised so partial writes are safe. |
| `READONLY|WRITEONLY` | 0x30 | `DDERR_INVALIDPARAMS` (0x80070057). |
| `NOSYSLOCK` | 0x800 | Hint, ignored. |
| `NOOVERWRITE` | 0x1000 | Hint, ignored. |
| `DISCARDCONTENTS` / `OKTOSWAP` | 0x2000 | Skips the GPU readback; contents are undefined for the guest. |
| `DONOTWAIT` | 0x4000 | Accepted; Lock never blocks. |
| `HASVOLUMETEXTUREBOXRECT`, `NODIRTYUPDATE`, unknown bits | 0x8000, 0x10000, ... | **Rejected** (`GuestFault`). |

Rectangles: `NULL` locks the whole surface; otherwise the rectangle must lie inside the surface and be non-empty
(`DDERR_INVALIDRECT` 0x88760096 on failure). `lpSurface` points at `data + top*pitch + left*bytes_per_pixel`, the pitch is the
full surface pitch, and Unlock copies back and uploads only the locked rectangle (and presents primaries). The
DirectDraw 7 Unlock rectangle is validated as readable and otherwise ignored. A second Lock on a locked surface returns
`DDERR_LOCKEDSURFACES` (0x887601ae); Unlock without Lock returns `DDERR_NOTLOCKED` (0x88760248).

Surface kinds: colour surfaces (primary, offscreen, texture, in every supported packed format) support the full
set above. Depth surfaces (`Depth16`, `Depth24Stencil8`) have no CPU backing in the runtime; Lock on them is still
rejected (`unqualified depth lock/readback`) and no call site below locks one.

## Static call-site sweep

Method: scan the instruction listings of `Sacred.exe`, `granny.dll` and `tincat2.dll` for
`CALL [reg+0x64]` within 60 instructions of a `DDSURFACEDESC2.dwSize = 0x7c` store, then read the five arguments from the
preceding `PUSH`es (`this, rect, desc, flags, event`, pushed in reverse). 272 `CALL [reg+0x64]` sites exist in Sacred.exe; 25 are
`IDirectDrawSurface7::Lock`. `granny.dll` has none, `tincat2.dll` has two `[reg+0x64]` calls but imports no DDraw and
builds no `DDSURFACEDESC2`, so neither locks a DirectDraw surface.

| Flags | Rect | Event | Sites (Sacred.exe VA) | Now supported |
|---|---|---|---|---|
| `0x821` WAIT\|WRITEONLY\|NOSYSLOCK | NULL | 0 | `0x401d88` (the crash, the managed texture above), `0x401f03` | yes |
| `0x1` WAIT | NULL | 0 | `0x4cc875 0x4cc99b 0x644e20 0x645ebb` (EBP is `1`)`  0x64618a 0x64b5bb 0x64b63d 0x64b73b 0x64b83e 0x64e257 0x64e2b5 0x64e42c 0x64e4cd 0x651d75 0x651dd4 0x651ee0 0x651f49 0x656ff2 0x71df10 0x75e349` | yes (unchanged) |
| `0x1` WAIT | register (value not constant) | 0 | `0x638fcc`, `0x645c3b`, `0x645ca8` | yes: NULL or an in-bounds rectangle |

`0x645ece` and `0x64619d` also match the filter but are one-argument calls (not Lock) on the retry path after
`DDERR_SURFACELOST` (0x887601c2); they are not Lock sites.

Every constant flag value found (`0x1`, `0x821`) is supported. Sites with a register rectangle are covered by the
partial-rectangle implementation; any rectangle outside the surface returns `DDERR_INVALIDRECT` instead of faulting.

## Tests

`tests/menu/graphics_tests.cpp` (`menu_graphics_com`): partial `0x821` lock pointer and Unlock upload bounds,
READONLY write drop, DISCARDCONTENTS, every supported flag with relock/unlock status codes, `READONLY|WRITEONLY`,
out-of-bounds and empty rectangles, and rejection of `EVENT`, a non-zero event, `NODIRTYUPDATE` and
`HASVOLUMETEXTUREBOXRECT` with no lock left behind.
