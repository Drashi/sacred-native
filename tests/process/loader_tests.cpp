#include "runtime/process/loader.h"
#include "platform/digest.h"
#include <algorithm>
#include <cstring>
#include <iostream>

using namespace sacred;
static unsigned checks = 0;
static size_t host_page = 0;
static void require(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        throw std::runtime_error("FAIL: " + what);
    }
}

template <class F> static void fault(F operation, const std::string& what) {
    try {
        operation();
    } catch (const GuestFault&) {
        ++checks;
        return;
    }

    throw std::runtime_error("FAIL: expected guest fault: " + what);
}

static std::string hex(const std::array<uint8_t, 32>& digest) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (uint8_t byte : digest) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }

    return text;
}

static uint32_t u32(const std::vector<uint8_t>& bytes, size_t p) {
    return bytes.at(p) | uint32_t{bytes.at(p + 1)} << 8 | uint32_t{bytes.at(p + 2)} << 16 |
           uint32_t{bytes.at(p + 3)} << 24;
}

static uint16_t u16(const std::vector<uint8_t>& bytes, size_t p) {
    return bytes.at(p) | uint16_t(bytes.at(p + 1)) << 8;
}

static void put(std::vector<uint8_t>& bytes, size_t p, uint32_t value, unsigned n = 4) {
    for (unsigned i = 0; i < n; ++i) {
        bytes.at(p + i) = value >> (i * 8);
    }
}

static void text(std::vector<uint8_t>& bytes, size_t p, const std::string& value) {
    std::copy(value.begin(), value.end(), bytes.begin() + p);
    bytes.at(p + value.size()) = 0;
}

static std::vector<uint8_t> fixture(uint32_t base, bool dll,
                                    const std::vector<std::pair<std::string, std::string>>& imports, bool tls = true) {
    std::vector<uint8_t> b(0x4000);
    put(b, 0, 0x5a4d, 2);
    put(b, 0x3c, 0x80);
    put(b, 0x80, 0x4550);
    put(b, 0x84, 0x14c, 2);
    put(b, 0x86, 2, 2);
    put(b, 0x94, 224, 2);
    put(b, 0x96, dll ? 0x2102 : 0x102, 2);
    const size_t o = 0x98, s = o + 224;
    put(b, o, 0x10b, 2);
    put(b, o + 16, 0x1000);
    put(b, o + 28, base);
    put(b, o + 32, 4096);
    put(b, o + 36, 512);
    put(b, o + 56, 0x6000);
    put(b, o + 60, 0x1000);
    put(b, o + 92, 16);
    text(b, s, ".text");
    put(b, s + 8, 0x1000);
    put(b, s + 12, 0x1000);
    put(b, s + 16, 0x1000);
    put(b, s + 20, 0x1000);
    put(b, s + 36, 0x60000020);
    text(b, s + 40, ".data");
    put(b, s + 48, 0x3000);
    put(b, s + 52, 0x2000);
    put(b, s + 56, 0x2000);
    put(b, s + 60, 0x2000);
    put(b, s + 76, 0xc0000040);
    std::fill(b.begin() + 0x1000, b.begin() + 0x2000, 0xcc);
    auto directory = [&](unsigned index, uint32_t rva, uint32_t length) {
        put(b, o + 96 + index * 8, rva);
        put(b, o + 100 + index * 8, length);
    };
    if (!imports.empty()) {
        directory(1, 0x2100, (imports.size() + 1) * 20);
        for (size_t i = 0; i < imports.size(); ++i) {
            const size_t p = 0x2100 + i * 20;
            const uint32_t name = 0x2c00 + i * 128, source = 0x2d00 + i * 32;
            const uint32_t iat = 0x2d80 + i * 32, hint = 0x2e00 + i * 128;
            put(b, p, source);
            put(b, p + 12, name);
            put(b, p + 16, iat);
            text(b, name, imports[i].first);
            const auto& symbol = imports[i].second;
            const uint32_t value = symbol[0] == '#' ? 0x80000000u | std::stoul(symbol.substr(1)) : hint;
            if (symbol[0] != '#') {
                text(b, hint + 2, symbol);
            }

            put(b, source, value);
            put(b, iat, value);
        }
    }

    directory(0, 0x2300, 0x100);
    put(b, 0x2310, 7);
    put(b, 0x2314, 2);
    put(b, 0x2318, 2);
    put(b, 0x231c, 0x2330);
    put(b, 0x2320, 0x2340);
    put(b, 0x2324, 0x2350);
    put(b, 0x2330, 0x1010);
    put(b, 0x2334, 0x2380);
    put(b, 0x2340, 0x2360);
    put(b, 0x2344, 0x2370);
    put(b, 0x2350, 0, 2);
    put(b, 0x2352, 1, 2);
    text(b, 0x2360, "ByName");
    text(b, 0x2370, "Forward");
    text(b, 0x2380, "kernel32.Sleep");
    directory(5, 0x2400, 24);
    put(b, 0x2400, 0x2000);
    put(b, 0x2404, 24);
    if (tls) {
        directory(9, 0x2500, 24);
        put(b, 0x2500, base + 0x2600);
        put(b, 0x2504, base + 0x2604);
        put(b, 0x2508, base + 0x2610);
        put(b, 0x250c, base + 0x2620);
        put(b, 0x2510, 12);
        put(b, 0x2600, 0x44332211);
        put(b, 0x2620, base + 0x1010);
        put(b, 0x2624, base + 0x1020);
        const unsigned sites[] = {0x3500, 0x3504, 0x3508, 0x350c, 0x3620, 0x3624};
        for (unsigned i = 0; i < 6; ++i) {
            put(b, 0x2408 + i * 2, sites[i], 2);
        }
    }

    put(b, 0x2700, base + 0x1234); // Pointer-looking integer, NOT a relocation.
    return b;
}

