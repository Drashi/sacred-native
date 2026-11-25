# Movie playback: native DirectShow multimedia stream

* `platform/movie_decoder.{h,cpp}`: libavformat/libavcodec/libswscale/libswresample reader. Separate demux contexts for video (scaled to BGRA at the
  requested size) and audio (decoded whole to 44.1 kHz stereo S16). No SDL, no OS API.
* `platform/movie_pacer.h`: pure presentation policy (wait until due, drop a frame whose successor is already due). Unit-tested with a simulated clock.
* `runtime/ole/movie_playback.{h,cpp}`: decoder + audio + pacer. Master clock = audible position of the movie's PCM in the game's Miles mixer
  (mixed cursor minus device backlog); when no mixer exists (SOUND 0) a private SDL device is used, and with no audio device the wall clock.
* `runtime/ole/com.{h,cpp}`: host-implemented guest COM objects (IAMMultiMediaStream, IDirectDrawMediaStream, IDirectDrawStreamSample) with
  hosted-token vtables, same convention as `runtime/ddraw`. `Update` blocks on the pacer and writes the frame into the game's DirectDraw surface
  (`DirectDraw::movie_surface_*`); the game's own `Blt` presents it, so rendering stays on the existing SDL/GL path.
* Only the game's `COMPAT_VIDEO : 1` path is supported (the `Blt` above, onto `GetWindowRect`, so the film fills any window); the
  other path draws the film as a quad fixed to 0..1024 x 0..768 and an HD screen shows it in its top-left corner. The runtime
  sets `COMPAT_VIDEO : 1` (and `GFX32 : 1`, without which the game asks for a 16 bpp mode) in the profile's `Settings.cfg` at
  every start and logs `SETTINGS_FORCED key=... was=...` when it changed the file.
* `OpenFile` failing (file missing, codec unsupported) returns VFW_E_UNSUPPORTED_STREAM and logs `MOVIE_SKIPPED`: the documented skip path, unchanged
  game behaviour. Builds without the SDL host or DirectDraw adapter skip at `CoCreateInstance`.
* Added host APIs the movie path needs: `user32!GetWindowRect`, legacy `IDirectDrawSurface::Blt`, Blt/BltFast sources given as legacy pointers,
  `WindowHost::clear_audio`, QA `sleep MS` command.
* Events: `MOVIE_PLAY`, `MOVIE_END reason=completed|stopped frames_shown frames_dropped first_drop_pts max_av_error_ms audio_nonsilent_frames audio_rms`,
  `MOVIE_SKIPPED`, `MOVIE_AUDIO_UNAVAILABLE`.

## FFmpeg
Pinned FFmpeg 8.0.1, built by `tools/build_ffmpeg.sh` (SHA-256 checked, release signature verified against key FCF986EA15E6E293A5644F10B4322F04D67658D8)
into `build/ffmpeg/prefix` as shared libraries, LGPL, only the ASF demuxer, WMV1/2/3 and WMA1/2/Pro decoders, swscale and swresample, no
external libraries. CMake prefers that prefix (`SACRED_FFMPEG_PREFIX`, RUNPATH set) and falls back to pkg-config (Homebrew on macOS).
Distribution FFmpeg packages are usually GPL-configured and pull in many libraries (x264, x265, gnutls, ...), so they are not used for
the shipped binary.
