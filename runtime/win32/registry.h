#pragma once
#include "../process/control.h"

namespace sacred {
class Registry {
public:
    Registry(Control& control, const Loader& loader) : control_(control), loader_(loader) {}
    void bind();

private:
    struct Value {
        uint32_t type;
        std::vector<uint8_t> data;
    };
    using Values = std::map<std::string, Value>;
    Control& control_;
    const Loader& loader_;
    std::map<std::string, Values> keys_;
    std::map<uint32_t, std::string> handles_;
    uint32_t next_handle_ = 0xe8000001;
    std::string path(uint32_t parent, const std::string& subkey) const;
    std::string text(uint32_t pointer) const;
    uint32_t open(uint32_t parent, const std::string& subkey, uint32_t result, bool create, uint32_t* disposition);
};
}
