#pragma once
#include <ctime>
#include <cstdint>
#include <locale.h>

namespace sacred {
bool calendar_local(const time_t* seconds, tm* result);
bool calendar_utc(const time_t* seconds, tm* result);
time_t calendar_epoch(tm* utc);
long calendar_offset(const tm& local);

class UnicodeLocale {
public:
    UnicodeLocale();
    ~UnicodeLocale();
    UnicodeLocale(const UnicodeLocale&) = delete;
    UnicodeLocale& operator=(const UnicodeLocale&) = delete;
    uint16_t map(uint16_t value, bool lower) const;
    uint16_t properties(uint16_t value) const;

private:
    locale_t locale_;
};
}
