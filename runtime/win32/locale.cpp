#include "startup.h"
#include "../../platform/calendar.h"
#include <algorithm>
#include <string>

// NLS date/time/locale imports (GetDateFormatA, GetTimeFormatA, GetLocaleInfoA, ...).
// The guest profile is Windows 7 en-US (GetVersion/GetACP agree), so only that locale is
// carried; any other LCID is an unqualified input and faults instead of guessing.
namespace sacred {
namespace {
constexpr uint32_t error_invalid_parameter = 87, error_insufficient_buffer = 122, error_invalid_flags = 1004;
constexpr uint32_t date_short = 1, date_long = 2, date_yearmonth = 8;
constexpr uint32_t time_nominutesorseconds = 1, time_noseconds = 2, time_notimemarker = 4, time_force24 = 8;
constexpr uint32_t locale_return_number = 0x20000000;

const char* const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* const month_names[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};

struct SystemTime {
    uint16_t year, month, weekday, day, hour, minute, second, millisecond;
};

bool leap_year(uint32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint32_t days_in_month(uint32_t year, uint32_t month) {
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap_year(year) ? 29 : days[month - 1];
}

bool valid_date(const SystemTime& t) {
    return t.year >= 1601 && t.year <= 30827 && t.month >= 1 && t.month <= 12 && t.day >= 1 &&
           t.day <= days_in_month(t.year, t.month);
}

bool valid_time(const SystemTime& t) {
    return t.hour < 24 && t.minute < 60 && t.second < 60 && t.millisecond < 1000;
}

// Days since 1601-01-01 (the FILETIME epoch) of a proleptic Gregorian civil date.
int64_t days_since_1601(uint32_t year, uint32_t month, uint32_t day) {
    const int64_t y = int64_t(year) - (month <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468 + 134774;
}

uint16_t weekday_of(const SystemTime& t) {
    return uint16_t((days_since_1601(t.year, t.month, t.day) + 1) % 7);
}

std::string number(uint32_t value, unsigned width) {
    std::string text = std::to_string(value);
    return text.size() < width ? std::string(width - text.size(), '0') + text : text;
}

// Renders a Win32 date or time format picture. Date pictures recognise d M y g; time pictures
// recognise h H m s t. Single-quoted runs are literal and '' inside or outside them is a quote.
std::string render(const std::string& picture, const SystemTime& t, bool time_picture) {
    std::string out;
    for (size_t i = 0; i < picture.size();) {
        const char c = picture[i];
        if (c == '\'') {
            ++i;
            if (i < picture.size() && picture[i] == '\'') {
                out += '\'';
                ++i;
                continue;
            }

            while (i < picture.size()) {
                if (picture[i] == '\'') {
                    if (i + 1 < picture.size() && picture[i + 1] == '\'') {
                        out += '\'';
                        i += 2;
                        continue;
                    }

                    ++i;
                    break;
                }

                out += picture[i++];
            }

            continue;
        }

        const bool token = time_picture ? std::string("hHmst").find(c) != std::string::npos
                                        : std::string("dMyg").find(c) != std::string::npos;
        if (!token) {
            out += c;
            ++i;
            continue;
        }

        size_t n = 1;
        while (i + n < picture.size() && picture[i + n] == c) {
            ++n;
        }

        n = std::min<size_t>(n, c == 'g' || c == 'h' || c == 'H' || c == 'm' || c == 's' || c == 't' ? 2 : 4);
        i += n;
        switch (c) {
        case 'd':
            out += n == 1 ? number(t.day, 1)
                   : n == 2
                       ? number(t.day, 2)
                       : std::string(n == 3 ? std::string(day_names[t.weekday]).substr(0, 3) : day_names[t.weekday]);
            break;
        case 'M':
            out += n == 1   ? number(t.month, 1)
                   : n == 2 ? number(t.month, 2)
                   : n == 3 ? std::string(month_names[t.month - 1]).substr(0, 3)
                            : std::string(month_names[t.month - 1]);
            break;
        case 'y':
            out += n == 1 ? number(t.year % 100, 1) : n == 2 ? number(t.year % 100, 2) : number(t.year, 4);
            break;
        case 'g':
            out += "A.D.";
            break;
        case 'h': {
            const uint32_t h = t.hour % 12 ? t.hour % 12 : 12;
            out += number(h, n);
            break;
        }
        case 'H':
            out += number(t.hour, n);
            break;
        case 'm':
            out += number(t.minute, n);
            break;
        case 's':
            out += number(t.second, n);
            break;
        case 't':
            out += n == 1 ? std::string(1, t.hour < 12 ? 'A' : 'P') : std::string(t.hour < 12 ? "AM" : "PM");
            break;
        }
    }

    return out;
}

const struct {
    uint32_t type;
    const char* value;
} locale_strings[] = {
    {0x01, "0409"},
    {0x02, "English (United States)"},
    {0x03, "ENU"},
    {0x04, "English"},
    {0x05, "1"},
    {0x06, "United States"},
    {0x07, "USA"},
    {0x08, "United States"},
    {0x09, "0409"},
    {0x0a, "1"},
    {0x0b, "437"},
    {0x0c, ","},
    {0x0d, "1"},
    {0x0e, "."},
    {0x0f, ","},
    {0x10, "3;0"},
    {0x11, "2"},
    {0x12, "1"},
    {0x13, "1"},
    {0x14, "0123456789"},
    {0x15, "$"},
    {0x16, "USD"},
    {0x17, "."},
    {0x18, ","},
    {0x19, "3;0"},
    {0x1a, "2"},
    {0x1b, "0"},
    {0x1c, "0"},
    {0x1d, "/"},
    {0x1e, ":"},
    {0x1f, "M/d/yyyy"},
    {0x20, "dddd, MMMM dd, yyyy"},
    {0x21, "0"},
    {0x22, "0"},
    {0x23, "0"},
    {0x25, "0"},
    {0x26, "0"},
    {0x27, "0"},
    {0x28, "AM"},
    {0x29, "PM"},
    {0x59, "en"},
    {0x5a, "US"},
    {0x1001, "English"},
    {0x1002, "United States"},
    {0x1003, "h:mm:ss tt"},
    {0x1004, "1252"},
};

// LOCALE_NEUTRAL/USER_DEFAULT/SYSTEM_DEFAULT all resolve to the single carried en-US locale.
void require_locale(uint32_t lcid) {
    if (lcid != 0 && lcid != 0x400 && lcid != 0x800 && lcid != 0x409) {
        throw GuestFault("unqualified locale " + std::to_string(lcid));
    }
}
}

bool Startup::imported(const char* dll, const char* symbol) const {
    return std::any_of(loader_.hle_slots().begin(), loader_.hle_slots().end(),
                       [&](const HleSlot& slot) { return slot.dll == dll && slot.symbol == symbol; });
}

void Startup::bind_locale() {
    auto bind = [&](const char* symbol, Signature signature, HostFunction function) {
        if (imported("kernel32.dll", symbol)) {
            control_.host(loader_.resolve("kernel32.dll", symbol), std::move(signature), std::move(function),
                          std::string("kernel32.dll!") + symbol);
        }
    };
    const PointerDirection in = PointerDirection::In, out = PointerDirection::Out;
    // Win32 string-output contract shared by the NLS getters: cch == 0 queries the size (including the
    // terminator) without writing; a short buffer fails with ERROR_INSUFFICIENT_BUFFER and writes nothing.
    auto deliver = [this](const std::string& text, uint32_t buffer, int32_t capacity) {
        const uint32_t required = uint32_t(text.size()) + 1;
        if (capacity < 0 || (capacity > 0 && !buffer)) {
            return Result{error(error_invalid_parameter), {}};
        }

        if (capacity == 0) {
            return Result{required, {}};
        }

        if (uint32_t(capacity) < required) {
            return Result{error(error_insufficient_buffer), {}};
        }

        put_ansi(buffer, text);
        return Result{required, {}};
    };
    auto format = [this, deliver](bool time_picture) {
        return [this, deliver, time_picture](Control& c, const std::vector<uint64_t>& a) {
            require_locale(uint32_t(a[0]));
            const uint32_t flags = uint32_t(a[1]);
            const bool custom = a[3] != 0;
            if (time_picture) {
                // Assumption (Microsoft documentation): TIME_* flags only shape the locale's own picture,
                // so combining them with an explicit picture is ERROR_INVALID_FLAGS.
                if ((flags & ~15u) || (custom && flags)) {
                    return Result{error(error_invalid_flags), {}};
                }
            } else {
                if (flags & ~0x7fu) {
                    return Result{error(error_invalid_flags), {}};
                }

                const uint32_t layout = flags & (date_short | date_long | date_yearmonth);
                if ((layout & (layout - 1)) || (custom && layout)) {
                    return Result{error(error_invalid_flags), {}};
                }

                if (flags & ~(date_short | date_long | date_yearmonth)) {
                    throw GuestFault("unqualified date format flags " + std::to_string(flags));
                }
            }

            SystemTime t{};
            if (a[2]) {
                uint8_t bytes[16];
                c.memory.read(uint32_t(a[2]), bytes, sizeof bytes);
                uint16_t fields[8];
                for (unsigned i = 0; i < 8; ++i) {
                    fields[i] = uint16_t(bytes[i * 2] | bytes[i * 2 + 1] << 8);
                }

                t = {fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]};
            } else {
                const int64_t ticks = int64_t(file_time()) - 116444736000000000ll;
                time_t seconds = ticks / 10000000;
                tm local{};
                if (!calendar_local(&seconds, &local)) {
                    throw GuestFault("cannot convert native local time");
                }

                t = {uint16_t(local.tm_year + 1900), uint16_t(local.tm_mon + 1),      0,
                     uint16_t(local.tm_mday),        uint16_t(local.tm_hour),         uint16_t(local.tm_min),
                     uint16_t(local.tm_sec),         uint16_t((ticks / 10000) % 1000)};
            }

            if (time_picture ? !valid_time(t) : !valid_date(t)) {
                return Result{error(error_invalid_parameter), {}};
            }

            std::string picture;
            if (custom) {
                picture = ansi(uint32_t(a[3]));
            } else if (time_picture) {
                if (flags & time_force24) {
                    picture = flags & time_nominutesorseconds ? "HH" : flags & time_noseconds ? "HH:mm" : "HH:mm:ss";
                } else {
                    picture = flags & time_nominutesorseconds ? "h" : flags & time_noseconds ? "h:mm" : "h:mm:ss";
                }

                if (!(flags & time_force24) && !(flags & time_notimemarker)) {
                    picture += " tt";
                }
            } else {
                picture = flags & date_long        ? "dddd, MMMM dd, yyyy"
                          : flags & date_yearmonth ? "MMMM, yyyy"
                                                   : "M/d/yyyy";
            }

            if (!time_picture) {
                t.weekday = weekday_of(t);
            }

            return deliver(render(picture, t, time_picture), uint32_t(a[4]), int32_t(a[5]));
        };
    };
    const Signature formatter{Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, in}, {4, in}, {4, out}, {4}}, ""};
    bind("GetDateFormatA", formatter, format(false));
    bind("GetTimeFormatA", formatter, format(true));
    bind("GetLocaleInfoA", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}, {4, out}, {4}}, ""},
         [deliver](Control&, const std::vector<uint64_t>& a) {
             require_locale(uint32_t(a[0]));
             if (a[1] & locale_return_number) {
                 throw GuestFault("unqualified numeric locale query");
             }

             const uint32_t type = uint32_t(a[1]) & 0xffff;
             for (const auto& entry : locale_strings) {
                 if (entry.type == type) {
                     return deliver(entry.value, uint32_t(a[2]), int32_t(a[3]));
                 }
             }

             throw GuestFault("unqualified locale information type " + std::to_string(type));
         });
    bind("GetUserDefaultLCID", {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
         [](Control&, const std::vector<uint64_t>&) { return Result{0x409, {}}; });
    bind("IsValidLocale", {Convention::Stdcall, ReturnChannel::Eax, {{4}, {4}}, ""},
         [](Control&, const std::vector<uint64_t>& a) {
             if ((a[1] != 1 && a[1] != 2) || a[0] != 0x409) {
                 throw GuestFault("unqualified locale validity query");
             }

             return Result{1, {}};
         });
    bind("IsValidCodePage", {Convention::Stdcall, ReturnChannel::Eax, {{4}}, ""},
         [](Control&, const std::vector<uint64_t>& a) {
             if (a[0] != 1252 && a[0] != 437 && a[0] != 65001 && a[0] != 20127) {
                 throw GuestFault("unqualified code page " + std::to_string(a[0]));
             }

             return Result{1, {}};
         });
    bind("GetOEMCP", {Convention::Stdcall, ReturnChannel::Eax, {}, ""},
         [](Control&, const std::vector<uint64_t>&) { return Result{437, {}}; });
    bind("SystemTimeToFileTime", {Convention::Stdcall, ReturnChannel::Eax, {{4, in}, {4, out}}, ""},
         [this](Control& c, const std::vector<uint64_t>& a) {
             uint8_t bytes[16];
             c.memory.read(uint32_t(a[0]), bytes, sizeof bytes);
             uint16_t f[8];
             for (unsigned i = 0; i < 8; ++i) {
                 f[i] = uint16_t(bytes[i * 2] | bytes[i * 2 + 1] << 8);
             }

             const SystemTime t{f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]};
             if (!valid_date(t) || !valid_time(t)) {
                 return Result{error(error_invalid_parameter), {}};
             }

             const uint64_t seconds =
                 uint64_t(days_since_1601(t.year, t.month, t.day)) * 86400 + t.hour * 3600u + t.minute * 60u + t.second;
             const uint64_t ticks = seconds * 10000000ull + uint64_t(t.millisecond) * 10000ull;
             c.memory.store32(uint32_t(a[1]), uint32_t(ticks));
             c.memory.store32(uint32_t(a[1]) + 4, uint32_t(ticks >> 32));
             return Result{1, {}};
         });
}
}
