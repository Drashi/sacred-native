#include "calendar.h"
#include "../runtime/process/memory.h"
#include <cerrno>
#include <cwctype>
#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace sacred {
bool calendar_local(const time_t* seconds, tm* result) {
    return localtime_r(seconds, result) != nullptr;
}

bool calendar_utc(const time_t* seconds, tm* result) {
    return gmtime_r(seconds, result) != nullptr;
}

time_t calendar_epoch(tm* utc) {
    errno = 0;
    time_t result = timegm(utc);
    // Only a (time_t)-1 result reports failure: Apple's timegm leaves errno == EOVERFLOW after successful calls.
    if (result == time_t(-1) && errno == EOVERFLOW) {
        throw GuestFault("native UTC calendar overflow");
    }

    return result;
}

long calendar_offset(const tm& local) {
    return local.tm_gmtoff;
}

UnicodeLocale::UnicodeLocale() {
#ifdef __APPLE__
    const char* name = "UTF-8";
#else
    const char* name = "C.UTF-8";
#endif
    locale_ = newlocale(LC_CTYPE_MASK, name, nullptr);
    if (!locale_) {
        throw GuestFault("Unicode classification locale unavailable");
    }
}

UnicodeLocale::~UnicodeLocale() {
    freelocale(locale_);
}

uint16_t UnicodeLocale::map(uint16_t value, bool lower) const {
    return uint16_t(lower ? towlower_l(value, locale_) : towupper_l(value, locale_));
}

uint16_t UnicodeLocale::properties(uint16_t c) const {
    uint16_t result = 0x200 | (iswupper_l(c, locale_) ? 1 : 0) | (iswlower_l(c, locale_) ? 2 : 0) |
                      (iswdigit_l(c, locale_) ? 4 : 0) | (iswspace_l(c, locale_) ? 8 : 0) |
                      (iswpunct_l(c, locale_) ? 16 : 0) | (iswcntrl_l(c, locale_) ? 32 : 0) |
                      ((c == 9 || c == 32 || c == 0xa0) ? 64 : 0) | (iswxdigit_l(c, locale_) ? 128 : 0) |
                      (iswalpha_l(c, locale_) ? 256 : 0);
#ifdef __APPLE__
    // glibc's C.UTF-8 classes match Windows GetStringTypeW on the code page 1252 repertoire.
    // Apple's UTF-8 classes differ in these places, measured against the same reference:
    // symbols (Sc/Sk/Sm/So, e.g. U+00A2 U+00A9 U+00D7) are not punct, and a few Latin-1 letters lack flags.
    if (iswgraph_l(c, locale_) && !iswalnum_l(c, locale_)) {
        result |= 16; // POSIX punct: graph and not alnum
    }

    switch (c) {
    case 0xb2:
    case 0xb3:
    case 0xb9:
        result |= 4 | 16;
        break; // superscript digits
    case 0xaa:
    case 0xb5:
    case 0xba:
        result |= 2 | 16;
        break; // ordinal indicators, micro sign
    case 0xad:
        result |= 16;
        break; // soft hyphen
    default:
        break;
    }
#endif
    return result;
}
}