static void map_fixture(Loader& l, const std::string& name, uint32_t base, const std::vector<uint8_t>& bytes) {
    l.map_image(bytes, {name, base, sha256(bytes)});
}

// Guest pages stay 4 KiB; the host page only sizes backing storage. Guest
// neighbours that share a host page must keep their bytes and permissions.
static void host_page_tests() {
    Memory m(host_page);
    const size_t expected = host_page ? host_page : m.arena().host_page();
    require(m.arena().host_page() == expected, "arena uses the injected host page size");
    require(reinterpret_cast<uintptr_t>(m.arena().base()) % expected == 0, "arena base is host-page aligned");
    const uint32_t base = 0x00400000, other = base + Memory::page_size;
    m.reserve(base, 3 * Memory::page_size, "t");
    m.commit(base + Memory::page_size, Memory::page_size, Read | Write, "t");
    m.store32(base + Memory::page_size, 0xa5a5a5a5);
    m.protect(base + Memory::page_size, Memory::page_size, Read, "t");
    m.commit(base + 2 * Memory::page_size, Memory::page_size, Read | Write, "t");
    require(m.arena().last_commit_begin() % expected == 0 && m.arena().last_commit_end() % expected == 0 &&
                m.arena().last_commit_end() - m.arena().last_commit_begin() >= expected,
            "commit extent covers whole host pages");
    require(m.load32(other) == 0xa5a5a5a5 && m.permissions(other) == Read, "neighbour commit preserves page");
    m.decommit(base + 2 * Memory::page_size, Memory::page_size, "t");
    require(m.load32(other) == 0xa5a5a5a5 && m.permissions(other) == Read, "neighbour decommit preserves page");
    fault([&] { m.store32(other, 1); }, "checked permission stays authoritative inside a host page");
    fault([&] { m.load32(base + 2 * Memory::page_size); }, "decommitted guest page faults");
    m.release(base, "t");
}

