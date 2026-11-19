#include "loader.h"
#include "../../platform/digest.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace sacred {
static std::string canonical(std::string name) {
    for (char& c : name) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (name.find('.') == std::string::npos) {
        name += ".dll";
    }

    return name;
}

static uint32_t align_page(uint32_t n) {
    if (n > UINT32_MAX - 4095) {
        throw GuestFault("PE size overflow");
    }

    return (n + 4095) & ~4095u;
}

std::vector<uint8_t> read_image(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw GuestFault("Cannot read PE image: " + path);
    }

    const auto size = stream.tellg();
    if (size < 0 || size > 0x20000000) {
        throw GuestFault("Invalid PE file size: " + path);
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        throw GuestFault("Incomplete PE image: " + path);
    }

    return bytes;
}

std::string sha256(const std::vector<uint8_t>& bytes) {
    const auto digest = host_sha256(bytes.data(), bytes.size());
    std::ostringstream text;
    for (uint8_t byte : digest) {
        text << std::hex << std::setfill('0') << std::setw(2) << unsigned{byte};
    }

    return text.str();
}

uint32_t Module::address(uint32_t rva, uint64_t width) const {
    if (rva >= image_size || width > uint64_t{image_size} - rva) {
        throw GuestFault("PE RVA outside " + name);
    }

    return base + rva;
}

Loader::Loader(Memory& memory) : memory_(memory) {
    memory_.reserve(layout::hle, layout::hle_end - layout::hle, "HLE tokens");
    memory_.reserve(layout::returns, Memory::page_size, "Callback returns");
    // Callable tokens remain uncommitted IDs, not x86 code. Opt-in
    // metadata occupies separate pages inside the native-module reservation.
}

Module* Loader::find(const std::string& name) {
    for (auto& module : modules_) {
        if (module.name == canonical(name)) {
            return &module;
        }
    }

    return nullptr;
}

const Module* Loader::find(const std::string& name) const {
    for (const auto& module : modules_) {
        if (module.name == canonical(name)) {
            return &module;
        }
    }

    return nullptr;
}

void Loader::map_file(const std::string& path, const ModuleSpec& spec) {
    map_image(read_image(path), spec);
}

