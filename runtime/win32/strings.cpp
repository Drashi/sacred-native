#include "startup.h"
#include <algorithm>

namespace sacred {
void Startup::bind_strings() {
    auto imported = [this](const char* symbol) {
        return std::any_of(loader_.hle_slots().begin(), loader_.hle_slots().end(),
                           [&](const HleSlot& slot) { return slot.dll == "kernel32.dll" && slot.symbol == symbol; });
    };
    auto bind = [&](const char* symbol, Signature signature, HostFunction function) {
        if (imported(symbol)) {
            control_.host(loader_.resolve("kernel32.dll", symbol), std::move(signature), std::move(function),
                          std::string("kernel32.dll!") + symbol);
        }
    };
    const PointerDirection in = PointerDirection::In, out = PointerDirection::Out;
    bind("lstrlenA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             return Result{a[0] ? uint32_t(ansi(uint32_t(a[0])).size()) : 0, {}};
         });
    bind("lstrcpyA", {Convention::Stdcall, ReturnChannel::Eax, {{4, out}, {4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0] || !a[1]) {
                 throw GuestFault("lstrcpyA with a null pointer");
             }

             put_ansi(uint32_t(a[0]), ansi(uint32_t(a[1])));
             return Result{a[0], {}};
         });
    bind("lstrcpynA", {Convention::Stdcall, ReturnChannel::Eax, {{4, out}, {4, in}, {4}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0] || !a[1]) {
                 throw GuestFault("lstrcpynA with a null pointer");
             }

             const int32_t limit = int32_t(a[2]);
             if (limit <= 0) {
                 return Result{0, {}};
             }

             std::string value = ansi(uint32_t(a[1]));
             value.resize(std::min<size_t>(value.size(), size_t(limit) - 1));
             put_ansi(uint32_t(a[0]), value);
             return Result{a[0], {}};
         });
    bind("lstrcatA", {Convention::Stdcall, ReturnChannel::Eax, {{4, PointerDirection::InOut}, {4, in}}, ""},
         [this](Control&, const std::vector<uint64_t>& a) {
             if (!a[0] || !a[1]) {
                 throw GuestFault("lstrcatA with a null pointer");
             }

             std::string head = ansi(uint32_t(a[0]));
             put_ansi(uint32_t(a[0]) + uint32_t(head.size()), ansi(uint32_t(a[1])));
             return Result{a[0], {}};
         });
    auto compare = [this](bool fold) {
        return [this, fold](Control&, const std::vector<uint64_t>& a) {
            std::string left = a[0] ? ansi(uint32_t(a[0])) : std::string();
            std::string right = a[1] ? ansi(uint32_t(a[1])) : std::string();
            for (const std::string* text : {&left, &right}) {
                for (unsigned char character : *text) {
                    if (character >= 0x80) {
                        throw GuestFault("unqualified non-ASCII string comparison");
                    }
                }
            }

            if (fold) {
                for (auto* text : {&left, &right}) {
                    for (char& character : *text) {
                        character = char(std::tolower(static_cast<unsigned char>(character)));
                    }
                }
            }

            const int order = left.compare(right);
            return Result{uint32_t(order < 0 ? -1 : order > 0 ? 1 : 0), {}};
        };
    };
    bind("lstrcmpA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, in}}, ""}, compare(false));
    bind("lstrcmpiA", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, in}}, ""}, compare(true));
}
}