static void memory_tests() {
    Memory m(host_page);
    uint8_t bytes[16]{};
    require(m.host_address(0x12345678) - m.host_address(0) == 0x12345678, "base plus offset");
    require(m.host_address(0) > UINT32_MAX, "arena is not low-address identity mapping");
    fault([&] { m.read(0, bytes, 1); }, "null");
    fault([&] { m.read(0xffffffff, bytes, 2); }, "wide access wraps IA-32");
    fault([&] { m.reserve(0, 4096, "bad"); }, "null allocation");
    m.reserve(0x10000, 8192, "owner");
    fault([&] { m.reserve(0x11000, 4096, "collision"); }, "collision");
    fault([&] { m.commit(0x10000, 4096, Read, "other"); }, "allocation ownership");
    fault([&] { m.read(0x10000, bytes, 1); }, "reserved not committed");
    m.commit(0x10000, 4096, Read | Write, "owner");
    m.store32(0x10001, 0xabcdef12);
    require(m.load32(0x10001) == 0xabcdef12, "unaligned little-endian scalar");
    fault([&] { m.read(0x10ffc, bytes, 8); }, "64-bit cross-page check");
    fault([&] { m.read(0x10ff8, bytes, 16); }, "128-bit cross-page check");
    fault([&] { m.write(0x10ff8, bytes, 16); }, "HLE copy checks entire range");
    m.commit(0x11000, 4096, Read, "owner");
    require(m.load32(0x11000) == 0, "zero commit");
    fault([&] { m.store32(0x11000, 1); }, "guest read-only permission");
    m.protect(0x11000, 4096, Read | Write, "owner");
    m.store32(0x11000, 0x13579bdf);
    m.protect(0x11000, 4096, Read, "owner");
    fault([&] { m.check(0x10000, 1, Execute); }, "guest execute permission");
    m.protect(0x10000, 4096, Read, "owner");
    fault([&] { m.store32(0x10000, 1); }, "protect");
    m.decommit(0x10000, 4096, "owner");
    fault([&] { m.load32(0x10000); }, "decommit");
    require(m.load32(0x11000) == 0x13579bdf, "decommit preserves adjacent guest page");
    m.commit(0x10000, 4096, Read | Write, "owner");
    require(m.load32(0x10001) == 0, "recommit clears previous contents");
    require(m.load32(0x11000) == 0x13579bdf, "recommit preserves adjacent guest page");
    fault([&] { m.store32(0x11000, 1); }, "neighbor retains independent protection");
    fault([&] { m.release(0x10000, "other"); }, "foreign release");
    m.release(0x10000, "owner");
    fault([&] { m.load32(0x10000); }, "release");
    m.reserve(0xfffff000, 4096, "last");
    m.commit(0xfffff000, 4096, Read | Write, "last");
    m.store32(0xfffffffc, 0x12345678);
    require(m.load32(0xfffffffc) == 0x12345678, "last IA-32 page remains accessible");
    fault([&] { m.reserve(0xfffff000, 8192, "overflow"); }, "reservation overflow");
    Heap heap(m);
    const uint32_t block = heap.allocate(7);
    require(block == layout::heap && m.load32(block) == 0, "heap range and zeroing");
    m.store32(block, 123);
    fault([&] { heap.free(block + 1); }, "heap base ownership");
    heap.free(block);
    require(heap.allocate(7) == block && m.load32(block) == 0, "heap reuse zeroing");
    fault([&] { heap.allocate(0x30000000); }, "heap range limit");
    {
        Thread a(m, 0, {}), b(m, 1, {});
        require(a.stack_top != b.stack_top && a.teb != b.teb, "per-thread stack and TEB");
        require(m.load32(a.teb) == 0xffffffff && m.load32(a.teb + 0x18) == a.fs_base(), "TEB fields");
        require(m.load32(a.teb + 4) == a.stack_top && m.load32(a.teb + 8) == a.stack_limit, "TEB stack bounds");
        require(m.load32(a.teb + 0x2c) == a.tls_vector, "TEB TLS vector");
        fault([&] { m.load32(a.stack_limit - 4); }, "stack guard stays uncommitted");
        fault([&] { Thread duplicate(m, 0, {}); }, "thread allocation collision");
    }

    fault([&] { m.load32(layout::tebs); }, "thread teardown");
}

