#pragma once
/* Generated resolution table (tools/lift/resolution.py): guest writes for an opt-in screen size. */
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ResolutionWrite {
    uint32_t address;
    uint32_t size;   /* 1, 2, 4 or 8 bytes, little-endian */
    uint32_t always; /* 1: a code cell, written at every start; 0: a data site, written only with the mode on */
    uint32_t fps;    /* 1: the value depends on the frame rate limit (F) */
};
extern const char resolution_table_sha256[];
extern const unsigned resolution_write_count;
/* Guest word raised by hooks while a screen designed for 1024x768 draws (0: the table has none); see resolution.h. */
extern const uint32_t resolution_inset_address;
extern const struct ResolutionWrite resolution_writes[];
/* Settings.cfg switches (tools/lift/resolution.py options): the guest word the generated code tests, 1 = on. */
struct ResolutionOption {
    const char* setting;
    uint32_t address;
};
/* Bytes written into the cell area at every start (strings that option writes point at). */
struct ResolutionBlob {
    uint32_t address;
    uint32_t size;
    const char* bytes;
};
extern const unsigned resolution_blob_count;
extern const struct ResolutionBlob resolution_blobs[];
/* Image data replaced while a switch is on (a pointer table aimed at strings of a blob). */
struct ResolutionOptionWrite {
    const char* setting;
    uint32_t address;
    uint32_t size;
    const char* bytes;
};
extern const unsigned resolution_option_write_count;
extern const struct ResolutionOptionWrite resolution_option_writes[];
extern const unsigned resolution_option_count;
extern const struct ResolutionOption resolution_options[];
/* Value of entry `index` for a screen of w x h, hd = 1 with the mode on, and a frame rate limit f (the low `size`
   bytes are stored). */
uint64_t resolution_value(unsigned index, int32_t w, int32_t h, int32_t hd, int32_t f);
/* All of the above in one place: what the runtime reads (resolution.cpp, use_resolution_table). */
struct ResolutionTable {
    const char* sha256;
    unsigned write_count;
    uint32_t inset_address;
    const struct ResolutionWrite* writes;
    unsigned blob_count;
    const struct ResolutionBlob* blobs;
    unsigned option_write_count;
    const struct ResolutionOptionWrite* option_writes;
    unsigned option_count;
    const struct ResolutionOption* options;
    uint64_t (*value)(unsigned index, int32_t w, int32_t h, int32_t hd, int32_t f);
};
extern const struct ResolutionTable resolution_table;
#ifdef __cplusplus
}
#endif
