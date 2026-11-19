#pragma once
#include "../process/control.h"

namespace sacred {
class Startup;
// msvcrt.dll entries imported by the translated Granny module. Inputs of the _CI* helpers are
// the guest x87 stack, exactly as the MSVC compiler-intrinsic convention defines them.
class Crt {
public:
    Crt(Control& control, const Loader& loader, Startup& startup)
        : control_(control), loader_(loader), startup_(startup) {}
    void bind();

private:
    Control& control_;
    const Loader& loader_;
    Startup& startup_;
};
}
