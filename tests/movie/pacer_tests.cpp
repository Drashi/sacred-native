// Playback-timing unit test: drives the pacing policy against a simulated master clock (no real time, no devices).
#include "../../platform/movie_pacer.h"
#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

struct Outcome {
    std::vector<double> shown_pts, shown_at;
    int dropped = 0;
};
// Plays `count` frames at `fps`. The clock advances only through waits; `stall_at` frames take `stall` extra seconds
// after being presented (a slow guest), which the audio clock keeps counting.
Outcome play(int count, double fps, int stall_at, double stall) {
    Outcome out;
    double clock = 0;
    for (int i = 0; i < count; ++i) {
        for (;;) {
            double pts = i / fps, next = (i + 1) / fps;
            auto decision = sacred::decide_frame(pts, i + 1 < count, next, clock);
            if (decision.kind == sacred::PacerDecision::Wait) {
                clock += decision.wait_seconds;
                continue;
            }

            if (decision.kind == sacred::PacerDecision::Drop) {
                ++out.dropped;
                break;
            }

            out.shown_pts.push_back(pts);
            out.shown_at.push_back(clock);
            if (i == stall_at) {
                clock += stall;
            }

            break;
        }
    }

    return out;
}
}

int main() {
    auto smooth = play(250, 25.0, -1, 0);
    check(smooth.dropped == 0 && smooth.shown_pts.size() == 250, "on-time playback must show every frame");
    double worst = 0;
    for (size_t i = 0; i < smooth.shown_pts.size(); ++i) {
        worst = std::fmax(worst, std::fabs(smooth.shown_at[i] - smooth.shown_pts[i]));
    }

    check(worst < 1e-9, "on-time frames are presented exactly when due");
    std::printf("smooth: shown=%zu dropped=%d worst_av_error=%.6f\n", smooth.shown_pts.size(), smooth.dropped, worst);

    auto stalled = play(250, 25.0, 50, 0.31); // 310 ms hiccup = 7.75 frames
    // Frames 51..56 have a successor due by 2.31 s and are dropped; frame 57 (2.28 s) is shown 30 ms late.
    check(stalled.dropped == 6, "a 310 ms stall at 25 fps drops exactly the six stale frames");
    check(stalled.shown_pts.size() + stalled.dropped == 250, "every frame is either shown or dropped");
    worst = 0;
    bool ordered = true, never_early = true;
    for (size_t i = 0; i < stalled.shown_pts.size(); ++i) {
        worst = std::fmax(worst, std::fabs(stalled.shown_at[i] - stalled.shown_pts[i]));
        never_early &= stalled.shown_at[i] + 1e-9 >= stalled.shown_pts[i];
        if (i) {
            ordered &= stalled.shown_pts[i] > stalled.shown_pts[i - 1];
        }
    }

    check(ordered && never_early, "frames are in order and never presented before their timestamp");
    check(worst < 1.0 / 25.0, "after resync every presented frame is within one frame interval of the audio clock");
    std::printf("stalled: shown=%zu dropped=%d worst_av_error=%.6f\n", stalled.shown_pts.size(), stalled.dropped,
                worst);

    check(sacred::decide_frame(1.0, true, 1.04, 0.5).kind == sacred::PacerDecision::Wait, "early frame waits");
    check(std::fabs(sacred::decide_frame(1.0, true, 1.04, 0.5).wait_seconds - 0.5) < 1e-12, "wait equals time to due");
    check(sacred::decide_frame(1.0, false, 0, 5.0).kind == sacred::PacerDecision::Present,
          "last frame is never dropped");
    check(sacred::decide_frame(1.0, true, 1.04, 1.04).kind == sacred::PacerDecision::Drop,
          "frame whose successor is due is dropped");
    std::printf("%s\n", failures ? "MOVIE_PACER_FAIL" : "MOVIE_PACER_OK");
    return failures ? 1 : 0;
}
