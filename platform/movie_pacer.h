#pragma once
namespace sacred {
// Presentation policy for decoded movie frames against a master clock (seconds). Pure; no time source of its own.
struct PacerDecision {
    enum Kind { Wait, Present, Drop } kind;
    double wait_seconds; // Wait only: time until the frame is due
};
// `pts` is the candidate frame; `next_pts` the following decoded frame when `has_next`. A frame is stale (Drop)
// once its successor is already due; otherwise it is held until its own timestamp and then presented.
inline PacerDecision decide_frame(double pts, bool has_next, double next_pts, double clock) {
    if (has_next && next_pts <= clock) {
        return {PacerDecision::Drop, 0};
    }

    if (pts > clock) {
        return {PacerDecision::Wait, pts - clock};
    }

    return {PacerDecision::Present, 0};
}
}
