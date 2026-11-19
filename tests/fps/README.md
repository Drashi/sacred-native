# Frame rate checks (FPS_LIMIT)

Checks that the game runs at the same speed whatever the frame rate limit (`FPS_LIMIT` in Settings.cfg, or
`SACRED_FPS_LIMIT`, see `runtime/modules/resolution.h`). Needs a `SACRED_QA` build in `build/game`.

- `SACRED_QA_FRAME_DT_US=D` makes the guest clock virtual once the world runs: it advances D microseconds per
  present, so the game sees exactly 1e6/D FPS however fast the host renders.
  Menus and loading keep host time.
- `SACRED_QA_PPOS=1` (set by `run.sh`) logs the local player's position and the guest time at every present to
  `<evidence>/ppos.csv`.

`run.sh LABEL SCRIPT_FPS [ENV=VALUE...]` starts a new Dwarf game from `gen.py` (four held-button walks whose lengths
are given in seconds of game time; SCRIPT_FPS converts them to frames) and `analyze.py LABEL...` prints the walking
speed per run and fails when the first two walks differ by more than 1% between runs. Each run gets a fresh profile
with the installation's own `Settings.cfg`, windowed and without sound or movies:

    tests/fps/run.sh m60 60 SACRED_QA_FRAME_DT_US=16667
    tests/fps/run.sh m120 120 SACRED_FPS_LIMIT=120 SACRED_QA_FRAME_DT_US=8333
    tests/fps/run.sh m144 144 SACRED_FPS_LIMIT=144 SACRED_QA_FRAME_DT_US=6944
    python3 tests/fps/analyze.py m60 m120 m144

Example result (Linux, llvmpipe): 288.4, 288.6 and 288.6 position units per second (spread 0.07%).
Without `SACRED_QA_FRAME_DT_US` the runs use the real clock; `median_dt` then shows the frame rate the host reached.