void Loader::map_image(const std::vector<uint8_t>& bytes, const ModuleSpec& spec) {
    if (bound_ || find(spec.name)) {
        throw GuestFault("Duplicate module or already bound loader");
    }

    const std::string hash = sha256(bytes);
    if (hash != spec.sha256) {
        throw GuestFault("Unmanifested module SHA-256: " + spec.name);
    }

    auto u16 = [&](uint64_t p) -> uint16_t {
        if (p + 2 > bytes.size()) {
            throw GuestFault("Truncated PE field");
        }

        return uint16_t(bytes[p]) | uint16_t(bytes[p + 1]) << 8;
    };
    auto u32 = [&](uint64_t p) -> uint32_t {
        return uint32_t{u16(p)} | uint32_t{u16(p + 2)} << 16;
    };
    if (u16(0) != 0x5a4d) {
        throw GuestFault("Missing DOS signature");
    }

    const uint64_t pe = u32(0x3c);
    if (u32(pe) != 0x4550 || u16(pe + 4) != 0x14c || u16(pe + 20) < 224) {
        throw GuestFault("Expected PE32 i386 image");
    }

    const uint64_t opt = pe + 24;
    if (u16(opt) != 0x10b || u32(opt + 32) != Memory::page_size) {
        throw GuestFault("Unsupported PE format or section alignment");
    }

    Module m{};
    m.name = canonical(spec.name);
    m.hash = hash;
    m.base = spec.base;
    m.preferred_base = u32(opt + 28);
    m.entry_rva = u32(opt + 16);
    m.image_size = u32(opt + 56);
    m.headers_size = u32(opt + 60);
    m.dll = (u16(pe + 22) & 0x2000) != 0;
    if (!m.image_size || m.image_size % Memory::page_size || m.base < 0x10000 ||
        uint64_t{m.base} + m.image_size > layout::image_end || !m.headers_size || m.headers_size > bytes.size() ||
        m.headers_size > m.image_size) {
        throw GuestFault("Invalid PE image extent");
    }

    if (!m.dll && (m.base != layout::exe || m.base != m.preferred_base)) {
        throw GuestFault("EXE must stay at its fixed 0x00400000 base");
    }

    if (m.base != m.preferred_base && (u16(pe + 22) & 1)) {
        throw GuestFault("Relocations stripped from rebased image");
    }

    if (m.entry_rva) {
        m.address(m.entry_rva);
    }

    const unsigned directories = std::min(u32(opt + 92), 16u);
    for (unsigned i = 0; i < directories; ++i) {
        m.directories[i] = {u32(opt + 96 + i * 8), u32(opt + 100 + i * 8)};
        // The security directory is a file offset, not an RVA.
        if (i != 4 && m.directories[i].second) {
            m.address(m.directories[i].first, m.directories[i].second);
        }
    }

    const uint64_t table = opt + u16(pe + 20);
    const unsigned count = u16(pe + 6);
    if (!count || table + uint64_t{count} * 40 > m.headers_size) {
        throw GuestFault("Invalid PE section table");
    }

    uint32_t previous_end = align_page(m.headers_size);
    for (unsigned i = 0; i < count; ++i) {
        const uint64_t p = table + i * 40;
        Section s{};
        for (unsigned j = 0; j < 8 && bytes[p + j]; ++j) {
            s.name += char(bytes[p + j]);
        }

        s.virtual_size = u32(p + 8);
        s.rva = u32(p + 12);
        s.raw_size = u32(p + 16);
        s.raw_offset = u32(p + 20);
        s.flags = u32(p + 36);
        s.permissions = ((s.flags & 0x40000000) ? Read : 0) | ((s.flags & 0x80000000) ? Write : 0) |
                        ((s.flags & 0x20000000) ? Execute : 0);
        const uint32_t length = std::max(s.virtual_size, s.raw_size);
        if (!length || s.rva % Memory::page_size || s.rva < previous_end ||
            uint64_t{s.raw_offset} + s.raw_size > bytes.size()) {
            throw GuestFault("Invalid or overlapping PE section");
        }

        m.address(s.rva, align_page(length));
        previous_end = s.rva + align_page(length);
        m.sections.push_back(s);
    }

    memory_.reserve(m.base, m.image_size, m.name);
    try {
        memory_.commit(m.base, align_page(m.headers_size), Read | Write, m.name);
        memory_.write(m.base, bytes.data(), m.headers_size);
        for (const auto& s : m.sections) {
            memory_.commit(m.base + s.rva, align_page(std::max(s.virtual_size, s.raw_size)), Read | Write, m.name);
            memory_.write(m.base + s.rva, bytes.data() + s.raw_offset, s.raw_size);
        }

        const auto reloc = m.directories[5];
        if (m.base != m.preferred_base && !reloc.second) {
            throw GuestFault("Rebased DLL has no PE relocation directory");
        }

        uint64_t p = reloc.first, end = p + reloc.second;
        while (p < end) {
            if (end - p < 8) {
                throw GuestFault("Truncated relocation block");
            }

            const uint32_t page = word(m, p), block_size = word(m, p + 4);
            if (block_size < 8 || block_size % 2 || block_size > end - p) {
                throw GuestFault("Invalid relocation block size");
            }

            for (uint64_t q = p + 8; q < p + block_size; q += 2) {
                uint8_t b[2];
                memory_.read(m.address(q, 2), b, 2);
                const uint16_t entry = uint16_t(b[0]) | uint16_t(b[1]) << 8;
                if (!(entry >> 12)) {
                    continue; // IMAGE_REL_BASED_ABSOLUTE is padding.
                }

                if ((entry >> 12) != 3) {
                    throw GuestFault("Unsupported PE32 relocation type");
                }

                const uint64_t rva = uint64_t{page} + (entry & 0xfff);
                if (rva > UINT32_MAX) {
                    throw GuestFault("Relocation RVA overflow");
                }

                const uint32_t before = word(m, rva);
                const uint32_t after = before + (m.base - m.preferred_base);
                memory_.store32(m.address(rva, 4), after);
                m.relocations.push_back({uint32_t(rva), before, after});
            }

            p += block_size;
        }

        metadata(m);
        modules_.push_back(std::move(m));
    } catch (...) {
        memory_.release(spec.base, canonical(spec.name));
        throw;
    }
}

