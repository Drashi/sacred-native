# DirectShow surface used by Sacred

Source: the `Sacred.exe` instruction listing, functions at 0x006a0d80 (create stream), 0x006a0ea0 (play) and
0x006a1590 (wrapper); the single `CoCreateInstance` call site is 0x006a0d92. VAs below are image VAs (RVA + 0x400000).

## Activation
`CoCreateInstance(CLSID_AMMultiMediaStream {49C47CE5-9BA4-11D0-8212-00C04FC32C45}, NULL, CLSCTX_INPROC_SERVER, IID_IAMMultiMediaStream
{BEBE595C-9A6F-11D0-8FDE-00C04FD9189D}, &stream)`. If it returns 0x800401F0 (CO_E_NOTINITIALIZED) the game calls `CoInitialize(0)` and
retries once. Any other failure is returned to the caller, which treats the movie as unavailable (the skip path).

## Calls on the stream (vtable slot = byte offset / 4)
| Interface | Slot | Method | Arguments seen |
|---|---|---|---|
| IAMMultiMediaStream | 12 (+0x30) | Initialize | STREAMTYPE_READ(0), 0, NULL |
| | 15 (+0x3c) | AddMediaStream | (IDirectDraw legacy, MSPID_PrimaryVideo, 0, NULL), then (NULL, MSPID_PrimaryAudio, AMMSF_ADDDEFAULTRENDERER, NULL) |
| | 16 (+0x40) | OpenFile | (wide path converted with MultiByteToWideChar, 0) |
| | 4 (+0x10) | GetMediaStream | (MSPID_PrimaryVideo, &IMediaStream) |
| | 7 (+0x1c) | SetState | RUN(1) before the loop, STOP(0) after it |
| IMediaStream | 0 | QueryInterface | IID_IDirectDrawMediaStream {F4104FCE-9A70-11D0-8FDE-00C04FD9189D} |
| IDirectDrawMediaStream | 13 (+0x34) | CreateSample | (surface or NULL, RECT 0,0,640,480, flags 0 or 1, &sample) |
| IDirectDrawStreamSample | 6 (+0x18) | Update | (0, NULL, NULL, 0): synchronous, blocks until the next frame is due |
| | 8 (+0x20) | GetSurface | (&IDirectDrawSurface, &RECT) in the stream-owned-surface mode |
All objects are `AddRef`/`Release`d (slots 1/2) and QueryInterface'd (slot 0). Nothing else is called (no GetTime, Seek, GetDuration, event handles).

## Rendering
Two modes, selected by a config string at 0x182ee88 (empty selects mode A):
* A (texture): the game creates a 1024x512 texture surface (IDirectDraw7::CreateSurface), QueryInterfaces it for legacy IDirectDrawSurface
  (IID 6C14DB81...), passes it to `CreateSample`, and draws it as a textured quad with IDirect3DDevice7 (SetTexture + DrawPrimitive).
* B (windowed Blt): `CreateSample(NULL, NULL, DDSFF_PROGRESSIVERENDER, ...)`; the stream owns the frame surface, the game `GetSurface`s it and
  `Blt`s it (DDBLT_WAIT) from the legacy primary surface onto the window rectangle (GetWindowRect of the game window).
The measured configuration (default profile, SHOWMOVIE 1) takes mode B; mode A is not exercised by the shipped profile and is implemented
through the same sample code (caller-supplied surface) but not run.

## Wait, skip, completion
Per frame the game polls `GetAsyncKeyState` for VK_ESCAPE (0x1b), VK_SPACE (0x20), VK_LBUTTON (1) and VK_RBUTTON (2) (high bit); any of them
ends the movie. ESC/SPACE additionally make the game wait until the key is released (no frames presented meanwhile). Then `Update` is called;
a nonzero return (MS_S_ENDOFSTREAM 0x00040003) ends the loop. In all cases the game calls `SetState(STOP)` and releases every object. The
caller expects only that `OpenFile`/`CreateSample` etc. return S_OK; a failing HRESULT from any of them is the documented skip path.
