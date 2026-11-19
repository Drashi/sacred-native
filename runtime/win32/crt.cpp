#include "crt.h"
#include "startup.h"
#include <cmath>
#include <vector>

namespace sacred {
namespace {
// Pops st(0); a two-operand _CI helper takes the second operand from st(1) afterwards.
double operand(X86& cpu) {
    const double value = x87_to_double(x87_get(&cpu, 0));
    fdrop(&cpu);
    return value;
}
}

void Crt::bind() {
    const Signature intrinsic{Convention::Cdecl, ReturnChannel::St0, {}, ""};
    // fmod(st1, st0) -> st0
    control_.host(
        loader_.resolve("msvcrt.dll", "_CIfmod"), intrinsic,
        [](Control& c, const std::vector<uint64_t>&) {
            const double divisor = operand(c.cpu), dividend = operand(c.cpu);
            return Result{0, x87_from_double(std::fmod(dividend, divisor))};
        },
        "msvcrt.dll!_CIfmod");
    // pow(st1, st0) -> st0
    control_.host(
        loader_.resolve("msvcrt.dll", "_CIpow"), intrinsic,
        [](Control& c, const std::vector<uint64_t>&) {
            const double exponent = operand(c.cpu), base = operand(c.cpu);
            return Result{0, x87_from_double(std::pow(base, exponent))};
        },
        "msvcrt.dll!_CIpow");
    control_.host(
        loader_.resolve("msvcrt.dll", "_CIacos"), intrinsic,
        [](Control& c, const std::vector<uint64_t>&) { return Result{0, x87_from_double(std::acos(operand(c.cpu)))}; },
        "msvcrt.dll!_CIacos");
    // st0 truncated toward zero to a signed 64-bit integer in EDX:EAX.
    control_.host(
        loader_.resolve("msvcrt.dll", "_ftol"), {Convention::Cdecl, ReturnChannel::EdxEax, {}, ""},
        [](Control& c, const std::vector<uint64_t>&) {
            const X87Value value = x87_get(&c.cpu, 0);
            const int64_t integer = x87_to_integer(&c.cpu, value, 64, 1);
            fdrop(&c.cpu);
            return Result{uint64_t(integer), {}};
        },
        "msvcrt.dll!_ftol");
    control_.host(
        loader_.resolve("msvcrt.dll", "free"), {Convention::Cdecl, ReturnChannel::Void, {{4}}, ""},
        [this](Control&, const std::vector<uint64_t>& a) {
            if (!startup_.release_crt(uint32_t(a[0]))) {
                throw GuestFault("invalid CRT free");
            }

            return Result{};
        },
        "msvcrt.dll!free");
    control_.host(
        loader_.resolve("msvcrt.dll", "memmove"),
        {Convention::Cdecl, ReturnChannel::Eax, {{4, PointerDirection::Out}, {4, PointerDirection::In}, {4}}, ""},
        [](Control& c, const std::vector<uint64_t>& a) {
            const uint32_t size = uint32_t(a[2]);
            if (size) {
                std::vector<uint8_t> bytes(size);
                c.memory.read(uint32_t(a[1]), bytes.data(), size);
                c.memory.write(uint32_t(a[0]), bytes.data(), size);
            }

            return Result{a[0], {}};
        },
        "msvcrt.dll!memmove");
}
}