uint32_t Loader::word(const Module& m, uint32_t rva) const {
    return memory_.load32(m.address(rva, 4));
}

std::string Loader::string(const Module& m, uint32_t rva) const {
    std::string text;
    for (uint64_t p = rva; p < m.image_size; ++p) {
        char c;
        memory_.read(m.address(p), &c, 1);
        if (!c) {
            return text;
        }

        text += c;
    }

    throw GuestFault("Unterminated PE string");
}

void Loader::metadata(Module& m) {
    const auto imports = m.directories[1];
    if (imports.second) {
        bool terminated = false;
        for (uint64_t p = imports.first; p + 20 <= uint64_t{imports.first} + imports.second; p += 20) {
            const uint32_t original = word(m, p), name = word(m, p + 12), iat = word(m, p + 16);
            if (!original && !name && !iat) {
                terminated = true;
                break;
            }

            const std::string dll = canonical(string(m, name));
            for (uint64_t offset = 0;; offset += 4) {
                const uint64_t source = uint64_t{original ? original : iat} + offset;
                const uint64_t slot = uint64_t{iat} + offset;
                if (source >= m.image_size || slot >= m.image_size) {
                    throw GuestFault("Import thunk outside PE image");
                }

                const uint32_t value = word(m, source);
                if (!value) {
                    break;
                }

                const std::string symbol =
                    value & 0x80000000 ? "#" + std::to_string(value & 0xffff) : string(m, value + 2);
                m.imports.push_back({dll, symbol, uint32_t(slot), 0});
            }
        }

        if (!terminated) {
            throw GuestFault("Unterminated import directory");
        }
    }

    const auto exp = m.directories[0];
    if (exp.second) {
        m.address(exp.first, 40);
        const uint32_t first = word(m, exp.first + 16), count = word(m, exp.first + 20);
        const uint32_t named = word(m, exp.first + 24), functions = word(m, exp.first + 28);
        const uint32_t names = word(m, exp.first + 32), ordinals = word(m, exp.first + 36);
        m.address(functions, uint64_t{count} * 4);
        m.address(names, uint64_t{named} * 4);
        m.address(ordinals, uint64_t{named} * 2);
        std::vector<Export> entries;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t rva = word(m, functions + i * 4);
            Export target{rva, {}};
            if (rva >= exp.first && uint64_t{rva} < uint64_t{exp.first} + exp.second) {
                target.forwarder = string(m, rva);
            } else if (rva) {
                m.address(rva);
            }

            entries.push_back(target);
            if (rva) {
                m.exports.emplace("#" + std::to_string(uint64_t{first} + i), target);
            }
        }

        for (uint32_t i = 0; i < named; ++i) {
            uint8_t b[2];
            memory_.read(m.address(ordinals + i * 2, 2), b, 2);
            const unsigned index = unsigned{b[0]} | unsigned{b[1]} << 8;
            if (index >= entries.size() || !entries[index].rva) {
                throw GuestFault("Invalid export ordinal");
            }

            m.exports.emplace(string(m, word(m, names + i * 4)), entries[index]);
        }
    }
}

uint32_t Loader::resolve(const std::string& dll, const std::string& symbol) const {
    std::set<std::string> visiting;
    return resolve(dll, symbol, visiting);
}

uint32_t Loader::resolve(const std::string& dll, const std::string& symbol, std::set<std::string>& visiting) const {
    const std::string name = canonical(dll);
    if (!visiting.insert(name + "!" + symbol).second) {
        throw GuestFault("Cyclic export forwarder");
    }

    if (const Module* module = find(name)) {
        auto it = module->exports.find(symbol);
        if (it == module->exports.end()) {
            throw GuestFault("Missing translated export: " + name + "!" + symbol);
        }

        if (it->second.forwarder.empty()) {
            return module->address(it->second.rva);
        }

        const auto& forwarder = it->second.forwarder;
        const auto dot = forwarder.rfind('.');
        if (dot == std::string::npos) {
            throw GuestFault("Invalid export forwarder");
        }

        return resolve(forwarder.substr(0, dot), forwarder.substr(dot + 1), visiting);
    }

    for (const auto& module : native_) {
        if (module.name == name) {
            auto entry = module.exports.find(symbol);
            if (entry != module.exports.end()) {
                return entry->second;
            }
        }
    }

    for (const auto& slot : hle_) {
        if (slot.dll == name && slot.symbol == symbol) {
            return slot.token;
        }
    }

    throw GuestFault("Unregistered module export: " + name + "!" + symbol);
}

