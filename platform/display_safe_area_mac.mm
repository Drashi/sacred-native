#include "display_safe_area.h"
#import <AppKit/AppKit.h>
#include <cmath>

namespace sacred {
// SDL's display bounds are CGDisplayBounds; the NSScreen of the same display carries its CGDirectDisplayID as
// NSScreenNumber. safeAreaInsets (macOS 12) is the area hidden by the camera housing, which a fullscreen window
// does not cover; it is 0 on displays without a notch, where a fullscreen window covers everything.
DisplayInsets display_safe_insets(int x, int y, int width, int height) {
    DisplayInsets insets;
    if (@available(macOS 12.0, *)) {
        @autoreleasepool {
            for (NSScreen* screen in [NSScreen screens]) {
                NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
                if (!number) {
                    continue;
                }

                const CGRect bounds = CGDisplayBounds(CGDirectDisplayID(number.unsignedIntValue));
                if (std::lround(bounds.origin.x) != x || std::lround(bounds.origin.y) != y ||
                    std::lround(bounds.size.width) != width || std::lround(bounds.size.height) != height) {
                    continue;
                }

                const NSEdgeInsets safe = screen.safeAreaInsets;
                insets.top = int(std::lround(safe.top));
                // A fullscreen window on a notched display starts below the menu bar strip, which is a point taller
                // than the notch (33 against 32 on a 14" MacBook Pro: 1512x982 gives a 1512x949 window). The strip
                // is the top of the visible frame while the menu bar shows; with the menu bar hidden only the notch
                // is known.
                if (safe.top > 0) {
                    const CGFloat strip = NSMaxY(screen.frame) - NSMaxY(screen.visibleFrame);
                    if (strip > safe.top) {
                        insets.top = int(std::lround(strip));
                    }
                }
                insets.left = int(std::lround(safe.left));
                insets.bottom = int(std::lround(safe.bottom));
                insets.right = int(std::lround(safe.right));
                break;
            }
        }
    }
    return insets;
}
} // namespace sacred
