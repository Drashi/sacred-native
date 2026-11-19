# Miles NULL-handle behaviour (original `mss32.dll` vs the runtime)

What the game's `mss32.dll` (Miles 6.6g) does when an `_AIL_*` export that `runtime/miles/miles.cpp` implements gets
a NULL handle, and what the runtime does now.

## Scenario that crashed (caller at `Sacred.exe` 0x694a2b)

`FUN_00694510` is a per-object audio-track update, called from 23 sites, all under a window-message dispatch. At
0x694a2b it asks `_AIL_stream_ms_position` for the total and current milliseconds of the stream stored at
`obj+0x5d4`. The failing call passed handle 0: the track slot had no open stream (never started, or already closed
by a music or area transition) while the message handler still ran the update. The game relies on the original
returning without touching the two output words.

## Original behaviour with handle == 0

"No writes" means the output pointers are left untouched. A void export returns with EAX unspecified; the runtime returns 0.

| Entry | Original with handle 0 | Runtime now |
|---|---|---|
| `_AIL_stream_ms_position@12` | returns; **writes neither output** | same (was trap) |
| `_AIL_set_stream_ms_position@8` | returns, no effect | same |
| `_AIL_stream_status@4` | returns **0xFFFFFFFF** | same |
| `_AIL_stream_volume_pan@12` | writes **-1.0f (0xBF800000)** to every non-NULL output pointer | same |
| `_AIL_set_stream_volume_pan@12` | no-op | same |
| `_AIL_set_stream_loop_count@8` | no-op | same |
| `_AIL_start_stream@4`, `_AIL_pause_stream@8`, `_AIL_close_stream@4` | no-op | same |
| `_AIL_open_stream@12` | driver NULL **or** name NULL: returns 0 (no error text) | same |
| `_AIL_init_sample@4`, `_AIL_start_sample@4`, `_AIL_stop_sample@4`, `_AIL_end_sample@4` | no-op | same |
| `_AIL_release_sample_handle@4` | no-op | same |
| `_AIL_set_sample_loop_count@8`, `_AIL_set_sample_playback_rate@8`, `_AIL_set_sample_volume_pan@12` | no-op | same |
| `_AIL_sample_status@4` | returns 0 | same |
| `_AIL_sample_playback_rate@4` | returns 0 | same |
| `_AIL_sample_volume_pan@12` | returns; writes neither output | same |
| `_AIL_register_EOS_callback@8` | returns 0, stores nothing | same |
| `_AIL_set_sample_file@12` | sample NULL **or** image NULL: returns 0 | same |
| `_AIL_set_named_sample_file@20` | sample NULL: if the image starts with an ID3 header or the name ends in `.mp3` the ASI path writes through the NULL sample (access violation); otherwise falls to `set_sample_file` and returns 0 | trap in the ASI case, else 0 |
| `_AIL_WAV_info@8` | data NULL: sets "Not a WAV file.", returns 0, writes nothing | same |
| `_AIL_file_type@8` | data NULL (or size < 8): returns 0 | same |
| `_AIL_decompress_ADPCM@12` | info NULL: returns 0, writes nothing | same |
| `_AIL_mem_free_lock@4` | ptr NULL: no-op | already so |
| `_AIL_allocate_sample_handle@4` | driver NULL: returns 0 | same (was trap) |
| `_AIL_waveOutClose@4` | driver NULL: returns; also returns when no driver is open at all | same |
| `_AIL_enumerate_3D_providers@12` | cursor pointer NULL is accepted (`RIB_enumerate_providers` substitutes a local, starting at 1) | NULL cursor treated as "start" (was trap) |

## Non-NULL handles (unchanged strict traps)

The original dereferences the handle without validation (sample/stream structs at fixed offsets, `+0xc4`, `+0x37c`
in `waveOutClose`, ...). A stale or unknown non-NULL handle is therefore garbage in the original, so every entry keeps its
`GuestFault` for a handle that the mixer or driver table does not know. `_AIL_waveOutClose` with unknown non-NULL
and an open driver also dereferences before it searches its list, so it stays a trap.

Not handle-taking and unchanged: `_AIL_startup@0` (original returns 0 when already started; the runtime still traps on a
repeated call), `_AIL_shutdown@0`, `_AIL_open_digital_driver@16`, `_AIL_set_preference@8`, `_AIL_mem_free_lock@4`.

## Noted, not changed

`RIB_enumerate_providers` writes 0 to the cursor and to its second output when no provider matches; the runtime leaves
those outputs untouched (the existing test pins that). Unverified which guest output slot the original's second write
reaches; no recorded session depends on it.
