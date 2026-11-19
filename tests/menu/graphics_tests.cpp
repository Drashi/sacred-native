#include "runtime/ddraw/ddraw.h"
#include "runtime/win32/menu.h"
#include "runtime/win32/startup.h"
#include "runtime/miles/miles.h"
#include "platform/window_host.h"
#include <SDL.h>
#include <cstdio>
#include <exception>
#include <thread>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace sacred;
static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        ++checks;                                                                                                      \
        if (!(x))                                                                                                      \
            throw GuestFault(#x);                                                                                      \
    } while (0)
static void procedure(X86* cpu) {
    auto& c = active_control();
    cpu->r[R_EAX] = c.arg(1) == 0x81 ? 1 : 0;
    cpu->eip = c.memory.load32(cpu->r[R_ESP]);
    cpu->r[R_ESP] += 20;
}

// A 4x4 X8R8G8B8 guest image of one color (the texture contents the pack test locks in).
static PixelImage solid(uint32_t xrgb) {
    PixelImage image{4, 4, 16, PixelFormat::Xrgb8888, std::vector<uint8_t>(64)};
    for (unsigned i = 0; i < 16; ++i) {
        for (unsigned b = 0; b < 4; ++b) {
            image.pixels[i * 4 + b] = uint8_t(xrgb >> (b * 8));
        }
    }

    return image;
}

// A 256x256 A8R8G8B8 guest image with every byte `fill`: a background tile of the menu backdrop fixture.
static PixelImage tile_image(uint8_t fill) {
    return PixelImage{256, 256, 1024, PixelFormat::Argb8888, std::vector<uint8_t>(256 * 256 * 4, fill)};
}

// Menu backdrop fixture (menu_backdrops.h): a texture.pak whose UW_OPTION_1_01.TGA (the underworld options screen's
// top-left tile) holds tile_image(0x42), and that screen's wide picture.
static std::shared_ptr<MenuBackdrops> backdrop_fixture() {
    const std::string directory = "build/tests/menu/graphics-backdrops";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory + "/images");
    std::vector<uint8_t> data(0x100 + 12, 0);
    data[0] = 'T';
    data[1] = 'E';
    data[2] = 'X';
    data[3] = 3;
    data[4] = 1;
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            data[at + i] = uint8_t(v >> (8 * i));
        }
    };
    put32(0x100 + 4, uint32_t(data.size()));
    put32(0x100 + 8, 256 * 256 * 4);
    std::vector<uint8_t> header(0x50, 0);
    const std::string name = "UW_OPTION_1_01.TGA";
    std::copy(name.begin(), name.end(), header.begin());
    header[33] = 1;
    header[35] = 1;
    header[36] = 6; // 256 x 256, A8R8G8B8
    data.insert(data.end(), header.begin(), header.end());
    const auto tile = tile_image(0x42);
    data.insert(data.end(), tile.pixels.begin(), tile.pixels.end());
    std::ofstream(directory + "/texture.pak", std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    if (!texture_png_write(directory + "/images/options-underworld.png", 1536, 768,
                           std::vector<uint8_t>(1536 * 768 * 4, 255))) {
        throw GuestFault("cannot write the menu backdrop fixture");
    }

    return std::make_shared<MenuBackdrops>(directory + "/images",
                                           std::vector<std::filesystem::path>{directory + "/texture.pak"});
}

// Texture pack fixture: 8x8 (k = 2) for 0x00112233, 6x6 (not a multiple) for 0x00778899, 512x512 for the menu
// backdrop tile.
static std::string pack_fixture() {
    const std::string directory = "build/tests/menu/graphics-pack";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory + "/ground");
    std::vector<uint8_t> blue(8 * 8 * 4), wrong(6 * 6 * 4);
    for (size_t i = 0; i < blue.size(); i += 4) {
        blue[i] = 10;
        blue[i + 1] = 20;
        blue[i + 2] = 30;
        blue[i + 3] = 0;
    }

    // The dump's own name in a subdirectory, and a bare "<hash16>.png" (the 13-character "4x4_X8R8G8B8_" cut).
    const auto fits = solid(0x00112233), odd = solid(0x00778899);
    if (!texture_png_write(directory + "/ground/" + texture_file_name(fits, texture_hash(fits)), 8, 8, blue) ||
        !texture_png_write(directory + "/" + texture_file_name(odd, texture_hash(odd)).substr(13), 6, 6, wrong) ||
        !texture_png_write(directory + "/" + texture_file_name(tile_image(0x42), texture_hash(tile_image(0x42))), 512,
                           512, std::vector<uint8_t>(512 * 512 * 4, 200))) {
        throw GuestFault("cannot write the texture pack fixture");
    }

    return directory;
}

