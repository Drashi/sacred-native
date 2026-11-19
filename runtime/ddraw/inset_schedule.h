#pragma once

namespace sacred {
// Which presents of the primary surface show the 1024x768 inset that the screen hooks raise (DirectDraw::inset_word).
// A screen raises the word while it draws a frame, so the present that ends the frame finds it raised. A guest frame
// may present more than once (a Blt and an Unlock of the primary surface): a present that follows another with nothing
// drawn in between shows that same frame again and keeps its inset. Any present after new drawing (a scene, a blit
// or unlock into another surface) is a new frame and shows the inset only if that frame raised it, so the inset ends
// with the last frame of its screen: the first world frame after a loading screen, the world map, the options or the
// save game window is shown whole, not inside the screen's 4:3 box.
class InsetSchedule {
public:
    struct Present {
        bool on = false;    // this present shows the inset
        bool fresh = false; // raised for this present (not the repeat present of a frame)
    };
    // The guest drew into something other than the primary surface: the next present ends a new frame.
    void drew() { drawn_ = true; }
    // Whether the next present may show the inset (it then needs the client size); `raised`: the word is raised.
    bool pending(bool raised) const { return raised || (shown_ && !drawn_); }
    // One present of the primary surface; `raised`: the word was found raised (and has been cleared).
    Present present(bool raised) {
        Present result;
        if (raised) {
            result = {true, true};
        } else if (shown_ && !drawn_) {
            result = {true, false};
        }

        shown_ = result.on;
        drawn_ = false;
        return result;
    }

private:
    bool shown_ = false; // the last present showed the inset
    bool drawn_ = false; // drawing since the last present
};
}