void Loader::enable_native_catalog() {
    if (bound_) {
        throw GuestFault("Native catalog must precede import binding");
    }

    native_catalog_ = true;
}

uint32_t Loader::module_handle(const std::string& name) const {
    if (name.empty()) {
        for (const auto& m : modules_) {
            if (!m.dll) {
                return m.base;
            }
        }

        return 0;
    }

    for (const auto& m : modules_) {
        if (m.name == canonical(name)) {
            return m.base;
        }
    }

    for (const auto& m : native_) {
        if (m.name == canonical(name)) {
            return m.base;
        }
    }

    return 0;
}

std::string Loader::module_name(uint32_t handle) const {
    if (!handle) {
        handle = module_handle("");
    }

    for (const auto& m : modules_) {
        if (m.base == handle) {
            return m.name;
        }
    }

    for (const auto& m : native_) {
        if (m.base == handle) {
            return m.name;
        }
    }

    return {};
}

void Loader::build_native_catalog() {
    // Each 64 KiB native image owns 32 KiB of metadata and 32 KiB of
    // uncommitted function tokens. No guest address contains host code.
    std::sort(hle_.begin(), hle_.end(),
              [](const HleSlot& a, const HleSlot& b) { return std::tie(a.dll, a.symbol) < std::tie(b.dll, b.symbol); });
    for (auto& slot : hle_) {
        if (native_.empty() || native_.back().name != slot.dll) {
            native_.push_back({slot.dll, layout::hle + uint32_t(native_.size()) * 0x10000, {}});
        }

        auto& module = native_.back();
        if (module.base >= layout::hle_end - 0x10000 || module.exports.size() >= 2048) {
            throw GuestFault("Native module catalog exhausted");
        }

        slot.token = module.base + 0x8000 + uint32_t(module.exports.size()) * layout::token_stride;
        if (slot.dll == "msvcrt.dll" && slot.symbol == "_adjust_fdiv") {
            slot.token = module.base + 0x7000;
        }

        module.exports.emplace(slot.symbol, slot.token);
    }

    for (auto& module : native_) {
        std::vector<uint8_t> bytes(0x8000);
        auto w16 = [&](uint32_t p, uint16_t v) {
            bytes.at(p) = uint8_t(v);
            bytes.at(p + 1) = uint8_t(v >> 8);
        };
        auto w32 = [&](uint32_t p, uint32_t v) {
            w16(p, uint16_t(v));
            w16(p + 2, uint16_t(v >> 16));
        };
        uint32_t text = 0x5000;
        auto string = [&](const std::string& value) {
            uint32_t rva = text;
            if (text + value.size() + 1 > 0x7000) {
                throw GuestFault("Native export strings exhausted");
            }

            std::copy(value.begin(), value.end(), bytes.begin() + text);
            text += uint32_t(value.size()) + 1;
            return rva;
        };
        std::map<uint32_t, uint32_t> ordinals;
        for (const auto& e : module.exports) {
            if (e.first[0] == '#') {
                ordinals.emplace(std::stoul(e.first.substr(1)), e.second);
            }
        }

        uint32_t ordinal = 1, named = 0;
        std::vector<std::pair<std::string, uint32_t>> aliases;
        for (const auto& e : module.exports) {
            if (e.first[0] != '#') {
                while (ordinals.count(ordinal)) {
                    ++ordinal;
                }

                ordinals.emplace(ordinal, e.second);
                aliases.emplace_back("#" + std::to_string(ordinal), e.second);
                w32(0x3000 + named * 4, string(e.first));
                w16(0x4000 + named * 2, uint16_t(ordinal - 1));
                ++ordinal;
                ++named;
            }
        }

        if (ordinals.empty() || ordinals.rbegin()->first > 1024) {
            throw GuestFault("Native export ordinal range exhausted");
        }

        for (const auto& alias : aliases) {
            module.exports.emplace(alias);
        }

        for (const auto& e : ordinals) {
            w32(0x2000 + (e.first - 1) * 4, e.second - module.base);
        }

        w16(0, 0x5a4d);
        w16(2, 0x90);
        w16(4, 3);
        w16(8, 4);
        w16(12, 0xffff);
        w16(16, 0xb8);
        w16(24, 0x40);
        w32(0x3c, 0x80);
        w32(0x80, 0x4550);
        w16(0x84, 0x14c);
        w16(0x86, 2);
        w16(0x94, 224);
        w16(0x96, 0x2102);
        w16(0x98, 0x10b);
        w32(0xb4, module.base);
        w32(0x9c, 0x8000);
        w32(0xa0, 0x7000);
        w32(0xac, 0x8000);
        w32(0xb0, 0x1000);
        w32(0xb8, 0x1000);
        w32(0xbc, 0x200);
        w16(0xc0, 4);
        w16(0xc8, 4);
        w32(0xd0, 0x10000);
        w32(0xd4, 0x1000);
        w16(0xdc, 2);
        w32(0xf4, 16);
        w32(0xf8, 0x1000);
        w32(0xfc, 0x6000);
        w32(0xe0, 0x100000);
        w32(0xe4, 0x1000);
        w32(0xe8, 0x100000);
        w32(0xec, 0x1000);
        const char names[][8] = {".edata", ".tokens"};
        for (unsigned i = 0; i < 2; ++i) {
            uint32_t p = 0x178 + i * 40;
            std::copy(names[i], names[i] + 8, bytes.begin() + p);
            w32(p + 8, i ? 0x8000 : 0x7000);
            w32(p + 12, i ? 0x8000 : 0x1000);
            w32(p + 16, i ? 0 : 0x7000);
            w32(p + 20, i ? 0 : 0x1000);
            w32(p + 36, i ? 0x60000020 : 0x40000040);
        }

        w32(0x100c, string(module.name));
        w32(0x1010, 1);
        w32(0x1014, ordinals.rbegin()->first);
        w32(0x1018, named);
        w32(0x101c, 0x2000);
        w32(0x1020, 0x3000);
        w32(0x1024, 0x4000);
        memory_.commit(module.base, bytes.size(), Read | Write, "HLE tokens");
        memory_.write(module.base, bytes.data(), bytes.size());
        memory_.protect(module.base, bytes.size(), Read, "HLE tokens");
    }
}