static void fixture_tests() {
    Memory m(host_page);
    Loader l(m);
    auto exe = fixture(layout::exe, false, {{"a.dll", "ByName"}, {"KERNEL32.dll", "Sleep"}});
    auto a = fixture(0x10000000, true, {{"b.dll", "#7"}});
    auto b = fixture(0x10000000, true, {});
    map_fixture(l, "sacred.exe", layout::exe, exe);
    map_fixture(l, "a.dll", 0x10000000, a);
    map_fixture(l, "b.dll", 0x11000000, b);
    require(m.load32(0x11002700) == 0x10001234, "do not relocate pointer-looking integers");
    require(m.load32(0x11002620) == 0x11001010, "HIGHLOW relocates TLS callback operand");
    {
        Thread unbound_thread(m, 0, {});
        fault([&] { l.initialize(unbound_thread, {}); }, "cannot initialize before binding");
    }

    l.bind();
    require(l.resolve("B.DLL", "#7") == 0x11001010, "ordinal translated export");
    require(l.resolve("a.dll", "ByName") == 0x10001010, "named translated export");
    require(l.resolve("b.dll", "Forward") == l.resolve("kernel32", "Sleep"), "export forwarder");
    require(l.hle_slots().size() == 1, "HLE tokens deduplicated by DLL and symbol");
    fault([&] { l.unsupported_call(l.hle_slots()[0].token); }, "missing API is not success");
    fault([&] { m.load32(l.hle_slots()[0].token); }, "HLE tokens are not executable bytes");
    fault([&] { m.store32(layout::exe, 0); }, "headers become read-only");
    fault([&] { m.store32(layout::exe + 0x1000, 0); }, "original code becomes read-only");
    require(m.load32(layout::exe + 0x4000) == 0, "virtual section tail zero filled");
    fault([&] { m.load32(layout::exe + 0x5000); }, "image hole remains uncommitted");
    Thread thread(m, 0, l.tls_templates());
    Thread second(m, 1, l.tls_templates());
    require(l.tls_templates().size() == 3, "TLS template per module");
    for (size_t i = 0; i < l.tls_templates().size(); ++i) {
        const uint32_t pointer = m.load32(thread.tls_vector + i * 4);
        require(m.load32(pointer) == 0x44332211 && m.load32(pointer + 4) == 0 && m.load32(pointer + 12) == 0,
                "TLS copy and zero-fill");
        require(m.load32(second.tls_vector + i * 4) != pointer, "TLS ownership per thread");
        require(m.load32(l.modules()[i].tls_index_address) == i, "PE TLS index publication");
    }

    std::vector<std::string> calls;
    const uint32_t entry = l.initialize(thread, [&](const InitCall& c) {
        require(c.reason == 1 && c.reserved == 0, "process attach arguments");
        require(c.instance == (c.module == "b.dll"   ? 0x11000000u
                               : c.module == "a.dll" ? 0x10000000u
                                                     : layout::exe),
                "module instance");
        const bool tls = c.kind == InitKind::TlsCallback;
        const unsigned offset = c.target - c.instance;
        require(offset == (tls ? (calls.size() % 3 == 0 ? 0x1010u : 0x1020u) : 0x1000u),
                "relocated callback and DLL entry targets");
        calls.push_back(c.module + (tls ? ":tls" : ":entry"));
        // Executable native fixture writes observable guest state at every call.
        m.store32(thread.stack_limit, calls.size());
        return true;
    });
    const std::vector<std::string> expected{"b.dll:tls", "b.dll:tls",   "b.dll:entry",    "a.dll:tls",
                                            "a.dll:tls", "a.dll:entry", "sacred.exe:tls", "sacred.exe:tls"};
    require(calls == expected && m.load32(thread.stack_limit) == 8, "dependency/TLS/DllMain/EXE TLS order");
    require(entry == layout::exe + 0x1000, "returns original PE entry, not WinMain");
    fault([&] { l.initialize(thread, [](const InitCall&) { return true; }); }, "no repeated attach");
    fault([&] { l.bind(); }, "no repeated import bind");
    std::cout << "FIXTURE_INIT_ORDER b.tls,b.tls,b.entry,a.tls,a.tls,a.entry,exe.tls,exe.tls\n";
}

static void negative_tests() {
    const auto exe = fixture(layout::exe, false, {}, false);
    {
        Memory m(host_page);
        Loader l(m);
        fault([&] { l.map_image(exe, {"sacred.exe", layout::exe, "wrong"}); }, "hash admission");
        fault([&] { map_fixture(l, "sacred.exe", 0x500000, exe); }, "fixed EXE base");
        map_fixture(l, "sacred.exe", layout::exe, exe);
        fault([&] { map_fixture(l, "collision.dll", layout::exe, fixture(layout::exe, true, {}, false)); },
              "module-base collision");
    }

    {
        Memory m(host_page);
        Loader l(m);
        auto dll = fixture(layout::granny, true, {}, false);
        put(dll, 0x96, 0x2103, 2); // IMAGE_FILE_RELOCS_STRIPPED
        fault([&] { map_fixture(l, "a.dll", layout::tincat, dll); }, "stripped DLL rebase");
        dll = fixture(layout::granny, true, {}, false);
        put(dll, 0x98 + 96 + 5 * 8, 0);
        put(dll, 0x98 + 100 + 5 * 8, 0);
        fault([&] { map_fixture(l, "a.dll", layout::tincat, dll); }, "missing DLL relocations");
    }

    {
        Memory m(host_page);
        Loader l(m);
        map_fixture(l, "sacred.exe", layout::exe, exe);
        map_fixture(l, "a.dll", layout::granny, fixture(layout::granny, true, {}, false));
        l.bind();
        Thread thread(m, 0, l.tls_templates());
        unsigned calls = 0;
        fault(
            [&] {
                l.initialize(thread, [&](const InitCall&) {
                    ++calls;
                    return false;
                });
            },
            "attach failure");
        require(calls == 1, "attach failure stops initialization");
    }
}

