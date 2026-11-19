#pragma once
#include "memory.h"
#include <array>
#include <functional>
#include <set>

namespace sacred {
struct ModuleSpec {
    std::string name;
    uint32_t base;
    std::string sha256;
};
// Assigned before generation. No runtime rebasing of this layout is allowed.
inline const std::array<ModuleSpec, 3> sacred_modules{
    {{"sacred.exe", layout::exe, "db9b86a44c8c1cc8c5ed9e3f427121ab8b4ed62760861e58995f10af9d971e74"},
     {"granny.dll", layout::granny, "9ac73f4aa5cc5f0096bac3bc79b299bcae0e839d08a470fc14b924142c643d68"},
     {"tincat2.dll", layout::tincat, "f134adde22cc50052dd0abd9fcc1dcea2807f0733dee45f099e1119b644ed137"}}};
std::vector<uint8_t> read_image(const std::string& path);
std::string sha256(const std::vector<uint8_t>& bytes);

struct Section {
    std::string name;
    uint32_t rva, virtual_size, raw_size, raw_offset, flags;
    uint8_t permissions;
};
struct Import {
    std::string dll, symbol; // Ordinals have the canonical form "#123".
    uint32_t iat_rva, target = 0;
};
struct Export {
    uint32_t rva;
    std::string forwarder;
};
struct Relocation {
    uint32_t rva, before, after;
};
struct Module {
    std::string name, hash;
    uint32_t base, preferred_base, image_size, entry_rva, headers_size;
    bool dll;
    std::array<std::pair<uint32_t, uint32_t>, 16> directories{};
    std::vector<Section> sections;
    std::vector<Import> imports;
    std::map<std::string, Export> exports;
    std::vector<Relocation> relocations;
    std::vector<uint32_t> tls_callbacks;
    uint32_t tls_index_address = 0;
    uint32_t address(uint32_t rva, uint64_t width = 1) const;
};
struct HleSlot {
    std::string dll, symbol;
    uint32_t token;
    // Catalog presence is not implementation. Unbound callable slots fail strictly.
};
struct NativeModule {
    std::string name;
    uint32_t base;
    std::map<std::string, uint32_t> exports;
};
enum class InitKind { TlsCallback, DllEntry };
struct InitCall {
    InitKind kind;
    std::string module;
    uint32_t target, instance;
    uint32_t reason = 1, reserved = 0; // DLL_PROCESS_ATTACH
};

class Loader {
public:
    explicit Loader(Memory& memory);
    void map_file(const std::string& path, const ModuleSpec& spec);
    void map_image(const std::vector<uint8_t>& bytes, const ModuleSpec& spec);
    void bind();
    void enable_native_catalog();
    uint32_t module_handle(const std::string& name) const;
    std::string module_name(uint32_t handle) const;
    uint32_t resolve(const std::string& dll, const std::string& symbol) const;
    void unsupported_call(uint32_t token) const;
    // The executor is supplied by the native fixture in the tests. Real translated
    // execution enters through the generated code, not a guest-address cast.
    uint32_t initialize(Thread& thread, const std::function<bool(const InitCall&)>& executor);
    const std::vector<Module>& modules() const { return modules_; }
    const std::vector<HleSlot>& hle_slots() const { return hle_; }
    const std::vector<NativeModule>& native_modules() const { return native_; }
    const std::vector<TlsTemplate>& tls_templates() const { return tls_; }

private:
    Memory& memory_;
    std::vector<Module> modules_;
    std::vector<HleSlot> hle_;
    std::vector<NativeModule> native_;
    bool native_catalog_ = false;
    void build_native_catalog();
    std::vector<TlsTemplate> tls_;
    bool bound_ = false, init_started_ = false;
    Module* find(const std::string& name);
    const Module* find(const std::string& name) const;
    uint32_t resolve(const std::string& dll, const std::string& symbol, std::set<std::string>& visiting) const;
    std::string string(const Module& module, uint32_t rva) const;
    uint32_t word(const Module& module, uint32_t rva) const;
    void metadata(Module& module);
};
}
