#pragma once
#include <cstddef>
#include <cstdint>

namespace sacred {
// Host pages are backing storage only. Guest permissions live in Memory.
class GuestArena {
public:
    static constexpr uint64_t size = uint64_t{1} << 32;
    // host_page == 0 queries the host; tests inject another power-of-two size.
    explicit GuestArena(size_t host_page = 0);
    ~GuestArena();
    GuestArena(const GuestArena&) = delete;
    GuestArena& operator=(const GuestArena&) = delete;
    void commit(uint32_t address, uint64_t length);
    uint8_t* base() const { return base_; }
    size_t host_page() const { return host_page_; }
    uint64_t last_commit_begin() const { return last_begin_; }
    uint64_t last_commit_end() const { return last_end_; }

private:
    uint8_t* reservation_;
    uint8_t* base_;
    size_t host_page_;
    uint64_t last_begin_ = 0, last_end_ = 0;
};
}