// Independent PE layout reconstruction: no Loader section/relocation metadata is
// used to build the expected bytes. A disabled section copy must fail this test.
static void real_image_tests(const std::string& assets) {
    Memory m(host_page);
    Loader l(m);
    size_t highlow_total = 0;
    for (size_t index = 0; index < sacred_modules.size(); ++index) {
        const auto& spec = sacred_modules[index];
        const std::string filename = index == 0 ? "Sacred.exe" : spec.name;
        const auto bytes = read_image(assets + "/" + filename);
        l.map_image(bytes, spec);
        require(m.arena().host_page() == (host_page ? host_page : m.arena().host_page()),
                "image mapped with injected host page");
        const size_t pe = u32(bytes, 0x3c), opt = pe + 24;
        const size_t sections = opt + u16(bytes, pe + 20);
        const uint32_t image_size = u32(bytes, opt + 56), preferred = u32(bytes, opt + 28);
        std::vector<uint8_t> expected(image_size);
        std::copy_n(bytes.begin(), u32(bytes, opt + 60), expected.begin());
        const unsigned count = u16(bytes, pe + 6);
        for (unsigned j = 0; j < count; ++j) {
            const size_t s = sections + j * 40;
            const uint32_t rva = u32(bytes, s + 12), raw = u32(bytes, s + 16);
            std::copy_n(bytes.begin() + u32(bytes, s + 20), raw, expected.begin() + rva);
        }

        const uint32_t reloc = u32(bytes, opt + 96 + 5 * 8), size = u32(bytes, opt + 100 + 5 * 8);
        size_t relocated = 0;
        for (uint32_t p = reloc; p < reloc + size;) {
            const uint32_t page = u32(expected, p), length = u32(expected, p + 4);
            require(length >= 8 && length <= reloc + size - p, "reference relocation block");
            for (uint32_t q = p + 8; q < p + length; q += 2) {
                const uint16_t fixup = u16(expected, q);
                if (!(fixup >> 12)) {
                    continue;
                }

                require((fixup >> 12) == 3, "reference HIGHLOW policy");
                const uint32_t rva = page + (fixup & 0xfff);
                put(expected, rva, u32(expected, rva) + (spec.base - preferred));
                ++relocated;
            }

            p += length;
        }

        // Compare all mapped raw bytes and every zero-fill byte, not just a marker.
        for (unsigned j = 0; j < count; ++j) {
            const size_t s = sections + j * 40;
            const uint32_t rva = u32(bytes, s + 12);
            const uint32_t length = std::max(u32(bytes, s + 8), u32(bytes, s + 16));
            std::vector<uint8_t> actual(length);
            m.read(spec.base + rva, actual.data(), length);
            require(std::equal(actual.begin(), actual.end(), expected.begin() + rva),
                    spec.name + " complete mapped section bytes and zero tail");
        }

        std::vector<uint8_t> headers(u32(bytes, opt + 60));
        m.read(spec.base, headers.data(), headers.size());
        require(std::equal(headers.begin(), headers.end(), bytes.begin()), "original PE headers");
        const auto& module = l.modules().back();
        require(module.relocations.size() == relocated, "all and only real PE relocation records");
        require(module.directories[9].first == 0 && module.directories[9].second == 0, "no real TLS directory");
        if (index == 0) {
            require(module.base == 0x00400000 && module.entry_rva == 0x44d964 && module.image_size == 0x196c000,
                    "measured fixed EXE layout");
            require(module.imports.size() == 389, "all 389 original EXE imports");
            require(module.relocations.empty(), "EXE relocation policy");
        } else {
            require(module.entry_rva == (index == 1 ? 0x57abau : 0x26d66u), "real DLL entry RVA");
            require(relocated > 0, "real DLL relocations admitted");
        }

        highlow_total += relocated;
        std::cout << "IMAGE " << spec.name << " sha256=" << module.hash << " base=0x" << std::hex << module.base
                  << " entry_rva=0x" << module.entry_rva << std::dec << " sections=" << count
                  << " imports=" << module.imports.size() << " highlow=" << relocated << " tls=absent\n";
    }

    l.bind();
    size_t imports = 0, translated = 0;
    for (const auto& module : l.modules()) {
        for (const auto& i : module.imports) {
            require(m.load32(module.base + i.iat_rva) == l.resolve(i.dll, i.symbol), "real IAT target binding");
            if (i.target < layout::hle) {
                ++translated;
                require(i.dll == "granny.dll" || i.dll == "tincat2.dll", "translated import provenance");
                m.check(i.target, 1, Execute);
            }

            ++imports;
        }

        require(m.permissions(module.base) == Read, "real headers read-only");
        for (const auto& section : module.sections) {
            require(m.permissions(module.base + section.rva) == section.permissions, "real section permissions");
            if (section.name == ".rsrc") {
                require(module.directories[2].first >= section.rva, "resources retain PE directory RVA");
                require(m.load32(module.base + section.rva + 12) != 0, "resource directory data mapped");
            }
        }
    }

    require(imports > 389 && translated > 0 && !l.hle_slots().empty(), "full translated module import closure");
    for (const auto& slot : l.hle_slots()) {
        fault([&] { l.unsupported_call(slot.token); }, "real unsupported HLE slot traps");
    }

    const uint32_t hp = static_cast<uint32_t>(m.arena().host_page());
    require(l.modules().size() == sacred_modules.size(), "all three original images are loaded");
    for (const auto& loaded : l.modules()) {
        const Module* module = &loaded;
        const std::string spec_name = module->name;
        const uint64_t image_end = uint64_t{module->base} + module->image_size;
        size_t mixed = 0;
        for (uint64_t host = module->base; host < image_end; host += hp) {
            const uint64_t last = std::min<uint64_t>(host + hp, image_end);
            for (uint64_t p = host + Memory::page_size; p < last; p += Memory::page_size) {
                if (m.permissions(p) != m.permissions(host)) {
                    ++mixed;
                    break;
                }
            }
        }

        if (hp > Memory::page_size) {
            require(mixed > 0, spec_name + " has mixed guest permissions inside one host page");
        }
    }

    Thread thread(m, 0, l.tls_templates());
    Heap heap(m);
    const uint32_t receipt = heap.allocate(8);
    std::vector<uint32_t> calls;
    const uint32_t entry = l.initialize(thread, [&](const InitCall& c) {
        require(c.kind == InitKind::DllEntry && c.reason == 1 && !c.reserved, "real attach fixture ABI");
        require(c.target == c.instance + (c.module == "granny.dll" ? 0x57aba : 0x26d66), "real attach target");
        m.store32(receipt + calls.size() * 4, c.target);
        calls.push_back(c.target);
        return true;
    });
    require(calls == std::vector<uint32_t>{0x10057aba, 0x11026d66}, "real module initialization fixture order");
    require(m.load32(receipt) == 0x10057aba && m.load32(receipt + 4) == 0x11026d66,
            "native DLL fixtures executed observable writes");
    require(entry == 0x0084d964 && l.tls_templates().empty(), "ready at actual EXE entry without invented TLS");
    std::cout << "REAL_INIT_FIXTURES granny@10057aba,tincat@11026d66 PE_ENTRY=0084d964\n"
              << "BINDINGS imports=" << imports << " translated=" << translated << " hle_slots=" << l.hle_slots().size()
              << " highlow=" << highlow_total << "\n";
}