static void qualify(const std::string& assets, WindowHost& host) {
    Memory memory;
    Loader loader(memory);
    const char* names[] = {"Sacred.exe", "granny.dll", "tincat2.dll"};
    for (size_t i = 0; i < sacred_modules.size(); ++i) {
        loader.map_file(assets + "/" + names[i], sacred_modules[i]);
    }

    loader.enable_native_catalog();
    loader.bind();
    Thread thread(memory, 0, {});
    Control control(memory, thread);
    StartupEnvironment environment;
    environment.assets = assets;
    environment.profile = "build/tests/menu/graphics-profile";
    Startup files(control, loader, environment);
    files.bind();
    Menu menu(control, loader, {}, &host, &files);
    menu.bind();
    DirectDraw draw(control, loader, &menu, &host);
    draw.bind();
    auto pack = std::make_shared<TexturePack>(pack_fixture(), false); // synchronous: replaced at the unlock
    CHECK(pack->size() == 3);
    draw.texture_pack(pack);
    std::filesystem::remove_all("build/tests/menu/graphics-dump");
    draw.texture_dump("build/tests/menu/graphics-dump");
    control.entry(0x812bf0, procedure, CallEntry, "graphics fixture window ABI");
    uint32_t scratch = control.heap.allocate(4096);
    const char name[] = "Sacred graphics qualification";
    memory.write(scratch + 64, name, sizeof name);
    const uint32_t window_class[] = {48, 0, 0x812bf0, 0, 0, layout::exe, 0, 0, 0, 0, scratch + 64, 0};
    memory.write(scratch, window_class, sizeof window_class);
    auto call = [&](const char* dll, const char* symbol, std::vector<uint64_t> args) {
        return control
            .invoke(loader.resolve(dll, symbol),
                    {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(args.size()), ""}, args)
            .integer;
    };
    const char bitmap_path[] = "pak\\LOADING0.BMP";
    memory.write(scratch + 800, bitmap_path, sizeof bitmap_path);
    CHECK(call("user32.dll", "LoadImageA", {0, scratch + 800, 0, 0, 0, 0x2010}) == 0xe8000001);
    const char missing_bitmap[] = "pak\\missing-bitmap.bmp";
    memory.write(scratch + 800, missing_bitmap, sizeof missing_bitmap);
    CHECK(call("user32.dll", "LoadImageA", {0, scratch + 800, 0, 0, 0, 0x2010}) == 0);
    CHECK(call("kernel32.dll", "GetLastError", {}) == 2);
    CHECK(call("user32.dll", "LoadImageA", {layout::exe, 113, 0, 0, 0, 0x2000}) == 0xe8000002);
    const uint8_t logical_font[60]{};
    memory.write(scratch + 1024, logical_font, sizeof logical_font);
    memory.store32(scratch + 1024, 20);
    memory.store32(scratch + 1040, 400);
    const char family[] = "Times New Roman";
    memory.write(scratch + 1052, family, sizeof family);
    CHECK(call("gdi32.dll", "CreateFontIndirectA", {scratch + 1024}) == 0xe9000001);
    const char antiqua_path[] = "FONT\\ANTQS__.TTF", carolingia_path[] = "font\\CAROLING.TTF";
    memory.write(scratch + 800, antiqua_path, sizeof antiqua_path);
    CHECK(call("gdi32.dll", "AddFontResourceA", {scratch + 800}) == 1);
    CHECK(call("gdi32.dll", "AddFontResourceA", {scratch + 800}) == 1);
    memory.write(scratch + 800, carolingia_path, sizeof carolingia_path);
    CHECK(call("gdi32.dll", "AddFontResourceA", {scratch + 800}) == 1);
    const char antiqua_family[] = "AntiquaSSK";
    memory.write(scratch + 1052, antiqua_family, sizeof antiqua_family);
    CHECK(call("gdi32.dll", "CreateFontIndirectA", {scratch + 1024}) == 0xe9000002);
    {
        Miles miles(control, loader, &host, {}, [](const std::string&) {}, "");
        miles.bind();
        CHECK(call("mss32.dll", "_AIL_startup@0", {}) == 1);
        CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0);
        CHECK(call("mss32.dll", "_AIL_set_preference@8", {15, 0}) == 0);
        std::string audio_error;
        uint32_t native_device = host.open_audio(44100, audio_error);
        CHECK(native_device != 0 && audio_error.empty());
        CHECK(SDL_GetAudioDeviceStatus(native_device) == SDL_AUDIO_PLAYING);
        host.close_audio(native_device);
        uint32_t driver = call("mss32.dll", "_AIL_open_digital_driver@16", {44100, 16, 2, 1});
        CHECK(driver == 0xe6000001);
        const uint32_t sample_a = call("mss32.dll", "_AIL_allocate_sample_handle@4", {driver});
        const uint32_t sample_b = call("mss32.dll", "_AIL_allocate_sample_handle@4", {driver});
        CHECK(sample_a != 0 && sample_b != 0 && sample_a != sample_b);
        CHECK(call("mss32.dll", "_AIL_sample_status@4", {sample_a}) == 2);
        auto faults = [&](const char* symbol, std::vector<uint64_t> args) {
            try {
                call("mss32.dll", symbol, args);
            } catch (const GuestFault&) {
                return true;
            }

            return false;
        };
        // NULL handles follow the original mss32 (docs/internals/miles-null-handles.md): early return, no trap.
        CHECK(call("mss32.dll", "_AIL_allocate_sample_handle@4", {0}) == 0);
        CHECK(faults("_AIL_allocate_sample_handle@4", {0xdead0001}));
        {
            const uint32_t sentinel = 0x12345678;
            memory.store32(scratch + 3072, sentinel);
            memory.store32(scratch + 3076, sentinel);
            call("mss32.dll", "_AIL_stream_ms_position@12", {0, scratch + 3072, scratch + 3076});
            CHECK(memory.load32(scratch + 3072) == sentinel &&
                  memory.load32(scratch + 3076) == sentinel); // outputs untouched
            CHECK(call("mss32.dll", "_AIL_stream_status@4", {0}) == 0xffffffffu);
            call("mss32.dll", "_AIL_stream_volume_pan@12", {0, scratch + 3072, 0});
            CHECK(memory.load32(scratch + 3072) == 0xbf800000u &&
                  memory.load32(scratch + 3076) == sentinel); // -1.0f, NULL out skipped
            memory.store32(scratch + 3072, sentinel);
            call("mss32.dll", "_AIL_sample_volume_pan@12", {0, scratch + 3072, scratch + 3076});
            CHECK(memory.load32(scratch + 3072) == sentinel && memory.load32(scratch + 3076) == sentinel);
            CHECK(call("mss32.dll", "_AIL_sample_status@4", {0}) == 0);
            CHECK(call("mss32.dll", "_AIL_sample_playback_rate@4", {0}) == 0);
            CHECK(call("mss32.dll", "_AIL_register_EOS_callback@8", {0, 0x1000}) == 0);
            CHECK(call("mss32.dll", "_AIL_open_stream@12", {0, scratch + 800, 0}) == 0);
            CHECK(call("mss32.dll", "_AIL_open_stream@12", {driver, 0, 0}) == 0);
            CHECK(call("mss32.dll", "_AIL_set_sample_file@12", {0, scratch + 800, 0}) == 0);
            CHECK(call("mss32.dll", "_AIL_set_sample_file@12", {sample_a, 0, 0}) == 0);
            const char plain_name[] = "x.wav", mp3_name[] = "x.MP3";
            memory.write(scratch + 3200, plain_name, sizeof plain_name);
            memory.write(scratch + 3232, mp3_name, sizeof mp3_name);
            memory.store32(scratch + 3264, 0);
            CHECK(call("mss32.dll", "_AIL_set_named_sample_file@20", {0, scratch + 3200, scratch + 3264, 4, 0}) == 0);
            CHECK(faults("_AIL_set_named_sample_file@20",
                         {0, scratch + 3232, scratch + 3264, 4, 0})); // ASI path writes through NULL
            CHECK(call("mss32.dll", "_AIL_WAV_info@8", {0, scratch + 3072}) == 0);
            CHECK(memory.load32(scratch + 3072) == sentinel);
            CHECK(call("mss32.dll", "_AIL_file_type@8", {0, 16}) == 0);
            CHECK(call("mss32.dll", "_AIL_decompress_ADPCM@12", {0, scratch + 3072, scratch + 3076}) == 0);
            CHECK(memory.load32(scratch + 3072) == sentinel && memory.load32(scratch + 3076) == sentinel);
            for (const char* symbol : {"_AIL_close_stream@4", "_AIL_start_stream@4", "_AIL_init_sample@4",
                                       "_AIL_start_sample@4", "_AIL_stop_sample@4", "_AIL_end_sample@4",
                                       "_AIL_release_sample_handle@4", "_AIL_mem_free_lock@4", "_AIL_waveOutClose@4"}) {
                call("mss32.dll", symbol, {0});
            }

            for (const char* symbol :
                 {"_AIL_pause_stream@8", "_AIL_set_stream_loop_count@8", "_AIL_set_stream_ms_position@8",
                  "_AIL_set_sample_loop_count@8", "_AIL_set_sample_playback_rate@8"}) {
                call("mss32.dll", symbol, {0, 1});
            }

            for (const char* symbol : {"_AIL_set_stream_volume_pan@12", "_AIL_set_sample_volume_pan@12"}) {
                call("mss32.dll", symbol, {0, 0x3f800000, 0x3f000000});
            }

            // A non-NULL handle that is not live is garbage to the original: strict trap.
            for (const char* symbol : {"_AIL_stream_status@4", "_AIL_close_stream@4", "_AIL_sample_status@4",
                                       "_AIL_release_sample_handle@4"}) {
                CHECK(faults(symbol, {0xdead0001}));
            }

            CHECK(faults("_AIL_stream_ms_position@12", {0xdead0001, scratch + 3072, scratch + 3076}));
            CHECK(faults("_AIL_waveOutClose@4", {0xdead0001}));
            CHECK(call("mss32.dll", "_AIL_sample_status@4", {sample_a}) == 2); // NULL calls left the live samples alone
        }

        memory.store32(scratch + 768, 0);
        memory.store32(scratch + 772, 0x12345678);
        memory.store32(scratch + 776, 0xabcdef01);
        CHECK(call("mss32.dll", "_AIL_enumerate_3D_providers@12", {scratch + 768, scratch + 772, scratch + 776}) == 0);
        CHECK(memory.load32(scratch + 772) == 0x12345678 && memory.load32(scratch + 776) == 0xabcdef01);
        CHECK(call("mss32.dll", "_AIL_enumerate_3D_providers@12", {0, scratch + 772, scratch + 776}) ==
              0); // NULL cursor starts from the beginning
    }

    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    uint32_t atom = call("user32.dll", "RegisterClassExA", {scratch});
    CHECK(atom != 0);
    uint32_t window =
        call("user32.dll", "CreateWindowExA", {0, atom, scratch + 64, 0x90000000, 0, 0, 64, 64, 0, 0, layout::exe, 0});
    CHECK(window != 0);
    CHECK(call("ddraw.dll", "DirectDrawCreateEx", {0, scratch + 256, 0x8994d8, 0}) == 0);
    uint32_t dd = memory.load32(scratch + 256);
    auto method = [&](uint32_t object, unsigned slot, std::vector<uint64_t> args) {
        args.insert(args.begin(), object);
        return control
            .invoke(memory.load32(memory.load32(object) + slot * 4),
                    {Convention::Stdcall, ReturnChannel::Eax, std::vector<Argument>(args.size()), ""}, args)
            .integer;
    };
    CHECK(method(dd, 20, {window, 0xc08}) == 0);
    CHECK(method(dd, 22, {1, 0}) == 0);
    CHECK(method(dd, 0, {0x8994e8, scratch + 640}) == 0);
    uint32_t dd1 = memory.load32(scratch + 640);
    CHECK(dd1 != dd && dd1 != 0 && method(dd1, 2, {}) == 1);
    auto create = [&] {
        for (unsigned i = 0; i < 31; ++i) {
            memory.store32(scratch + 512 + i * 4, 0);
        }

        memory.store32(scratch + 512, 124);
        memory.store32(scratch + 516, 7);
        memory.store32(scratch + 520, 4);
        memory.store32(scratch + 524, 4);
        memory.store32(scratch + 616, 0x6000);
        CHECK(method(dd, 6, {scratch + 512, scratch + 640, 0}) == 0);
        return memory.load32(scratch + 640);
    };
    uint32_t source = create(), copy = create();
    CHECK(method(copy, 0, {0x8994c8, scratch + 640}) == 0);
    uint32_t copy1 = memory.load32(scratch + 640);
    CHECK(copy1 != copy && copy1 != 0 && method(copy1, 2, {}) == 1);
    memory.store32(scratch + 1024, 380);
    CHECK(method(dd, 11, {scratch + 1024, 0}) == 0);
    CHECK(memory.load32(scratch + 1088) == 256u * 1024 * 1024 - 128);
    memory.store32(scratch + 512, 124);
    CHECK(method(source, 22, {scratch + 512}) == 0);
    CHECK(memory.load32(scratch + 520) == 4 && memory.load32(scratch + 524) == 4);
    CHECK(memory.load32(scratch + 596) == 32 && memory.load32(scratch + 548) == 0);
    CHECK(method(source, 25, {0, scratch + 512, 1, 0}) == 0);
    uint32_t backing = memory.load32(scratch + 548);
    CHECK(backing != 0 && memory.load32(scratch + 528) == 16);
    for (unsigned i = 0; i < 16; ++i) {
        memory.store32(backing + i * 4, 0x80112233);
    }

    CHECK(method(source, 32, {0}) == 0);
    auto read = [&](uint32_t surface) {
        std::vector<uint8_t> pixels;
        host.render(menu.native_window(window), [&](Renderer& renderer) { pixels = renderer.read(surface); });
        return pixels;
    };
    auto pixels = read(source);
    CHECK(pixels.size() == 64 && pixels[0] == 0x11 && pixels[1] == 0x22 && pixels[2] == 0x33 && pixels[3] == 255);
    { // DDLOCK flags, rejections and partial rectangles on the 4x4 XRGB surface (pitch 16).
        auto lock = [&](uint32_t area, uint32_t flags, uint32_t event = 0) {
            memory.store32(scratch + 512, 124);
            return method(source, 25, {area, scratch + 512, flags, event});
        };
        auto faults = [&](uint32_t area, uint32_t flags, uint32_t event) {
            try {
                lock(area, flags, event);
            } catch (const GuestFault&) {
                return true;
            }

            return false;
        };
        const uint32_t partial[] = {1, 2, 3, 4}, outside[] = {0, 0, 5, 4}, empty[] = {2, 2, 2, 3};
        memory.write(scratch + 2560, partial, sizeof partial);
        memory.write(scratch + 2576, outside, sizeof outside);
        memory.write(scratch + 2592, empty, sizeof empty);
        CHECK(lock(scratch + 2560, 0x821) == 0); // WAIT|WRITEONLY|NOSYSLOCK, partial rect
        uint32_t at = memory.load32(scratch + 548);
        CHECK(at == backing + 2 * 16 + 1 * 4 && memory.load32(scratch + 528) == 16);
        memory.store32(at, 0x00aabbcc);
        memory.store32(at + 4, 0x00aabbcc);
        memory.store32(at + 16, 0x00aabbcc);
        memory.store32(at + 20, 0x00aabbcc);
        memory.store32(backing, 0x00010101); // outside the locked rectangle: not uploaded
        CHECK(method(source, 32, {0}) == 0);
        pixels = read(source);
        CHECK(pixels[(2 * 4 + 1) * 4] == 0xaa && pixels[(2 * 4 + 1) * 4 + 1] == 0xbb &&
              pixels[(3 * 4 + 2) * 4 + 2] == 0xcc);
        CHECK(pixels[0] == 0x11 && pixels[1] == 0x22 && pixels[(2 * 4) * 4] == 0x11);
        CHECK(lock(scratch + 2560, 0x11) == 0); // READONLY sees the new contents ...
        CHECK(memory.load32(memory.load32(scratch + 548)) == 0x00aabbcc);
        memory.store32(memory.load32(scratch + 548), 0); // ... and its writes are dropped
        CHECK(method(source, 32, {0}) == 0);
        CHECK(read(source) == pixels);
        CHECK(lock(0, 0x2001) == 0 && memory.load32(scratch + 548) == backing); // DISCARDCONTENTS
        CHECK(method(source, 32, {0}) == 0);
        for (uint32_t flags : {0u, 1u, 0x800u, 0x1000u, 0x4000u, 0x4801u, 0x3000u, 0x20u, 0x10u}) {
            CHECK(lock(0, flags) == 0);
            CHECK(lock(0, flags) == 0x887601ae); // second lock while locked
            CHECK(method(source, 32, {0}) == 0);
            CHECK(method(source, 32, {0}) == 0x88760248); // unlock when not locked
        }

        CHECK(lock(0, 0x30) == 0x80070057); // READONLY|WRITEONLY
        CHECK(lock(scratch + 2576, 1) == 0x88760096 && lock(scratch + 2592, 1) == 0x88760096);
        CHECK(faults(0, 0x2, 0) && faults(0, 1, 0x1234) && faults(0, 0x10000, 0) && faults(0, 0x8000, 0));
        CHECK(lock(0, 1) == 0); // the rejected calls left no lock behind
        for (unsigned i = 0; i < 16; ++i) {
            memory.store32(backing + i * 4, 0x80112233);
        }

        CHECK(method(source, 32, {0}) == 0);
        pixels = read(source);
        CHECK(pixels[0] == 0x11 && pixels[(2 * 4 + 1) * 4] == 0x11 && pixels[(3 * 4 + 2) * 4 + 2] == 0x33);
    }

    { // Texture pack: a full unlock whose content is in the pack shows the replacement (8x8 for the 4x4 guest
        // texture, opaque because X8R8G8B8 has no alpha); other content, a partial refill or a size that is not an
        // integer multiple shows the guest's own pixels. The guest never sees anything but its own 4x4 surface.
        for (unsigned i = 0; i < 31; ++i) {
            memory.store32(scratch + 512 + i * 4, 0);
        }

        memory.store32(scratch + 512, 124);
        memory.store32(scratch + 516, 7);
        memory.store32(scratch + 520, 4);
        memory.store32(scratch + 524, 4);
        memory.store32(scratch + 616, 0x1000);
        CHECK(method(dd, 6, {scratch + 512, scratch + 640, 0}) == 0);
        const uint32_t texture = memory.load32(scratch + 640);
        auto fill_texture = [&](uint32_t xrgb, uint32_t area) {
            memory.store32(scratch + 512, 124);
            CHECK(method(texture, 25, {area, scratch + 512, 1, 0}) == 0);
            CHECK(memory.load32(scratch + 520) == 4 && memory.load32(scratch + 524) == 4);
            const uint32_t at = memory.load32(scratch + 548);
            for (unsigned y = 0; y < 4; ++y) {
                for (unsigned x = 0; x < 4; ++x) {
                    memory.store32(at + y * 16 + x * 4, xrgb);
                }
            }

            CHECK(method(texture, 32, {0}) == 0);
        };
        auto color = [&](uint8_t r, uint8_t g, uint8_t b) {
            const auto shown = read(texture);
            return shown.size() == 64 && shown[0] == r && shown[1] == g && shown[2] == b && shown[3] == 255 &&
                   shown[60] == r && shown[63] == 255;
        };
        fill_texture(0x00112233, 0);
        CHECK(color(10, 20, 30) && draw.hd_stats().replaced == 1);
        fill_texture(0x00445566, 0); // refilled with other content: reverted
        CHECK(color(0x44, 0x55, 0x66) && draw.hd_stats().reverted == 1);
        fill_texture(0x00112233, 0);
        CHECK(color(10, 20, 30) && draw.hd_stats().replaced == 2);
        const uint32_t corner[] = {0, 0, 4, 4}; // a rectangle lock of the whole surface is full too
        memory.write(scratch + 2560, corner, sizeof corner);
        fill_texture(0x00112233, scratch + 2560);
        CHECK(color(10, 20, 30) && draw.hd_stats().replaced == 3);
        const uint32_t part[] = {0, 0, 4, 2}; // a partial refill: the guest's pixels again
        memory.write(scratch + 2560, part, sizeof part);
        fill_texture(0x00112233, scratch + 2560);
        CHECK(color(0x11, 0x22, 0x33) && draw.hd_stats().reverted == 2);
        fill_texture(0x00778899, 0); // 6x6 for 4x4: logged and ignored
        CHECK(color(0x77, 0x88, 0x99) && draw.hd_stats().replaced == 3);
        memory.store32(scratch + 512, 124); // a guest read sees its own pixels
        CHECK(method(texture, 25, {0, scratch + 512, 0x11, 0}) == 0);
        CHECK(memory.load32(memory.load32(scratch + 548)) == 0x00778899);
        CHECK(method(texture, 32, {0}) == 0);
        for (uint32_t xrgb : {0x00112233u, 0x00445566u, 0x00778899u}) {
            CHECK(std::filesystem::exists("build/tests/menu/graphics-dump/" +
                                          texture_file_name(solid(xrgb), texture_hash(solid(xrgb)))));
        }

        CHECK(method(texture, 2, {}) == 0);
    }

    { // A background tile of a wide-picture screen keeps its role when the pack replaces it at the unlock, as it
        // does once the replacement is decoded: every load after the first (the menu after a game, the game menu's
        // options after the main menu's). Other content drops the role; the tile's content brings it back.
        auto backdrops = backdrop_fixture();
        const int options = backdrops->find("options-underworld");
        CHECK(options >= 0 && backdrops->tile(texture_hash(tile_image(0x42))));
        draw.menu_backdrops(backdrops);
        for (unsigned i = 0; i < 31; ++i) {
            memory.store32(scratch + 512 + i * 4, 0);
        }

        memory.store32(scratch + 512, 124);
        memory.store32(scratch + 516, 0x1007);
        memory.store32(scratch + 520, 256);
        memory.store32(scratch + 524, 256);
        const auto argb = describe(PixelFormat::Argb8888);
        const uint32_t pixel_format[] = {32, 0x41, 0, argb.bits, argb.red, argb.green, argb.blue, argb.alpha};
        memory.write(scratch + 512 + 72, pixel_format, sizeof pixel_format);
        memory.store32(scratch + 616, 0x1000);
        CHECK(method(dd, 6, {scratch + 512, scratch + 640, 0}) == 0);
        const uint32_t tile = memory.load32(scratch + 640);
        auto fill_tile = [&](uint8_t fill) {
            memory.store32(scratch + 512, 124);
            CHECK(method(tile, 25, {0, scratch + 512, 1, 0}) == 0);
            const uint32_t at = memory.load32(scratch + 548), pitch = memory.load32(scratch + 528);
            const std::vector<uint8_t> row(1024, fill);
            for (uint32_t y = 0; y < 256; ++y) {
                memory.write(at + y * pitch, row.data(), row.size());
            }

            CHECK(method(tile, 32, {0}) == 0);
        };
        const uint64_t replaced = draw.hd_stats().replaced;
        fill_tile(0x42);
        const BackdropTile* role = draw.texture_backdrop(tile);
        CHECK(draw.hd_stats().replaced == replaced + 1);
        CHECK(role && role->role == BackdropTile::Tile && role->backdrop == options && role->column == 0 &&
              role->row == 0 && role->screen == "options-underworld");
        fill_tile(0x17);
        CHECK(!draw.texture_backdrop(tile));
        fill_tile(0x42);
        CHECK(draw.texture_backdrop(tile) == role && draw.hd_stats().replaced == replaced + 2);
        CHECK(method(tile, 2, {}) == 0);
        draw.menu_backdrops(nullptr);
    }

    const uint32_t rect[] = {1, 1, 3, 3};
    memory.write(scratch + 2048, rect, sizeof rect);
    memory.store32(scratch + 2304, 100);
    memory.store32(scratch + 2384, 0x00010203);
    CHECK(method(source, 5, {scratch + 2048, 0, 0, 0x400, scratch + 2304}) == 0);
    pixels = read(source);
    CHECK(pixels[0] == 0x11 && pixels[20] == 1 && pixels[21] == 2 && pixels[22] == 3);
    CHECK(method(copy, 5, {0, source, 0, 0x01000000, 0}) == 0);
    CHECK(read(copy) == pixels);
    CHECK(method(copy, 25, {0, scratch + 512, 1, 0}) == 0);
    uint32_t copied = memory.load32(scratch + 548);
    CHECK(memory.load32(copied + 20) == 0x00010203);
    CHECK(method(copy, 32, {0}) == 0);
    CHECK(method(copy, 17, {scratch + 640}) == 0);
    uint32_t dc = memory.load32(scratch + 640);
    CHECK(dc != 0);
    uint32_t memory_dc = call("gdi32.dll", "CreateCompatibleDC", {dc});
    CHECK(memory_dc != 0 && memory_dc != dc);
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0xe8000001}) == 0xe8000000);
    uint32_t second_dc = call("gdi32.dll", "CreateCompatibleDC", {dc});
    CHECK(call("gdi32.dll", "SelectObject", {second_dc, 0xe8000001}) == 0);
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0xe8000000}) == 0xe8000001);
    CHECK(call("gdi32.dll", "SelectObject", {second_dc, 0xe8000001}) == 0xe8000000);
    CHECK(call("gdi32.dll", "SelectObject", {second_dc, 0xe8000000}) == 0xe8000001);
    CHECK(call("gdi32.dll", "SelectObject", {dc, 0xe9000001}) == 0xe9000000);
    CHECK(call("gdi32.dll", "SelectObject", {dc, 0xe9000000}) == 0xe9000001);
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0xe8000001}) == 0xe8000000);
    CHECK(call("gdi32.dll", "BitBlt", {dc, 0, 0, 4, 4, memory_dc, 200, 200, 0xcc0020}) == 1);
    call("kernel32.dll", "SetLastError", {0x1234});
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0}) == 0);
    CHECK(call("kernel32.dll", "GetLastError", {}) == 0x1234);
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0xe8000000}) == 0xe8000001);
    CHECK(call("gdi32.dll", "SelectObject", {memory_dc, 0xe8000001}) == 0xe8000000);
    CHECK(call("gdi32.dll", "DeleteDC", {memory_dc}) == 1);
    CHECK(call("gdi32.dll", "SelectObject", {second_dc, 0xe8000001}) == 0xe8000000);
    CHECK(call("gdi32.dll", "DeleteDC", {second_dc}) == 1);
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> original(
        SDL_LoadBMP((assets + "/PAK/loading0.bmp").c_str()), SDL_FreeSurface);
    CHECK(original != nullptr);
    std::unique_ptr<SDL_Surface, decltype(&SDL_FreeSurface)> rgba(
        SDL_ConvertSurfaceFormat(original.get(), SDL_PIXELFORMAT_RGBA32, 0), SDL_FreeSurface);
    CHECK(rgba != nullptr);
    std::vector<uint8_t> bitmap_expected(64);
    for (unsigned row = 0; row < 4; ++row) {
        std::memcpy(bitmap_expected.data() + row * 16,
                    static_cast<uint8_t*>(rgba->pixels) + (200 + row) * rgba->pitch + 200 * 4, 16);
    }

    CHECK(method(copy, 25, {0, scratch + 512, 1, 0}) == 0x887601ae);
    CHECK(method(copy, 26, {dc}) == 0);
    CHECK(read(copy) == bitmap_expected);
    for (unsigned i = 0; i < 31; ++i) {
        memory.store32(scratch + 512 + i * 4, 0);
    }

    memory.store32(scratch + 512, 124);
    memory.store32(scratch + 516, 0x1007);
    memory.store32(scratch + 520, 4);
    memory.store32(scratch + 524, 4);
    memory.store32(scratch + 616, 0x20800);
    const uint32_t depth_format[] = {32, 0x4400, 0, 32, 8, 0xffffff, 0xff000000, 0};
    memory.write(scratch + 584, depth_format, sizeof depth_format);
    CHECK(method(dd, 6, {scratch + 512, scratch + 640, 0}) == 0);
    uint32_t depth = memory.load32(scratch + 640);
    CHECK(method(copy, 3, {depth}) == 0);
    const uint32_t requested[] = {0x20000, 0, 0, 0};
    memory.write(scratch + 256, requested, sizeof requested);
    CHECK(method(copy, 12, {scratch + 256, scratch + 640}) == 0);
    CHECK(memory.load32(scratch + 640) == depth && method(depth, 2, {}) == 2);
    CHECK(method(depth, 2, {}) == 1);
    const uint8_t d3d_iid[] = {0x77, 0x9e, 4, 0xf5, 0x61, 0x48, 0xd2, 0x11, 0xa4, 7, 0, 0xa0, 0xc9, 6, 0x29, 0xa8};
    memory.write(scratch + 256, d3d_iid, sizeof d3d_iid);
    CHECK(method(dd, 0, {scratch + 256, scratch + 640}) == 0);
    uint32_t d3d = memory.load32(scratch + 640);
    const uint8_t rgb_guid[] = {0x60, 0x5c, 0x66, 0xa4, 0x73, 0x26, 0xcf, 0x11,
                                0xa3, 0x1a, 0,    0xaa, 0,    0xb9, 0x33, 0x56};
    memory.write(scratch + 256, rgb_guid, sizeof rgb_guid);
    CHECK(method(d3d, 4, {scratch + 256, copy, scratch + 640}) == 0);
    uint32_t device = memory.load32(scratch + 640);
    CHECK(method(device, 20, {137, 0}) == 0);
    CHECK(method(device, 20, {22, 1}) == 0);
    CHECK(method(device, 20, {7, 0}) == 0);
    const uint32_t vertices[] = {0xbf000000, 0xbf000000, 0x3f000000, 0x3f800000, 0xffff0000, 0, 0, 0,
                                 0x40600000, 0xbf000000, 0x3f000000, 0x3f800000, 0xffff0000, 0, 0, 0,
                                 0xbf000000, 0x40600000, 0x3f000000, 0x3f800000, 0xffff0000, 0, 0, 0};
    memory.write(scratch + 3072, vertices, sizeof vertices);
    CHECK(method(device, 10, {0, 0, 3, 0, 0x3f800000, 0}) == 0);
    CHECK(method(device, 5, {}) == 0);
    CHECK(method(device, 25, {4, 0x1c4, scratch + 3072, 3, 1}) == 0); // D3DDP_WAIT, as the ALT item-name overlay draws
    { // a flag that changes what is drawn (D3DDP_DONOTLIGHT) stays rejected
        bool rejected = false;
        try {
            method(device, 25, {4, 0x1c4, scratch + 3072, 3, 0x800});
        } catch (const GuestFault&) {
            rejected = true;
        }

        CHECK(rejected);
    }

    CHECK(method(device, 6, {}) == 0);
    pixels = read(copy);
    CHECK(pixels[0] == 255 && pixels[1] == 0 && pixels[2] == 0 && pixels[3] == 255);
    memory.store32(scratch + 512, 124);
    CHECK(method(copy, 25, {0, scratch + 512, 1, 0}) == 0);
    CHECK(memory.load32(copied) == 0x00ff0000);
    CHECK(method(copy, 32, {0}) == 0);
    CHECK(method(device, 2, {}) == 0);
    CHECK(method(d3d, 2, {}) != 0);
    CHECK(method(dd, 4, {0, scratch + 640, 0}) == 0);
    uint32_t clipper = memory.load32(scratch + 640);
    CHECK(method(clipper, 8, {0, window}) == 0);
    CHECK(method(clipper, 4, {scratch + 640}) == 0 && memory.load32(scratch + 640) == window);
    CHECK(method(copy, 28, {clipper}) == 0);
    CHECK(method(clipper, 2, {}) == 1);
    host.position_window(menu.native_window(window), 1, 1, 64, 64);
    const auto before = read(copy);
    CHECK(method(copy, 5, {0, source, 0, 0x01000000, 0}) == 0);
    pixels = read(copy);
    const auto placed = host.window_state(menu.native_window(window));
    if (placed.x == 1 && placed.y == 1) {
        CHECK(pixels[0] == 255 && pixels[1] == 0 && pixels[20] == 1 && pixels[21] == 2 && pixels[22] == 3);
    } else {
        // The window manager moved the window (macOS keeps windows below the menu bar): the clipper rectangle
        // follows the real window position, which lies outside the 4x4 primary, so the blit is clipped away.
        std::fprintf(stderr, "GRAPHICS_NOTE window placed at %d,%d instead of 1,1\n", placed.x, placed.y);
        CHECK(placed.x >= 4 || placed.y >= 4 ? pixels == before : true);
    }

    CHECK(method(source, 2, {}) == 0 && memory.permissions(backing) == 0);
    host.destroy_window(menu.native_window(window));
    CHECK(method(copy, 2, {}) == 0 && memory.permissions(copied) == 0 && memory.permissions(depth) == 0 &&
          memory.permissions(clipper) == 0 && memory.permissions(copy1) == 0);
    CHECK(method(dd, 2, {}) == 0 && memory.permissions(dd1) == 0);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }

    WindowHost host;
    std::exception_ptr failure;
    std::thread worker([&] {
        try {
            qualify(argv[1], host);
        } catch (...) {
            failure = std::current_exception();
        }

        host.finish();
    });
    host.loop();
    worker.join();
    if (failure) {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "GRAPHICS_FAILURE %s\n", error.what());
        }

        return 1;
    }

    std::printf("GRAPHICS_PASS checks=%u guest_lock=1 GPU_roundtrip=1 dirty_copy=1 post_window_release=1\n", checks);
    return 0;
}