void Loader::bind() {
    if (bound_) {
        throw GuestFault("Loader already bound");
    }

    // Register the complete imported HLE catalog before resolving forwarders.
    auto register_hle = [&](const std::string& dll, const std::string& symbol) {
        if (find(dll)) {
            return;
        }

        for (const auto& slot : hle_) {
            if (slot.dll == dll && slot.symbol == symbol) {
                return;
            }
        }

        const uint64_t token = layout::hle + hle_.size() * uint64_t{layout::token_stride};
        if (token >= layout::hle_end) {
            throw GuestFault("HLE token range exhausted");
        }

        hle_.push_back({dll, symbol, uint32_t(token)});
    };
    for (const auto& m : modules_) {
        for (const auto& i : m.imports) {
            register_hle(i.dll, i.symbol);
        }

        for (const auto& e : m.exports) {
            if (!e.second.forwarder.empty()) {
                const auto dot = e.second.forwarder.rfind('.');
                if (dot == std::string::npos) {
                    throw GuestFault("Invalid export forwarder");
                }

                register_hle(canonical(e.second.forwarder.substr(0, dot)), e.second.forwarder.substr(dot + 1));
            }
        }
    }

    if (native_catalog_) {
        for (const char* name :
             {"InitializeCriticalSectionAndSpinCount", "FlsAlloc", "FlsGetValue", "FlsSetValue", "FlsFree"}) {
            register_hle("kernel32.dll", name);
        }

        for (const char* name : {"GetFullPathNameW"}) {
            register_hle("kernel32.dll", name);
        }

        for (const char* name : {"DrawTextW", "wsprintfW", "GetWindowLongA", "SetWindowLongA"}) {
            register_hle("user32.dll", name);
        }

        for (const char* name : {"TextOutW", "GetKerningPairsW", "GetTextExtentPoint32W"}) {
            register_hle("gdi32.dll", name);
        }

        build_native_catalog();
    }

    for (auto& m : modules_) {
        for (auto& i : m.imports) {
            i.target = resolve(i.dll, i.symbol);
            memory_.store32(m.address(i.iat_rva, 4), i.target);
        }

        const auto tls = m.directories[9];
        if (tls.second) {
            if (tls.second < 24) {
                throw GuestFault("Truncated PE TLS directory");
            }

            const uint32_t start = word(m, tls.first), end = word(m, tls.first + 4);
            m.tls_index_address = word(m, tls.first + 8);
            const uint32_t callbacks = word(m, tls.first + 12), zero = word(m, tls.first + 16);
            if (start < m.base || end < start) {
                throw GuestFault("Invalid TLS template");
            }

            m.address(start - m.base, uint64_t{end} - start);
            m.address(m.tls_index_address - m.base, 4);
            TlsTemplate t{uint32_t(tls_.size()), std::vector<uint8_t>(end - start), zero};
            memory_.read(start, t.bytes.data(), t.bytes.size());
            memory_.store32(m.tls_index_address, t.index);
            tls_.push_back(std::move(t));
            if (callbacks) {
                for (uint64_t p = callbacks;; p += 4) {
                    if (p < m.base || p - m.base >= m.image_size) {
                        throw GuestFault("TLS callback table outside module");
                    }

                    const uint32_t callback = word(m, p - m.base);
                    if (!callback) {
                        break;
                    }

                    m.address(callback - m.base);
                    m.tls_callbacks.push_back(callback);
                }
            }
        }

        memory_.protect(m.base, align_page(m.headers_size), Read, m.name);
        for (const auto& s : m.sections) {
            memory_.protect(m.base + s.rva, align_page(std::max(s.virtual_size, s.raw_size)), s.permissions, m.name);
        }
    }

    bound_ = true;
}