static void digest_tests() {
    // FIPS 180-4 padding edges (55/56/63/64/119 bytes), "abc" and a multi-block input; expected values from Python.
    require(hex(host_sha256(reinterpret_cast<const uint8_t*>("abc"), 3)) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256 abc");
    const struct {
        size_t length;
        const char* digest;
    } runs[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"},
        {56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"},
        {63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"},
        {64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"},
        {119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"},
        {1000003, "cf5f310cac13bb0f1de288d09940e8ee441e8d5d88b0fd4ec6021c405fda8c8c"},
    };
    for (const auto& run : runs) {
        const std::vector<uint8_t> bytes(run.length, uint8_t('a'));
        require(hex(host_sha256(bytes.data(), bytes.size())) == run.digest,
                "sha256 of " + std::to_string(run.length) + " bytes");
    }
}

int main(int argc, char** argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        std::string assets;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--assets" && i + 1 < args.size()) {
                assets = args[++i];
            } else if (args[i] == "--host-page" && i + 1 < args.size()) {
                host_page = std::stoul(args[++i]);
            } else {
                throw std::runtime_error("Usage: process_tests [--host-page BYTES] [--assets INSTALLATION]");
            }
        }

        if (assets.empty()) {
            digest_tests();
            memory_tests();
            fixture_tests();
            negative_tests();
            host_page_tests();
        } else {
            real_image_tests(assets);
        }

        std::cout << "PROCESS_PASS checks=" << checks << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