void Loader::unsupported_call(uint32_t token) const {
    for (const auto& slot : hle_) {
        if (slot.token == token) {
            throw GuestFault("Unsupported HLE import: " + slot.dll + "!" + slot.symbol);
        }
    }

    throw GuestFault("Unknown HLE token");
}

uint32_t Loader::initialize(Thread& thread, const std::function<bool(const InitCall&)>& executor) {
    if (!bound_ || init_started_) {
        throw GuestFault("Invalid loader initialization state");
    }

    if (memory_.load32(thread.teb + 0x2c) != thread.tls_vector) {
        throw GuestFault("Thread TLS vector not installed");
    }

    init_started_ = true;
    std::set<std::string> visiting, initialized;
    std::function<void(const Module&)> attach = [&](const Module& m) {
        if (initialized.count(m.name) || visiting.count(m.name)) {
            return;
        }

        visiting.insert(m.name);
        for (const auto& i : m.imports) {
            if (const Module* dep = find(i.dll)) {
                attach(*dep);
            }
        }

        auto invoke = [&](InitKind kind, uint32_t target) {
            memory_.check(target, 1, Execute);
            if (!executor({kind, m.name, target, m.base, 1, 0})) {
                throw GuestFault("DLL initialization failed: " + m.name);
            }
        };
        for (uint32_t callback : m.tls_callbacks) {
            invoke(InitKind::TlsCallback, callback);
        }

        if (m.dll && m.entry_rva) {
            invoke(InitKind::DllEntry, m.address(m.entry_rva));
        }

        initialized.insert(m.name);
        visiting.erase(m.name);
    };
    const Module* exe = nullptr;
    for (const auto& m : modules_) {
        if (!m.dll) {
            if (exe) {
                throw GuestFault("Multiple process executable modules");
            }

            exe = &m;
        }
    }

    if (!exe) {
        throw GuestFault("No process executable mapped");
    }

    for (const auto& m : modules_) {
        if (m.dll) {
            attach(m);
        }
    }

    attach(*exe);
    return exe->address(exe->entry_rva); // Return PE entry; never synthesize WinMain.
}
}
