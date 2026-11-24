#include "renderer.h"
#include "shaders.h"
#include "letterbox.h"
#include "../runtime/process/memory.h"
#include <SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL_opengl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <cstddef>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#ifdef __linux__
#include <unistd.h>
#endif

namespace sacred {
void configure_renderer_window() {
    // macOS exposes 3.2 and 4.1 profiles, not a 3.3 profile. Both hosts execute
    // the same GL 3.3 core subset; no optional extensions are used.
#ifdef __APPLE__
    constexpr int major = 4, minor = 1;
#else
    constexpr int major = 3, minor = 3;
#endif
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1)) {
        throw GuestFault(std::string("cannot configure core GL window: ") + SDL_GetError());
    }
}

namespace {
#ifdef __linux__
// Last resort when no hardware core context exists: restart this process once with Mesa software
// rendering (llvmpipe). Returns only when re-exec is not possible; never returns on success.
const char* software_reexec(const std::string& reason) {
    if (std::getenv("SACRED_GL_REEXEC") || std::getenv("LIBGL_ALWAYS_SOFTWARE")) {
        return nullptr;
    }

    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    std::string blob((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    std::vector<std::string> args;
    for (size_t start = 0; start < blob.size();) {
        size_t end = blob.find('\0', start);
        if (end == std::string::npos) {
            end = blob.size();
        }

        args.push_back(blob.substr(start, end - start));
        start = end + 1;
    }

    if (args.empty()) {
        return nullptr;
    }

    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }

    argv.push_back(nullptr);
    setenv("SACRED_GL_REEXEC", "1", 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    setenv("GALLIUM_DRIVER", "llvmpipe", 1);
    std::fprintf(stderr, "SACRED_GL no hardware core 3.3 context (%s); restarting with Mesa software rendering\n",
                 reason.c_str());
    std::fflush(nullptr);
    execv("/proc/self/exe", argv.data());
    std::fprintf(stderr, "SACRED_GL software re-exec failed: %s\n", std::strerror(errno));
    return nullptr;
}
#else
const char* software_reexec(const std::string&) {
    return nullptr;
}
#endif
class GlRenderer final : public Renderer {
    struct Surface {
        uint32_t width, height; // guest size
        GLuint texture = 0, framebuffer = 0, depth = 0;
        unsigned stencil_bits = 0;
        uint32_t scale = 1;  // GPU storage is scale x the guest size (render scale or a replaced texture)
        uint32_t levels = 0; // mipmap levels below level 0 (replaced textures only)
        // Sampler parameters last set on the texture object (D3D filter, filter, address U, address V, border).
        std::array<uint32_t, 5> sampler{};
        bool sampler_set = false;
    };
    // What draw() last left in the GL context. Consecutive draw calls mostly differ in their texture only, so
    // every piece of state is sent again only when it changes. Every other entry point of the renderer that
    // touches GL state calls forget() (or forget_textures() when it only rebinds a texture).
    struct Applied {
        bool valid = false, textures_valid = false;
        GLuint framebuffer = 0;
        unsigned stencil_bits = 0;
        RenderState state;
        bool transformed = false, diffuse = false, specular = false;
        std::array<GLuint, 2> texture{};
        std::array<GLint, 2> bound{};
        GLenum unit = 0;
        float bias = 0;
    } applied_;
    void forget() { applied_.valid = false; }
    void forget_textures() { applied_.textures_valid = false; }
    void unit(GLenum value) {
        if (applied_.unit != value) {
            glActiveTexture(value);
            applied_.unit = value;
        }
    }

    std::map<uint32_t, Surface> surfaces_;
    GLuint program_ = 0, vao_ = 0, vertices_ = 0, indices_ = 0;
    // The vertices of every DrawItem::mesh share one buffer (the arena) and one vertex array; a mesh is a range of
    // it, drawn with its offset as base vertex. One index buffer of quads serves them all. Ranges are taken from the
    // top; when the top reaches the end, the live ranges are packed into a new buffer twice their size.
    // Each mesh keeps its vertices here too: Apple's GL does not implement glCopyBufferSubData ("gldCopyBufferSubData:
    // NEEDS IMPLEMENTATION", the copy is dropped), so growing a range or packing the arena uploads them again.
    struct Mesh {
        uint32_t offset = 0, capacity = 0, used = 0;
        std::vector<RenderVertex> vertices;
    };
    std::unordered_map<uint32_t, Mesh> meshes_;
    GLuint arena_ = 0, arena_vao_ = 0, skin_vao_ = 0; // skin_vao_: the arena's vertices with the batch's indices
    uint32_t arena_capacity_ = 0, arena_top_ = 0;
    std::vector<GLsizei> multi_counts_;
    std::vector<const void*> multi_offsets_;
    std::vector<GLint> multi_bases_;
    uint32_t arena_take(uint32_t count) {
        if (arena_top_ + count > arena_capacity_) {
            uint64_t live = 0;
            for (const auto& mesh : meshes_) {
                live += mesh.second.capacity;
            }

            const uint64_t wanted = std::max<uint64_t>((live + count) * 2, 1u << 16);
            if (wanted > 0xffffffffull / sizeof(RenderVertex)) {
                throw GuestFault("ground mesh arena too large");
            }

            const uint32_t capacity = uint32_t(wanted);
            GLuint buffer = 0;
            glGenBuffers(1, &buffer);
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            glBufferData(GL_ARRAY_BUFFER, size_t(capacity) * sizeof(RenderVertex), nullptr, GL_STATIC_DRAW);
            uint32_t top = 0;
            for (auto& mesh : meshes_) {
                Mesh& m = mesh.second;
                if (!m.vertices.empty()) {
                    glBufferSubData(GL_ARRAY_BUFFER, size_t(top) * sizeof(RenderVertex),
                                    m.vertices.size() * sizeof(RenderVertex), m.vertices.data());
                }

                m.offset = top;
                top += m.capacity;
            }

            if (arena_) {
                glDeleteBuffers(1, &arena_);
            }

            arena_ = buffer;
            arena_capacity_ = capacity;
            arena_top_ = top;
            if (!arena_vao_) {
                glGenVertexArrays(1, &arena_vao_);
            }

            glBindVertexArray(arena_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, arena_);
            vertex_layout();
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_indices_);
            if (!skin_vao_) {
                glGenVertexArrays(1, &skin_vao_);
            }

            glBindVertexArray(skin_vao_);
            glBindBuffer(GL_ARRAY_BUFFER, arena_);
            vertex_layout();
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
            glBindVertexArray(vao_);
        }

        const uint32_t at = arena_top_;
        arena_top_ += count;
        return at;
    }

    GLuint quad_indices_ = 0;
    uint32_t quad_capacity_ = 0;
    bool mesh_applied_ = false, skin_applied_ = false;
    // DrawBatch::ground_light as a float texture on unit 2, which nothing else uses: row 0 the colour ramp, the four
    // rows below the lights (x, y, radius), 256 a row.
    GLuint ground_light_ = 0;
    bool ground_lit_applied_ = false;
    std::vector<float> ground_pixels_;
    void ground_light_upload(const GroundLight& light) {
        if (light.ramp.size() != 256 * 3 || light.lights.size() % 3 || light.lights.size() > 1024 * 3) {
            throw GuestFault("invalid ground light");
        }

        const size_t count = light.lights.size() / 3, rows = 1 + (count + 255) / 256;
        ground_pixels_.assign(rows * 256 * 4, 0.0f);
        for (size_t i = 0; i < 256; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                ground_pixels_[i * 4 + c] = float(light.ramp[i * 3 + c]);
            }
        }

        for (size_t i = 0; i < count; ++i) {
            for (size_t c = 0; c < 3; ++c) {
                ground_pixels_[(256 + i) * 4 + c] = float(light.lights[i * 3 + c]);
            }
        }

        glActiveTexture(GL_TEXTURE2);
        if (!ground_light_) {
            glGenTextures(1, &ground_light_);
            glBindTexture(GL_TEXTURE_2D, ground_light_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 5, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        }

        glBindTexture(GL_TEXTURE_2D, ground_light_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, GLsizei(rows), GL_RGBA, GL_FLOAT, ground_pixels_.data());
        glActiveTexture(GL_TEXTURE0);
        applied_.unit = GL_TEXTURE0;
    }

    void quad_index_room(uint32_t quads) {
        if (quads <= quad_capacity_) {
            return;
        }

        uint32_t capacity = std::max<uint32_t>(quads, std::max<uint32_t>(quad_capacity_ * 2, 16384));
        std::vector<uint32_t> indices(size_t(capacity) * 6);
        for (uint32_t q = 0; q < capacity; ++q) {
            const uint32_t v = q * 4;
            uint32_t* i = &indices[size_t(q) * 6];
            i[0] = v;
            i[1] = v + 1;
            i[2] = v + 2;
            i[3] = v + 2;
            i[4] = v + 1;
            i[5] = v + 3;
        }

        if (!quad_indices_) {
            glGenBuffers(1, &quad_indices_);
        }

        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, quad_indices_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices.size() * sizeof(uint32_t), indices.data(), GL_STATIC_DRAW);
        quad_capacity_ = capacity;
    }

    void vertex_layout() {
        const unsigned sizes[] = {4, 3, 4, 4, 2, 2};
        const size_t offsets[] = {offsetof(RenderVertex, position), offsetof(RenderVertex, normal),
                                  offsetof(RenderVertex, diffuse),  offsetof(RenderVertex, specular),
                                  offsetof(RenderVertex, uv),       offsetof(RenderVertex, uv) + sizeof(float) * 2};
        for (unsigned i = 0; i < 6; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                                  reinterpret_cast<const void*>(offsets[i]));
        }
    }

    void current() {
        if (SDL_GL_MakeCurrent(window_, context_)) {
            throw GuestFault("cannot make native GL context current");
        }
    }

    Surface& surface(uint32_t id) {
        auto found = surfaces_.find(id);
        if (found == surfaces_.end()) {
            throw GuestFault("unknown native rendering surface");
        }

        return found->second;
    }

    // glGetUniformLocation parses the name in Apple's GL front end on every call (~7% of the window thread in the
    // open world); the program is linked once, so its locations are looked up once per name.
    // Uniform locations of the current program, keyed by the address of the (static) name: every draw sets ~60
    // uniforms, and string keys made each lookup an allocation and a string compare.
    std::unordered_map<const char*, GLint> uniforms_;
    GLuint uniforms_program_ = 0;
    GLint uniform(const char* name) {
        if (uniforms_program_ != program_) {
            uniforms_.clear();
            uniforms_program_ = program_;
        }

        auto found = uniforms_.find(name);
        if (found == uniforms_.end()) {
            found = uniforms_.emplace(name, glGetUniformLocation(program_, name)).first;
        }

        return found->second;
    }

    void integer(const char* name, int value) { glUniform1i(uniform(name), value); }
    void scalar(const char* name, float value) { glUniform1f(uniform(name), value); }
    void color(const char* name, uint32_t argb) {
        glUniform4f(uniform(name), float((argb >> 16) & 255) / 255, float((argb >> 8) & 255) / 255,
                    float(argb & 255) / 255, float(argb >> 24) / 255);
    }

    static float bits(uint32_t value) {
        float result;
        std::memcpy(&result, &value, sizeof result);
        return result;
    }

    static GLenum comparison(uint32_t value) {
        const GLenum values[] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        if (!value || value > 8) {
            throw GuestFault("invalid D3D comparison function");
        }

        return values[value - 1];
    }

    static GLenum blend(uint32_t value) {
        const GLenum values[] = {GL_ZERO,
                                 GL_ONE,
                                 GL_SRC_COLOR,
                                 GL_ONE_MINUS_SRC_COLOR,
                                 GL_SRC_ALPHA,
                                 GL_ONE_MINUS_SRC_ALPHA,
                                 GL_DST_ALPHA,
                                 GL_ONE_MINUS_DST_ALPHA,
                                 GL_DST_COLOR,
                                 GL_ONE_MINUS_DST_COLOR,
                                 GL_SRC_ALPHA_SATURATE};
        if (!value || value > 11) {
            throw GuestFault("unqualified D3D blend factor");
        }

        return values[value - 1];
    }

    static GLenum stencil(uint32_t value) {
        const GLenum values[] = {GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
        if (!value || value > 8) {
            throw GuestFault("invalid D3D stencil operation");
        }

        return values[value - 1];
    }

    static GLenum address(uint32_t value) {
        switch (value) {
        case 1:
            return GL_REPEAT;
        case 2:
            return GL_MIRRORED_REPEAT;
        case 3:
            return GL_CLAMP_TO_EDGE;
        case 4:
            return GL_CLAMP_TO_BORDER;
        default:
            throw GuestFault("unqualified D3D texture address mode");
        }
    }

    static GLuint compile(GLenum type, const char* source) {
        GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char log[4096]{};
            glGetShaderInfoLog(shader, sizeof log, nullptr, log);
            glDeleteShader(shader);
            throw GuestFault(std::string("GL 3.3 shader compilation failed: ") + log);
        }

        return shader;
    }

    static GLuint link(const char* vertex_source, const char* fragment_source) {
        GLuint vertex = compile(GL_VERTEX_SHADER, vertex_source);
        GLuint fragment = 0;
        try {
            fragment = compile(GL_FRAGMENT_SHADER, fragment_source);
        } catch (...) {
            glDeleteShader(vertex);
            throw;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[4096]{};
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            glDeleteProgram(program);
            throw GuestFault(std::string("GL 3.3 program link failed: ") + log);
        }

        return program;
    }

    void shaders() {
        if (program_) {
            return;
        }

        program_ = link(vertex_shader, fragment_shader);
        glGenVertexArrays(1, &vao_);
        glBindVertexArray(vao_);
        glGenBuffers(1, &vertices_);
        glGenBuffers(1, &indices_);
        glBindBuffer(GL_ARRAY_BUFFER, vertices_);
        vertex_layout();
    }

public:
    explicit GlRenderer(SDL_Window* window) : window_(window) {
        const char* path = nullptr;
        std::string first_error;
        // Every attempt keeps the renderer contract (core profile, at least 3.3, no extensions).
        // macOS only exposes the 3.2 and 4.1 core profiles; its first attempt is the 4.1 one and
        // no further attempts are made there.
        struct Attempt {
            const char* label;
            int major, minor, flags;
        };
#ifdef __APPLE__
        const Attempt attempts[] = {{"core-4.1", 4, 1, 0}};
#else
        const Attempt attempts[] = {{"core-3.3", 3, 3, 0},
                                    {"core-3.3-forward-compatible", 3, 3, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG},
                                    {"core-4.1-forward-compatible", 4, 1, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG}};
#endif
        for (const auto& attempt : attempts) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, attempt.flags);
            context_ = SDL_GL_CreateContext(window_);
            if (context_) {
                path = attempt.label;
                break;
            }

            if (first_error.empty()) {
                first_error = SDL_GetError();
            }
        }

        if (!context_) {
            if (const char* via = software_reexec(first_error)) {
                path = via;
            } else {
                throw GuestFault("cannot create core GL context: " + first_error);
            }
        }

        int major = 0, minor = 0, profile = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
        if (major < 3 || (major == 3 && minor < 3) || profile != SDL_GL_CONTEXT_PROFILE_CORE) {
            SDL_GL_DeleteContext(context_);
            throw GuestFault("GL 3.3 core context contract not satisfied");
        }

        std::fprintf(stderr, "SACRED_GL path=%s%s\n", path,
                     std::getenv("SACRED_GL_REEXEC") ? " (after software-rendering re-exec)" : "");
        std::fprintf(stderr, "OPENGL version=%s renderer=%s GLSL=%s\n",
                     reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                     reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                     reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)));
    }

    ~GlRenderer() override {
        SDL_GL_MakeCurrent(window_, context_);
        for (auto& pair : surfaces_) {
            glDeleteTextures(1, &pair.second.texture);
            glDeleteFramebuffers(1, &pair.second.framebuffer);
            glDeleteRenderbuffers(1, &pair.second.depth);
        }

        glDeleteTextures(1, &staging_texture_);
        glDeleteFramebuffers(1, &staging_framebuffer_);
        glDeleteProgram(program_);
        if (ground_light_) {
            glDeleteTextures(1, &ground_light_);
        }

        glDeleteProgram(resample_program_);
        glDeleteVertexArrays(1, &resample_vao_);
        glDeleteSamplers(1, &resample_sampler_);
        glDeleteBuffers(1, &vertices_);
        glDeleteBuffers(1, &indices_);
        glDeleteVertexArrays(1, &vao_);
        if (arena_vao_) {
            glDeleteVertexArrays(1, &arena_vao_);
        }

        if (skin_vao_) {
            glDeleteVertexArrays(1, &skin_vao_);
        }

        if (arena_) {
            glDeleteBuffers(1, &arena_);
        }

        if (quad_indices_) {
            glDeleteBuffers(1, &quad_indices_);
        }

        SDL_GL_DeleteContext(context_);
    }

    void erase(uint32_t argb) override {
        current();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(float((argb >> 16) & 255) / 255, float((argb >> 8) & 255) / 255, float(argb & 255) / 255,
                     float(argb >> 24) / 255);
        glClear(GL_COLOR_BUFFER_BIT);
        SDL_GL_SwapWindow(window_);
        forget();
    }

    uint32_t max_texture_size() override {
        current();
        GLint texture = 0, renderbuffer = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texture);
        glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &renderbuffer);
        return uint32_t(std::min(texture, renderbuffer));
    }

    void create_surface(uint32_t id, uint32_t guest_width, uint32_t guest_height, RenderFormat format,
                        uint32_t scale) override {
        current();
        forget();
        bool depth = format != RenderFormat::Rgba8;
        if (!guest_width || !guest_height || !scale || scale > 8 || surfaces_.count(id)) {
            throw GuestFault("invalid native rendering surface");
        }

        const uint64_t width = uint64_t(guest_width) * scale, height = uint64_t(guest_height) * scale;
        GLint limit = 0;
        glGetIntegerv(depth ? GL_MAX_RENDERBUFFER_SIZE : GL_MAX_TEXTURE_SIZE, &limit);
        if (width > uint64_t(limit) || height > uint64_t(limit)) {
            throw GuestFault("surface exceeds GL limits");
        }

        Surface result{guest_width, guest_height, 0, 0, 0, 0};
        result.scale = scale;
        if (depth) {
            glGenRenderbuffers(1, &result.depth);
            glBindRenderbuffer(GL_RENDERBUFFER, result.depth);
            bool packed = format == RenderFormat::Depth24Stencil8;
            glRenderbufferStorage(GL_RENDERBUFFER, packed ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT16, GLsizei(width),
                                  GLsizei(height));
            GLint depth_bits = 0, stencil_bits = 0;
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_DEPTH_SIZE, &depth_bits);
            glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_STENCIL_SIZE, &stencil_bits);
            if (depth_bits < (packed ? 24 : 16) || stencil_bits < (packed ? 8 : 0)) {
                glDeleteRenderbuffers(1, &result.depth);
                throw GuestFault("GL depth/stencil storage does not satisfy the advertised format");
            }

            result.stencil_bits = stencil_bits;
        } else {
            color_storage(result, nullptr);
        }

        surfaces_.emplace(id, result);
    }

    // (Re)creates the texture and framebuffer of a color surface at its scaled size, with optional initial pixels.
    void color_storage(Surface& result, const uint8_t* rgba) {
        const GLsizei width = GLsizei(result.width * result.scale), height = GLsizei(result.height * result.scale);
        glGenTextures(1, &result.texture);
        glBindTexture(GL_TEXTURE_2D, result.texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        // Every level is given its size before any pixels go in, so the texture is complete from the start.
        if (result.levels) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, GLint(result.levels));
            for (uint32_t l = 0; l <= result.levels; ++l) {
                glTexImage2D(GL_TEXTURE_2D, GLint(l), GL_RGBA8, std::max<GLsizei>(1, width >> l),
                             std::max<GLsizei>(1, height >> l), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }

            if (rgba) {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
            }
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        }

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (result.levels) {
            // The levels below from 2x2 box averages, made here: Apple's glGenerateMipmap kept a heap copy of every
            // texture it was called on (~1 MB per 2x replacement, ~450 MB in a town).
            std::vector<uint8_t> level, next;
            const uint8_t* above = rgba;
            GLsizei w = width, h = height;
            for (uint32_t l = 1; l <= result.levels && above; ++l) {
                const GLsizei nw = std::max<GLsizei>(1, w / 2), nh = std::max<GLsizei>(1, h / 2);
                next.assign(size_t(nw) * nh * 4, 0);
                for (GLsizei y = 0; y < nh; ++y) {
                    for (GLsizei x = 0; x < nw; ++x) {
                        const GLsizei x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
                        const GLsizei y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
                        const uint8_t* a = above + (size_t(y0) * w + x0) * 4;
                        const uint8_t* b = above + (size_t(y0) * w + x1) * 4;
                        const uint8_t* c = above + (size_t(y1) * w + x0) * 4;
                        const uint8_t* d = above + (size_t(y1) * w + x1) * 4;
                        uint8_t* out = &next[(size_t(y) * nw + x) * 4];
                        for (int k = 0; k < 4; ++k) {
                            out[k] = uint8_t((a[k] + b[k] + c[k] + d[k] + 2) / 4);
                        }
                    }
                }

                glTexSubImage2D(GL_TEXTURE_2D, GLint(l), 0, 0, nw, nh, GL_RGBA, GL_UNSIGNED_BYTE, next.data());
                level.swap(next);
                above = level.data();
                w = nw;
                h = nh;
            }

            if (!rgba) {
                glGenerateMipmap(GL_TEXTURE_2D); // no pixels given (not a replacement): levels as before
            }
        }

        glGenFramebuffers(1, &result.framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, result.framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, result.texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw GuestFault("incomplete native color surface");
        }
    }

    void replace(uint32_t id, uint32_t scale, const std::vector<uint8_t>& rgba) override {
        current();
        forget();
        auto& value = surface(id);
        if (!value.texture || !scale || scale > 16 || value.depth ||
            rgba.size() != uint64_t(value.width) * scale * value.height * scale * 4) {
            throw GuestFault("invalid native texture replacement");
        }

        GLint limit = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        if (uint64_t(value.width) * scale > uint64_t(limit) || uint64_t(value.height) * scale > uint64_t(limit)) {
            throw GuestFault("texture replacement exceeds GL limits");
        }

        glDeleteTextures(1, &value.texture);
        glDeleteFramebuffers(1, &value.framebuffer);
        value.texture = value.framebuffer = 0;
        value.scale = scale;
        value.levels = 0;
        while ((2u << value.levels) <= scale) {
            ++value.levels;
        }

        value.sampler_set = false;
        color_storage(value, rgba.data());
        if (glGetError() != GL_NO_ERROR) {
            throw GuestFault("native texture replacement failed");
        }
    }

    // A color texture of exactly width x height used to resample scaled surfaces (reads and magnified uploads).
    GLuint staging_texture_ = 0, staging_framebuffer_ = 0;
    uint32_t staging_width_ = 0, staging_height_ = 0;
    void staging(uint32_t width, uint32_t height) {
        if (staging_texture_ && staging_width_ == width && staging_height_ == height) {
            return;
        }

        if (!staging_texture_) {
            glGenTextures(1, &staging_texture_);
            glGenFramebuffers(1, &staging_framebuffer_);
        }

        glBindTexture(GL_TEXTURE_2D, staging_texture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, staging_framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, staging_texture_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw GuestFault("incomplete native staging surface");
        }

        staging_width_ = width;
        staging_height_ = height;
    }

    void destroy_surface(uint32_t id) override {
        current();
        forget();
        auto& value = surface(id);
        glDeleteTextures(1, &value.texture);
        glDeleteFramebuffers(1, &value.framebuffer);
        glDeleteRenderbuffers(1, &value.depth);
        surfaces_.erase(id);
    }

    void attach_depth(uint32_t target, uint32_t depth) override {
        current();
        forget();
        auto& color = surface(target);
        auto& z = surface(depth);
        if (!color.framebuffer || !z.depth || color.width != z.width || color.height != z.height ||
            color.scale != z.scale) {
            throw GuestFault("invalid depth attachment");
        }

        glBindFramebuffer(GL_FRAMEBUFFER, color.framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, z.stencil_bits ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, z.depth);
        color.stencil_bits = z.stencil_bits;
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw GuestFault("incomplete native depth attachment");
        }
    }

    void upload(uint32_t id, const std::vector<uint8_t>& rgba, uint32_t x, uint32_t y, uint32_t width,
                uint32_t height) override {
        current();
        auto& value = surface(id);
        if (!width) {
            width = value.width;
        }

        if (!height) {
            height = value.height;
        }

        if (!value.texture || uint64_t(x) + width > value.width || uint64_t(y) + height > value.height ||
            rgba.size() != uint64_t(width) * height * 4) {
            throw GuestFault("invalid RGBA surface upload");
        }

        if (value.scale == 1) {
            forget_textures();
            glBindTexture(GL_TEXTURE_2D, value.texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            return;
        }

        // Guest pixels into a scaled surface: each one becomes a scale x scale block.
        forget();
        staging(width, height);
        glBindTexture(GL_TEXTURE_2D, staging_texture_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, staging_framebuffer_);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, value.framebuffer);
        glDisable(GL_SCISSOR_TEST);
        const GLint k = GLint(value.scale);
        glBlitFramebuffer(0, 0, GLint(width), GLint(height), GLint(x) * k, GLint(y) * k, GLint(x + width) * k,
                          GLint(y + height) * k, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (value.levels) {
            glBindTexture(GL_TEXTURE_2D, value.texture);
            glGenerateMipmap(GL_TEXTURE_2D);
        }
    }

    std::vector<uint8_t> read(uint32_t id) override {
        current();
        auto& value = surface(id);
        if (!value.framebuffer) {
            throw GuestFault("unqualified depth surface readback");
        }

        std::vector<uint8_t> rgba(size_t(value.width) * value.height * 4);
        forget();
        glBindFramebuffer(GL_FRAMEBUFFER, value.framebuffer);
        if (value.scale != 1) {
            // Back to the guest size: a linear minification (an exact 2x2 box filter at scale 2).
            staging(value.width, value.height);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, value.framebuffer);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, staging_framebuffer_);
            glDisable(GL_SCISSOR_TEST);
            glBlitFramebuffer(0, 0, GLint(value.width * value.scale), GLint(value.height * value.scale), 0, 0,
                              GLint(value.width), GLint(value.height), GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, staging_framebuffer_);
        }

        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, value.width, value.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        return rgba;
    }

    void copy(uint32_t destination, int32_t x, int32_t y, uint32_t source, int32_t source_x, int32_t source_y,
              uint32_t width, uint32_t height, bool opaque) override {
        current();
        auto& to = surface(destination);
        auto& from = surface(source);
        if (!to.framebuffer || !from.framebuffer || destination == source || !width || !height || x < 0 || y < 0 ||
            source_x < 0 || source_y < 0 || uint64_t(x) + width > to.width || uint64_t(y) + height > to.height ||
            uint64_t(source_x) + width > from.width || uint64_t(source_y) + height > from.height) {
            throw GuestFault("invalid native surface copy");
        }

        forget();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, from.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to.framebuffer);
        glDisable(GL_SCISSOR_TEST);
        // Guest rectangles in both surfaces' own storage scale; a larger source is minified linearly.
        const int32_t from_k = int32_t(from.scale), to_k = int32_t(to.scale);
        glBlitFramebuffer(source_x * from_k, source_y * from_k, (source_x + int32_t(width)) * from_k,
                          (source_y + int32_t(height)) * from_k, x * to_k, y * to_k, (x + int32_t(width)) * to_k,
                          (y + int32_t(height)) * to_k, GL_COLOR_BUFFER_BIT, from_k > to_k ? GL_LINEAR : GL_NEAREST);
        if (opaque) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x * to_k, y * to_k, int32_t(width) * to_k, int32_t(height) * to_k);
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_SCISSOR_TEST);
        }

        if (glGetError() != GL_NO_ERROR) {
            throw GuestFault("native surface copy failed");
        }
    }

    void clear(uint32_t target, uint32_t flags, uint32_t argb, float depth, uint32_t stencil_value, int x, int y,
               int width, int height) override {
        current();
        if (flags & ~7u) {
            throw GuestFault("unqualified D3D clear flags");
        }

        auto& value = surface(target);
        if (!value.framebuffer) {
            throw GuestFault("invalid D3D clear target");
        }

        if ((flags & 4) && !value.stencil_bits) {
            throw GuestFault("clear requires an attached stencil buffer");
        }

        forget();
        glBindFramebuffer(GL_FRAMEBUFFER, value.framebuffer);
        if (width > 0 && height > 0) {
            const int k = int(value.scale);
            glEnable(GL_SCISSOR_TEST);
            glScissor(x * k, y * k, width * k, height * k);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glClearColor(float((argb >> 16) & 255) / 255, float((argb >> 8) & 255) / 255, float(argb & 255) / 255,
                     float(argb >> 24) / 255);
        glClearDepth(depth);
        glStencilMask(0xff);
        glClearStencil(stencil_value);
        glClear((flags & 1 ? GL_COLOR_BUFFER_BIT : 0) | (flags & 2 ? GL_DEPTH_BUFFER_BIT : 0) |
                (flags & 4 ? GL_STENCIL_BUFFER_BIT : 0));
        glDisable(GL_SCISSOR_TEST);
    }

    static bool same(const RenderLight& a, const RenderLight& b) {
        return a.type == b.type && a.enabled == b.enabled && a.diffuse == b.diffuse && a.specular == b.specular &&
               a.ambient == b.ambient && a.position == b.position && a.direction == b.direction && a.range == b.range &&
               a.falloff == b.falloff && a.theta == b.theta && a.phi == b.phi &&
               !std::memcmp(a.attenuation, b.attenuation, sizeof a.attenuation);
    }

    // Brings the GL context from applied_ to the state of one draw call.
    void apply(const DrawItem& item, const RenderState& state, const Surface& value) {
        auto& old = applied_.state;
        const bool all = !applied_.valid;
        const auto& rs = state.render;
        if (!value.framebuffer || !state.width || !state.height) {
            throw GuestFault("invalid D3D drawing target or viewport");
        }

        const bool retarget =
            all || applied_.framebuffer != value.framebuffer || applied_.stencil_bits != value.stencil_bits;
        if (retarget) {
            glBindFramebuffer(GL_FRAMEBUFFER, value.framebuffer);
        }

        if (all) {
            glDisable(GL_SCISSOR_TEST);
            glFrontFace(GL_CCW);
            glUseProgram(program_);
            integer("texture0", 0);
            integer("texture1", 1);
            integer("groundLight", 2);
            glActiveTexture(GL_TEXTURE0);
            applied_.unit = GL_TEXTURE0;
        }

        if (all || retarget || state.x != old.x || state.y != old.y || state.width != old.width ||
            state.height != old.height) {
            // The transformed-vertex mapping uses the guest viewport; only the rasterized area scales.
            const GLint k = GLint(value.scale);
            glViewport(GLint(state.x) * k, GLint(state.y) * k, GLsizei(state.width) * k, GLsizei(state.height) * k);
            glUniform4f(uniform("viewport"), state.x, state.y, state.width, state.height);
        }

        if (all || fit_.bias != applied_.bias) {
            scalar("pixelBias", fit_.bias);
            applied_.bias = fit_.bias;
        }

        if (all || state.min_z != old.min_z || state.max_z != old.max_z) {
            glDepthRange(state.min_z, state.max_z);
        }

        if (retarget || rs != old.render) {
            if (rs[7] > 1 || rs[8] < 1 || rs[8] > 3 || rs[22] < 1 || rs[22] > 3) {
                throw GuestFault("unqualified D3D raster/depth state");
            }

            if (rs[152] || rs[48] || rs[47]) {
                throw GuestFault("unqualified D3D clipping-plane, range-fog or depth-bias state");
            }

            if (rs[15]) {
                comparison(rs[25]);
            }

            if (rs[26]) {
                glEnable(GL_DITHER);
            } else {
                glDisable(GL_DITHER);
            }

            if (rs[7]) {
                glEnable(GL_DEPTH_TEST);
            } else {
                glDisable(GL_DEPTH_TEST);
            }

            glDepthFunc(comparison(rs[23]));
            glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
            if (rs[52]) {
                if (!value.stencil_bits) {
                    throw GuestFault("stencil testing requires an attached stencil buffer");
                }

                glEnable(GL_STENCIL_TEST);
                glStencilFunc(comparison(rs[56]), rs[57] & 255, rs[58] & 255);
                glStencilMask(rs[59] & 255);
                glStencilOp(stencil(rs[53]), stencil(rs[54]), stencil(rs[55]));
            } else {
                glDisable(GL_STENCIL_TEST);
            }

            if (rs[27]) {
                glEnable(GL_BLEND);
                glBlendFunc(blend(rs[19]), blend(rs[20]));
            } else {
                glDisable(GL_BLEND);
            }

            if (rs[22] == 1) {
                glDisable(GL_CULL_FACE);
            } else {
                glEnable(GL_CULL_FACE);
                glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK);
            }

            glPolygonMode(GL_FRONT_AND_BACK, rs[8] == 1 ? GL_POINT : rs[8] == 2 ? GL_LINE : GL_FILL);
            integer("lighting", rs[137]);
            integer("colorVertex", rs[141]);
            integer("normalizeNormals", rs[143]);
            integer("localViewer", rs[142]);
            integer("specularEnable", rs[29]);
            glUniform4i(uniform("materialSources"), rs[145], rs[147], rs[146], rs[148]);
            color("globalAmbient", rs[139]);
            color("textureFactor", rs[60]);
            integer("alphaTest", rs[15]);
            integer("alphaFunction", rs[25]);
            scalar("alphaReference", float(rs[24] & 255) / 255);
            integer("fogEnable", rs[28]);
            integer("fogMode", rs[35] ? rs[35] : rs[140]);
            color("fogColor", rs[34]);
            glUniform3f(uniform("fogParameters"), bits(rs[36]), bits(rs[37]), bits(rs[38]));
        }

        static const char* const matrices[] = {"world", "view", "projection", "texMatrix0", "texMatrix1"};
        for (unsigned i = 0; i < 5; ++i) {
            if (all || state.matrices[i] != old.matrices[i]) {
                glUniformMatrix4fv(uniform(matrices[i]), 1, GL_FALSE, state.matrices[i].data());
            }
        }

        if (all || item.transformed != applied_.transformed) {
            integer("transformed", item.transformed);
        }

        if (all || item.diffuse != applied_.diffuse) {
            integer("hasDiffuse", item.diffuse);
        }

        if (all || item.specular != applied_.specular) {
            integer("hasSpecular", item.specular);
        }

        if (all || state.material != old.material) {
            glUniform4fv(uniform("material[0]"), 4, state.material[0].data());
        }

        if (all || state.power != old.power) {
            scalar("power", state.power);
        }

        // Names built once; their addresses are the cache keys.
        static const auto light_names = [] {
            std::vector<std::array<std::string, 8>> names;
            for (unsigned i = 0; i < 64; ++i) {
                const std::string index = "[" + std::to_string(i) + "]";
                names.push_back({"lightType" + index, "lightDiffuse" + index, "lightAmbient" + index,
                                 "lightSpecular" + index, "lightPosition" + index, "lightDirection" + index,
                                 "lightAttenuation" + index, "lightSpot" + index});
            }

            return names;
        }();
        if (state.lights.size() > light_names.size()) {
            throw GuestFault("unqualified D3D light count");
        }

        for (unsigned i = 0; i < state.lights.size(); ++i) {
            const auto& light = state.lights[i];
            if (!all && same(light, old.lights[i])) {
                continue;
            }

            const auto& name = light_names[i];
            integer(name[0].c_str(), light.enabled ? light.type : 0);
            glUniform4fv(uniform(name[1].c_str()), 1, light.diffuse.data());
            glUniform4fv(uniform(name[2].c_str()), 1, light.ambient.data());
            glUniform4fv(uniform(name[3].c_str()), 1, light.specular.data());
            glUniform3fv(uniform(name[4].c_str()), 1, light.position.data());
            glUniform3fv(uniform(name[5].c_str()), 1, light.direction.data());
            glUniform3fv(uniform(name[6].c_str()), 1, light.attenuation);
            glUniform4f(uniform(name[7].c_str()), light.range, light.falloff, light.theta, light.phi);
        }

        if (all || state.stages != old.stages) {
            for (const auto& stage : state.stages) {
                for (unsigned op : {1u, 4u}) {
                    if (stage[op] < 1 || stage[op] > 16) {
                        throw GuestFault("unqualified D3D texture operation");
                    }
                }

                for (unsigned arg : {2u, 3u, 5u, 6u}) {
                    if ((stage[arg] & ~0x3fu) || (stage[arg] & 15) > 4) {
                        throw GuestFault("unqualified D3D texture argument");
                    }
                }
            }

            glUniform2i(uniform("texCount"), state.stages[0][24], state.stages[1][24]);
            static const char* const uniforms[] = {"colorOp",   "alphaOp",   "colorArg1", "colorArg2",
                                                   "alphaArg1", "alphaArg2", "coordinate"};
            const unsigned states[] = {1, 4, 2, 3, 5, 6, 11};
            for (unsigned i = 0; i < 7; ++i) {
                GLint values[2] = {GLint(state.stages[0][states[i]]), GLint(state.stages[1][states[i]])};
                if (i == 6 && (values[0] > 1 || values[1] > 1)) {
                    throw GuestFault("unqualified generated texture coordinates");
                }

                glUniform2iv(uniform(uniforms[i]), 1, values);
            }
        }

        const bool rebind = all || !applied_.textures_valid;
        const std::array<GLint, 2> bound = {item.textures[0] != 0, item.textures[1] != 0};
        if (all || bound != applied_.bound) {
            glUniform2iv(uniform("textureBound"), 1, bound.data());
        }

        for (unsigned stage = 0; stage < 2; ++stage) {
            Surface* texture = item.textures[stage] ? &surface(item.textures[stage]) : nullptr;
            const GLuint name = texture ? texture->texture : 0;
            if (rebind || name != applied_.texture[stage]) {
                unit(GL_TEXTURE0 + stage);
                glBindTexture(GL_TEXTURE_2D, name);
                applied_.texture[stage] = name;
            }

            if (!texture) {
                continue;
            }

            const auto& ts = state.stages[stage];
            if ((ts[16] != 1 && ts[16] != 2) || (ts[17] != 1 && ts[17] != 2) || (ts[24] && ts[24] != 2)) {
                throw GuestFault("unqualified D3D texture filtering or transform");
            }

            // Render scale: an axis that stays within a texel of [0,1] samples with CLAMP (see ItemFit).
            const uint32_t address_u = (fit_.clamp[stage] & 1) ? 3 : ts[13],
                           address_v = (fit_.clamp[stage] & 2) ? 3 : ts[14];
            const std::array<uint32_t, 5> sampler = {ts[16], ts[17], address_u, address_v, ts[15]};
            if (texture->sampler_set && texture->sampler == sampler) {
                continue;
            }

            const GLenum wrap_s = address(address_u), wrap_t = address(address_v);
            unit(GL_TEXTURE0 + stage);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, ts[16] == 1 ? GL_NEAREST : GL_LINEAR);
            // A replaced texture minifies through its few mipmap levels with the guest's filter kind.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                            texture->levels ? (ts[17] == 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_LINEAR)
                                            : (ts[17] == 1 ? GL_NEAREST : GL_LINEAR));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap_s);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap_t);
            const GLfloat border[] = {float((ts[15] >> 16) & 255) / 255, float((ts[15] >> 8) & 255) / 255,
                                      float(ts[15] & 255) / 255, float(ts[15] >> 24) / 255};
            glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, border);
            texture->sampler = sampler;
            texture->sampler_set = true;
        }

        applied_.framebuffer = value.framebuffer;
        applied_.stencil_bits = value.stencil_bits;
        applied_.state = state;
        applied_.transformed = item.transformed;
        applied_.diffuse = item.diffuse;
        applied_.specular = item.specular;
        applied_.bound = bound;
        applied_.valid = applied_.textures_valid = true;
    }

    // How one draw item is fitted to a scaled target (render scale; everything stays default at scale 1).
    //  * bias: the window offset of transformed (XYZRHW) positions, in guest pixels. D3D puts pixel centres at
    //    integer coordinates, so the default +0.5 maps them onto GL's centres and, at scale k, onto the centre of
    //    each k x k block. Geometry whose vertices all sit on integer coordinates (the 2D screens: menu backdrop
    //    tiles, panels, text) has its edges through pixel centres instead, which the top-left rule turns into
    //    pixel boundaries at scale 1; at scale k the same edges would cut guest pixels in half and show a sliver
    //    of the neighbouring quad. For such items the bias is 0.5/k: every edge lands on a guest-pixel boundary
    //    and covers exactly the pixels it covers at scale 1.
    //  * clamp, per stage, bit 0 (u) / bit 1 (v): sample that axis with CLAMP_TO_EDGE instead of the guest's
    //    address mode. The outermost fragments of a quad now sample up to a texel past what scale 1 reaches (the
    //    backdrop tiles map 256.5 texels onto 256 pixels); under WRAP that pulls in the opposite edge of the
    //    tile. When every coordinate of the axis stays within one texel of [0,1], scale 1 never wraps, so
    //    clamping only removes the seam.
    struct ItemFit {
        float bias = 0.5f;
        std::array<uint8_t, 2> clamp{};
        bool operator!=(const ItemFit& other) const { return bias != other.bias || clamp != other.clamp; }
    } fit_;
    ItemFit fit(const DrawBatch& batch, const DrawItem& item, const RenderState& state, const Surface& target) {
        ItemFit result;
        if (target.scale == 1) {
            return result;
        }

        const RenderVertex* vertices = batch.vertices.data() + item.first_vertex;
        if (item.transformed) {
            bool aligned = item.vertex_count != 0;
            for (uint32_t i = 0; i < item.vertex_count && aligned; ++i) {
                for (unsigned axis = 0; axis < 2; ++axis) {
                    const float value = vertices[i].position[axis];
                    aligned &= std::fabs(value - std::nearbyint(value)) < 1.0f / 64;
                }
            }

            if (aligned) {
                result.bias = 0.5f / float(target.scale);
            }
        }

        for (unsigned stage = 0; stage < 2; ++stage) {
            const auto& ts = state.stages[stage];
            if (!item.textures[stage] || ts[24] || ts[11] > 1) {
                continue;
            }

            const Surface& texture = surface(item.textures[stage]);
            const float slack[2] = {1.0f / float(texture.width), 1.0f / float(texture.height)};
            uint8_t inside = 3;
            for (uint32_t i = 0; i < item.vertex_count && inside; ++i) {
                const float* uv = vertices[i].uv[ts[11]];
                for (unsigned axis = 0; axis < 2; ++axis) {
                    if (!(uv[axis] >= -slack[axis] && uv[axis] <= 1.0f + slack[axis])) {
                        inside &= uint8_t(~(1u << axis));
                    }
                }
            }

            result.clamp[stage] = inside;
        }

        return result;
    }

    void mesh_write(uint32_t id, uint32_t first, const std::vector<RenderVertex>& vertices) override {
        if (vertices.empty()) {
            return;
        }

        current();
        shaders();
        forget();
        Mesh& mesh = meshes_[id];
        const uint32_t end = first + uint32_t(vertices.size());
        if (first > mesh.vertices.size()) {
            throw GuestFault("mesh write past its end");
        }

        if (mesh.vertices.size() < end) {
            mesh.vertices.resize(end);
        }

        std::copy(vertices.begin(), vertices.end(), mesh.vertices.begin() + first);
        glBindVertexArray(vao_);
        if (end > mesh.capacity) {
            // A larger range (a ground group holds ~10 quads), with all the mesh's vertices uploaded there. If the
            // arena is packed for it, the old range moves along and is dropped at the next packing.
            const uint32_t capacity = std::max<uint32_t>(end + end / 2, std::max<uint32_t>(mesh.capacity * 2, 64));
            quad_index_room((capacity + 3) / 4);
            mesh.offset = arena_take(capacity);
            mesh.capacity = capacity;
            glBindBuffer(GL_ARRAY_BUFFER, arena_);
            glBufferSubData(GL_ARRAY_BUFFER, size_t(mesh.offset) * sizeof(RenderVertex),
                            size_t(end) * sizeof(RenderVertex), mesh.vertices.data());
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, arena_);
            glBufferSubData(GL_ARRAY_BUFFER, size_t(mesh.offset + first) * sizeof(RenderVertex),
                            vertices.size() * sizeof(RenderVertex), vertices.data());
        }

        glBindBuffer(GL_ARRAY_BUFFER, vertices_);
        mesh.used = std::max(mesh.used, end);
    }

    void mesh_destroy(uint32_t id) override {
        auto found = meshes_.find(id);
        if (found == meshes_.end()) {
            return;
        }

        meshes_.erase(found); // its range comes back when the arena is next packed
    }

    void draw(const DrawBatch& batch) override {
        if (batch.items.empty()) {
            return;
        }

        current();
        shaders();
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vertices_);
        glBufferData(GL_ARRAY_BUFFER, batch.vertices.size() * sizeof(RenderVertex), batch.vertices.data(),
                     GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, indices_);
        if (!batch.indices.empty()) {
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, batch.indices.size() * sizeof(uint16_t), batch.indices.data(),
                         GL_STREAM_DRAW);
        }

        const GLenum modes[] = {GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
        const RenderState* state = nullptr;
        const DrawItem* previous = nullptr;
        GLuint bound_vao = vao_;  // ground meshes keep theirs bound across consecutive mesh draws
        bool ground_sent = false; // this batch's ground light is in the uniforms
        if (batch.ground_light.on) {
            ground_light_upload(batch.ground_light);
        }

        try {
            for (const auto& item : batch.items) {
                if (item.mesh) {
                    // The ranges to draw: the item's own, or its list in the batch.
                    multi_counts_.clear();
                    multi_offsets_.clear();
                    multi_bases_.clear();
                    const MeshRange single{item.mesh, item.first_quad, item.quads};
                    const MeshRange* ranges = &single;
                    uint32_t range_count = 1;
                    if (item.ranges) {
                        if (uint64_t(item.first_range) + item.ranges > batch.mesh_ranges.size()) {
                            throw GuestFault("invalid mesh draw");
                        }

                        ranges = &batch.mesh_ranges[item.first_range];
                        range_count = item.ranges;
                    }

                    if (item.state >= batch.states.size()) {
                        throw GuestFault("invalid mesh draw");
                    }

                    for (uint32_t r = 0; r < range_count; ++r) {
                        auto found = meshes_.find(ranges[r].mesh);
                        if (found == meshes_.end() ||
                            (uint64_t(ranges[r].first_quad) + ranges[r].quads) * 4 > found->second.used ||
                            ranges[r].first_quad + ranges[r].quads > quad_capacity_) {
                            throw GuestFault("invalid mesh draw");
                        }

                        multi_counts_.push_back(GLsizei(ranges[r].quads) * 6);
                        multi_offsets_.push_back(
                            reinterpret_cast<const void*>(uintptr_t(ranges[r].first_quad) * 6 * sizeof(uint32_t)));
                        multi_bases_.push_back(GLint(found->second.offset));
                    }

                    Surface& target = surface(item.target);
                    ItemFit fitted;
                    if (target.scale != 1) {
                        fitted.clamp = item.mesh_clamp;
                    }

                    if (!applied_.valid || !applied_.textures_valid || !previous || previous->state != item.state ||
                        previous->target != item.target || previous->textures != item.textures ||
                        previous->transformed != item.transformed || previous->diffuse != item.diffuse ||
                        previous->specular != item.specular || fitted != fit_) {
                        state = &batch.states[item.state];
                        fit_ = fitted;
                        apply(item, *state, target);
                    }

                    previous = &item;
                    if (!mesh_applied_) {
                        integer("meshOn", 1);
                        mesh_applied_ = true;
                    }

                    glUniform4fv(uniform("meshTransform"), 1, item.mesh_transform);
                    if (item.mesh_lit) {
                        const GroundLight& light = batch.ground_light;
                        if (!light.on) {
                            throw GuestFault("invalid lit mesh draw");
                        }

                        if (!ground_sent || !ground_lit_applied_) {
                            glUniform2i(uniform("groundLit"), 1, GLint(light.lights.size() / 3));
                            ground_lit_applied_ = true;
                        }

                        if (!ground_sent) {
                            glUniform4f(uniform("groundView"), light.inverse_zoom, light.zoom, light.origin[0],
                                        light.origin[1]);
                            glUniform2iv(uniform("groundCorners[0]"), 4, light.corners);
                            ground_sent = true;
                        }

                        glUniform2fv(uniform("groundOffset"), 1, item.mesh_light);
                    } else if (ground_lit_applied_) {
                        glUniform2i(uniform("groundLit"), 0, 0);
                        ground_lit_applied_ = false;
                    }

                    if (bound_vao != arena_vao_) {
                        bound_vao = arena_vao_;
                        glBindVertexArray(bound_vao);
                    }

                    if (range_count == 1) {
                        glDrawElementsBaseVertex(GL_TRIANGLES, multi_counts_[0], GL_UNSIGNED_INT, multi_offsets_[0],
                                                 multi_bases_[0]);
                    } else {
                        glMultiDrawElementsBaseVertex(GL_TRIANGLES, multi_counts_.data(), GL_UNSIGNED_INT,
                                                      multi_offsets_.data(), GLsizei(range_count), multi_bases_.data());
                    }

                    continue;
                }

                const GLuint item_vao = item.skin_mesh ? skin_vao_ : vao_;
                if (bound_vao != item_vao) {
                    bound_vao = item_vao;
                    glBindVertexArray(item_vao);
                }

                if (mesh_applied_) {
                    integer("meshOn", 0);
                    mesh_applied_ = false;
                }

                GLint base_vertex = GLint(item.first_vertex);
                if (item.skin_mesh) {
                    auto found = meshes_.find(item.skin_mesh);
                    if (found == meshes_.end() || item.vertex_count > found->second.used || !item.index_count ||
                        !item.skin_bones) {
                        throw GuestFault("invalid skinned mesh draw");
                    }

                    base_vertex = GLint(found->second.offset);
                    integer("skinColorOn", item.skin_color_on ? 1 : 0);
                    if (item.skin_color_on) {
                        glUniform4fv(uniform("skinColor"), 1, item.skin_color);
                    }
                }

                if (item.skin_bones) {
                    if (uint64_t(item.skin_first) + uint64_t(item.skin_bones) * 3 > batch.skin_rows.size() / 4 ||
                        item.skin_bones > 64) {
                        throw GuestFault("invalid skinned draw");
                    }

                    if (!skin_applied_) {
                        integer("skinOn", 1);
                        skin_applied_ = true;
                    }

                    if (!item.skin_mesh) {
                        integer("skinColorOn", 0);
                    }

                    integer("skinNormalize", item.skin_normalize ? 1 : 0);
                    glUniform4fv(uniform("bones"), GLsizei(item.skin_bones * 3),
                                 &batch.skin_rows[size_t(item.skin_first) * 4]);
                } else if (skin_applied_) {
                    integer("skinOn", 0);
                    skin_applied_ = false;
                }

                if (item.primitive < 1 || item.primitive > 6) {
                    throw GuestFault("invalid D3D primitive type");
                }

                if (item.state >= batch.states.size() ||
                    (!item.skin_mesh && uint64_t(item.first_vertex) + item.vertex_count > batch.vertices.size()) ||
                    uint64_t(item.first_index) + item.index_count > batch.indices.size()) {
                    throw GuestFault("invalid D3D draw batch");
                }

                for (uint32_t i = 0; i < item.index_count; ++i) {
                    if (batch.indices[item.first_index + i] >= item.vertex_count) {
                        throw GuestFault("D3D index exceeds vertex buffer");
                    }
                }

                Surface& target = surface(item.target);
                ItemFit fitted;
                if (!item.skin_mesh) {
                    fitted = fit(batch, item, batch.states[item.state], target);
                } else if (target.scale != 1) {
                    fitted.clamp = item.mesh_clamp;
                }

                // Nothing to send when only the geometry differs from the previous draw call of the batch.
                if (!applied_.valid || !applied_.textures_valid || !previous || previous->state != item.state ||
                    previous->target != item.target || previous->textures != item.textures ||
                    previous->transformed != item.transformed || previous->diffuse != item.diffuse ||
                    previous->specular != item.specular || fitted != fit_) {
                    state = &batch.states[item.state];
                    fit_ = fitted;
                    apply(item, *state, target);
                }

                previous = &item;
                if (!item.index_count) {
                    glDrawArrays(modes[item.primitive - 1], item.first_vertex, item.vertex_count);
                } else {
                    glDrawElementsBaseVertex(
                        modes[item.primitive - 1], item.index_count, GL_UNSIGNED_SHORT,
                        reinterpret_cast<const void*>(uintptr_t(item.first_index) * sizeof(uint16_t)), base_vertex);
                }
            }
        } catch (...) {
            glBindVertexArray(vao_);
            forget();
            throw;
        }

        if (bound_vao != vao_) {
            glBindVertexArray(vao_);
        }

        GLenum error = glGetError();
        if (error != GL_NO_ERROR) {
            forget();
            throw GuestFault("native GL draw error " + std::to_string(error));
        }

        flush_now_and_then();
    }

    void present(uint32_t id, int32_t x, int32_t y, uint32_t source_width, uint32_t source_height, uint32_t frame_width,
                 uint32_t frame_height, int32_t inset_x, int32_t inset_y, uint32_t inset_width, uint32_t inset_height,
                 const PresentBackdrop* backdrop) override {
        current();
        auto& value = surface(id);
        if (!value.framebuffer) {
            throw GuestFault("invalid presentation surface");
        }

        if (!source_width) {
            source_width = value.width;
        }

        if (!source_height) {
            source_height = value.height;
        }

        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window_, &width, &height);
        forget();
        glBindFramebuffer(GL_READ_FRAMEBUFFER, value.framebuffer);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        // Keep the guest aspect ratio in a drawable of another shape (fullscreen, HiDPI, a window manager that
        // changed the size): centre the image with black bars and filter only when it is actually scaled.
        Letterbox box = fit_letterbox(int(source_width), int(source_height), width, height);
        bool inset = false;
        if (frame_width && frame_height) {
            // An inset is a box of the full frame (frame units), placed and scaled as that frame is.
            if (!inset_width) {
                inset_width = source_width;
            }

            if (!inset_height) {
                inset_height = source_height;
            }

            const Letterbox frame = fit_letterbox(int(frame_width), int(frame_height), width, height);
            auto scale = [](int64_t value, int to, uint32_t from) {
                return int(value * to / int64_t(from));
            };
            box.x = frame.x + scale(inset_x, frame.width, frame_width);
            box.y = frame.y + scale(inset_y, frame.height, frame_height);
            box.width = scale(inset_x + int64_t(inset_width), frame.width, frame_width) + frame.x - box.x;
            box.height = scale(inset_y + int64_t(inset_height), frame.height, frame_height) + frame.y - box.y;
            box.scaled = box.width != int(source_width) || box.height != int(source_height);
            inset = true;
            if (backdrop && backdrop->id && backdrop->width > 0 && backdrop->height > 0) {
                // Behind the inset, inside the letterboxed frame only.
                const Surface& image = surface(backdrop->id);
                Letterbox area;
                area.x = frame.x + scale(backdrop->x, frame.width, frame_width);
                area.y = frame.y + scale(backdrop->y, frame.height, frame_height);
                area.width = scale(int64_t(backdrop->x) + backdrop->width, frame.width, frame_width) + frame.x - area.x;
                area.height =
                    scale(int64_t(backdrop->y) + backdrop->height, frame.height, frame_height) + frame.y - area.y;
                glEnable(GL_SCISSOR_TEST);
                glScissor(frame.x, height - frame.y - frame.height, frame.width, frame.height);
                resample(image, 0, 0, image.width, image.height, area, height, &backdrop->shade);
                glDisable(GL_SCISSOR_TEST);
            }
        }

        if (inset && (box.scaled || value.scale != 1) && box.width > 0 && box.height > 0) {
            resample(value, x, y, source_width, source_height, box, height);
        } else {
            // Surface row zero is guest top, including rendered textures/readbacks.
            // The one presentation flip maps that row to the native window top.
            const int64_t k = value.scale;
            glBlitFramebuffer(GLint(x * k), GLint(y * k), GLint((int64_t(x) + source_width) * k),
                              GLint((int64_t(y) + source_height) * k), box.x, height - box.y, box.x + box.width,
                              height - box.y - box.height, GL_COLOR_BUFFER_BIT,
                              box.scaled || k != 1 ? GL_LINEAR : GL_NEAREST);
        }

        const int64_t k = value.scale;
        for (const auto& [path, storage] : captures_) {
            save_capture(path);
            // Render scale: the presented rectangle at its storage resolution too ("<path>-x<k>.bmp"), since the
            // drawable shows it resampled to the window.
            if (k != 1 && storage) {
                save_storage(value, path.substr(0, path.rfind('.')) + "-x" + std::to_string(k) + ".bmp", GLint(x * k),
                             GLint(y * k), GLsizei(source_width * k), GLsizei(source_height * k));
            }
        }

        captures_.clear();
        draw_badge(height);
        draw_fps(frame_width && frame_height ? fit_letterbox(int(frame_width), int(frame_height), width, height) : box,
                 height);
        gpu_time_frame();
        const auto swap_start = std::chrono::steady_clock::now();
        SDL_GL_SwapWindow(window_);
        perf_log(std::chrono::steady_clock::now() - swap_start);
    }

    void capture_next(const std::string& path, bool storage) override { captures_.emplace_back(path, storage); }

private:
    // Draws [x, x+width) x [y, y+height) of the surface (guest units) into `box` of the drawable (top-left origin)
    // with the resampling program (shaders.h): HD mode insets, scaled to the window, sharper than a linear blit
    // and without the frame around the rectangle bleeding in.
    void resample(const Surface& value, int32_t x, int32_t y, uint32_t width, uint32_t height, const Letterbox& box,
                  int drawable_height, const std::array<float, 4>* shade = nullptr) {
        if (!resample_program_) {
            resample_program_ = link(resample_vertex_shader, resample_fragment_shader);
            glGenVertexArrays(1, &resample_vao_);
            glGenSamplers(1, &resample_sampler_);
            glSamplerParameteri(resample_sampler_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(resample_sampler_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glSamplerParameteri(resample_sampler_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(resample_sampler_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            resample_source_ = glGetUniformLocation(resample_program_, "source");
            resample_spread_ = glGetUniformLocation(resample_program_, "spread");
            resample_shade_ = glGetUniformLocation(resample_program_, "shade");
            glUseProgram(resample_program_);
            glUniform1i(glGetUniformLocation(resample_program_, "image"), 0);
        }

        forget_textures();
        const float k = float(value.scale);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_CULL_FACE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glViewport(box.x, drawable_height - box.y - box.height, box.width, box.height);
        glUseProgram(resample_program_);
        glUniform4f(resample_source_, float(x) * k, float(y) * k, float(int64_t(x) + width) * k,
                    float(int64_t(y) + height) * k);
        if (shade) {
            glUniform4f(resample_shade_, (*shade)[0], (*shade)[1], (*shade)[2], (*shade)[3]);
        } else {
            glUniform4f(resample_shade_, 0, 0, 0, 1);
        }

        glUniform2f(resample_spread_, std::clamp(float(width) * k / float(box.width), 1.0f, 4.0f),
                    std::clamp(float(height) * k / float(box.height), 1.0f, 4.0f));
        glActiveTexture(GL_TEXTURE0);
        applied_.unit = GL_TEXTURE0;
        glBindTexture(GL_TEXTURE_2D, value.texture);
        glBindSampler(0, resample_sampler_);
        glBindVertexArray(resample_vao_);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindSampler(0, 0);
        glBindVertexArray(vao_);
        forget();
    }

    GLuint resample_program_ = 0, resample_vao_ = 0, resample_sampler_ = 0;
    GLint resample_source_ = -1, resample_spread_ = -1, resample_shade_ = -1;
    // The QA badge (renderer.h PresentBadge), after the captures so that evidence images never contain it: scissored
    // clears of the window framebuffer, digits from a 3x5 cell font.
    void draw_badge(int height) {
        PresentBadge& badge = present_badge();
        const uint32_t argb = badge.argb.load(std::memory_order_relaxed);
        if (!argb) {
            return;
        }

        const int unit = std::max(2, height / 180);
        auto fill = [&](int x, int y, int w, int h, uint32_t color) {
            glScissor(x * unit, height - (y + h) * unit, w * unit, h * unit);
            glClearColor(float((color >> 16) & 255) / 255, float((color >> 8) & 255) / 255, float(color & 255) / 255,
                         1);
            glClear(GL_COLOR_BUFFER_BIT);
        };
        static const uint16_t glyphs[10] = {075557, 026227, 071747, 071717, 055711,
                                            074717, 074757, 071111, 075757, 075717};
        const std::string digits =
            badge.number ? std::to_string(badge.number.load(std::memory_order_relaxed)) : std::string();
        glEnable(GL_SCISSOR_TEST);
        fill(1, 1, 7 + 4 * int(digits.size()) + (digits.empty() ? 0 : 1), 7, 0xff000000u);
        fill(2, 2, 5, 5, argb);
        for (size_t i = 0; i < digits.size(); ++i) {
            for (int row = 0; row < 5; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if ((glyphs[digits[i] - '0'] >> ((4 - row) * 3 + (2 - column))) & 1) {
                        fill(8 + int(i) * 4 + column, 2 + row, 1, 1, 0xffffffffu);
                    }
                }
            }
        }

        glDisable(GL_SCISSOR_TEST);
    }

    // The frame rate counter (renderer.h present_fps_counter): "NN FPS" in the 3x5 cell font on a black box at the
    // top-left of the game's image (`image`, drawable pixels, top-left origin), below the QA badge when that shows.
    void draw_fps(const Letterbox& image, int height) {
        if (!present_fps_counter().load(std::memory_order_relaxed)) {
            fps_window_frames_ = 0;
            fps_shown_ = -1;
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (!fps_window_frames_++) {
            fps_window_start_ = now;
        }

        const double elapsed = std::chrono::duration<double>(now - fps_window_start_).count();
        if (elapsed >= 0.5) {
            // Frames between the window's first present and this one, over the time between them.
            fps_shown_ = int((fps_window_frames_ - 1) / elapsed + 0.5);
            fps_window_frames_ = 1;
            fps_window_start_ = now;
        }

        const std::string text = (fps_shown_ < 0 ? std::string("--") : std::to_string(fps_shown_)) + " FPS";
        const int unit = std::max(2, image.height / 270);
        const int left = std::max(0, image.x) + unit,
                  top = std::max(0, image.y) + unit +
                        (present_badge().argb.load(std::memory_order_relaxed) ? 8 * std::max(2, height / 180) : 0);
        auto fill = [&](int x, int y, int w, int h, float shade) {
            glScissor(left + x * unit, height - top - (y + h) * unit, w * unit, h * unit);
            glClearColor(shade, shade, shade, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        };
        glEnable(GL_SCISSOR_TEST);
        fill(0, 0, 4 * int(text.size()) + 1, 7, 0);
        for (size_t i = 0; i < text.size(); ++i) {
            const uint16_t glyph = cell_glyph(text[i]);
            for (int row = 0; row < 5; ++row) {
                for (int column = 0; column < 3; ++column) {
                    if ((glyph >> ((4 - row) * 3 + (2 - column))) & 1) {
                        fill(1 + int(i) * 4 + column, 1 + row, 1, 1, 1);
                    }
                }
            }
        }

        glDisable(GL_SCISSOR_TEST);
    }

    // A character of the 3x5 cell font as five rows of three bits, top row in the high bits; 0 for a blank.
    static uint16_t cell_glyph(char c) {
        static const uint16_t digits[10] = {075557, 026227, 071747, 071717, 055711,
                                            074717, 074757, 071111, 075757, 075717};
        if (c >= '0' && c <= '9') {
            return digits[c - '0'];
        }

        switch (c) {
        case 'F':
            return 074644;
        case 'P':
            return 075744;
        case 'S':
            return 074717;
        case '-':
            return 000700;
        }

        return 0;
    }

    // Apple's GL hands the frame's commands to the GPU only at the swap, so the GPU started a frame only once the
    // window thread had encoded all of it and the swap then waited for the whole GPU frame (~4 ms in the zoomed-out
    // city at render scale 2). Flushing about every millisecond of draw calls lets the GPU work alongside; more
    // often costs more per flush than it gains. SACRED_GL_FLUSH_US sets the interval in microseconds, 0 turns it off.
    void flush_now_and_then() {
        if (flush_interval_us_ < 0) {
            const char* value = std::getenv("SACRED_GL_FLUSH_US");
            flush_interval_us_ = value && *value ? std::max(0, std::atoi(value)) : 1000;
            last_flush_ = std::chrono::steady_clock::now();
        }

        if (!flush_interval_us_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_flush_ < std::chrono::microseconds(flush_interval_us_)) {
            return;
        }

        glFlush();
        last_flush_ = now;
    }

    int flush_interval_us_ = -1;
    std::chrono::steady_clock::time_point last_flush_{};
    // SACRED_PERF_LOG=1: once a second a PERF_GL line, the window thread's CPU time per frame (busy share) and the
    // time it waited in the swap. With the PERF line of the game thread: both well below 100% busy means the GPU
    // limits.
    void perf_log(std::chrono::steady_clock::duration swap) {
        if (perf_log_ < 0) {
            const char* on = std::getenv("SACRED_PERF_LOG");
            perf_log_ = on && *on == '1';
        }

        if (!perf_log_) {
            return;
        }

        timespec t{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
        const uint64_t cpu = uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec);
        const auto now = std::chrono::steady_clock::now();
        if (!perf_frames_++) {
            perf_start_ = now;
            perf_cpu_ns_ = cpu;
            perf_swap_ = {};
            return;
        }

        perf_swap_ += swap;
        const double seconds = std::chrono::duration<double>(now - perf_start_).count();
        if (seconds < 1.0) {
            return;
        }

        const double frames = double(perf_frames_ - 1);
        std::fprintf(stderr, "PERF_GL gl_cpu_ms=%.2f gl_busy=%.0f%% swap_ms=%.2f\n",
                     double(cpu - perf_cpu_ns_) / 1e6 / frames, double(cpu - perf_cpu_ns_) / 1e7 / seconds,
                     std::chrono::duration<double, std::milli>(perf_swap_).count() / frames);
        perf_frames_ = 1;
        perf_start_ = now;
        perf_cpu_ns_ = cpu;
        perf_swap_ = {};
    }

    int perf_log_ = -1;
    uint64_t perf_frames_ = 0, perf_cpu_ns_ = 0;
    std::chrono::steady_clock::time_point perf_start_{};
    std::chrono::steady_clock::duration perf_swap_{};
    // SACRED_GPU_TIME=1: GPU time from one present to the next (GL_TIME_ELAPSED, read a few frames late so the
    // query never stalls), as a stderr line per 300 presents.
    void gpu_time_frame() {
        if (gpu_time_ < 0) {
            const char* on = std::getenv("SACRED_GPU_TIME");
            gpu_time_ = on && *on == '1';
            if (gpu_time_) {
                glGenQueries(GLsizei(gpu_queries_.size()), gpu_queries_.data());
            }
        }

        if (!gpu_time_) {
            return;
        }

        const size_t ring = gpu_queries_.size();
        if (gpu_query_open_) {
            glEndQuery(GL_TIME_ELAPSED);
        }

        if (gpu_frame_ >= ring) {
            GLuint64 ns = 0;
            GLint ready = 0;
            glGetQueryObjectiv(gpu_queries_[gpu_frame_ % ring], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (ready) {
                glGetQueryObjectui64v(gpu_queries_[gpu_frame_ % ring], GL_QUERY_RESULT, &ns);
                gpu_sum_ns_ += ns;
                gpu_max_ns_ = std::max<uint64_t>(gpu_max_ns_, ns);
                ++gpu_count_;
            }
        }

        if (gpu_count_ == 300) {
            std::fprintf(stderr, "SACRED_GPU_TIME avg_ms=%.2f max_ms=%.2f\n", double(gpu_sum_ns_) / 300 / 1e6,
                         double(gpu_max_ns_) / 1e6);
            gpu_sum_ns_ = gpu_max_ns_ = 0;
            gpu_count_ = 0;
        }

        glBeginQuery(GL_TIME_ELAPSED, gpu_queries_[gpu_frame_ % ring]);
        gpu_query_open_ = true;
        ++gpu_frame_;
    }

    int gpu_time_ = -1;
    std::array<GLuint, 6> gpu_queries_{};
    bool gpu_query_open_ = false;
    uint64_t gpu_frame_ = 0, gpu_sum_ns_ = 0, gpu_max_ns_ = 0, gpu_count_ = 0;
    std::chrono::steady_clock::time_point fps_window_start_{};
    int64_t fps_window_frames_ = 0;
    int fps_shown_ = -1;
    void save_storage(const Surface& value, const std::string& path, GLint x, GLint y, GLsizei width, GLsizei height) {
        std::vector<uint8_t> pixels(size_t(width) * height * 4);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, value.framebuffer);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()); // storage row 0 is the top
        if (glGetError() != GL_NO_ERROR) {
            throw GuestFault("native storage capture failed");
        }

        SDL_Surface* image =
            SDL_CreateRGBSurfaceWithFormatFrom(pixels.data(), width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
        if (!image) {
            throw GuestFault("cannot create native capture image");
        }

        const int result = SDL_SaveBMP(image, path.c_str());
        SDL_FreeSurface(image);
        if (result) {
            throw GuestFault("cannot save native storage capture");
        }

        std::fprintf(stderr, "FRAME_CAPTURE path=%s source=scaled_surface_storage\n", path.c_str());
    }

    void save_capture(const std::string& path) {
        current();
        int width = 0, height = 0;
        SDL_GL_GetDrawableSize(window_, &width, &height);
        if (width <= 0 || height <= 0) {
            throw GuestFault("cannot capture an empty drawable");
        }

        std::vector<uint8_t> pixels(size_t(width) * height * 4), top(pixels.size());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        GLint previous = 0;
        glGetIntegerv(GL_READ_BUFFER, &previous);
        GLboolean double_buffer = GL_FALSE;
        glGetBooleanv(GL_DOUBLEBUFFER, &double_buffer);
        glReadBuffer(double_buffer ? GL_BACK : GL_FRONT);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glReadBuffer(previous);
        if (glGetError() != GL_NO_ERROR) {
            throw GuestFault("native drawable capture failed");
        }

        for (int row = 0; row < height; ++row) {
            std::memcpy(top.data() + size_t(row) * width * 4, pixels.data() + size_t(height - 1 - row) * width * 4,
                        size_t(width) * 4);
        }

        SDL_Surface* image =
            SDL_CreateRGBSurfaceWithFormatFrom(top.data(), width, height, 32, width * 4, SDL_PIXELFORMAT_RGBA32);
        if (!image) {
            throw GuestFault("cannot create native capture image");
        }

        int result = SDL_SaveBMP(image, path.c_str());
        SDL_FreeSurface(image);
        if (result) {
            throw GuestFault("cannot save native drawable capture");
        }

        std::fprintf(stderr, "FRAME_CAPTURE path=%s source=SDL_drawable_before_swap\n", path.c_str());
    }

    SDL_Window* window_;
    SDL_GLContext context_;
    std::vector<std::pair<std::string, bool>> captures_;
};
}

PresentBadge& present_badge() {
    static PresentBadge badge;
    return badge;
}

std::atomic<bool>& present_fps_counter() {
    static std::atomic<bool> on{false};
    return on;
}

std::unique_ptr<Renderer> make_renderer(SDL_Window* window) {
    return std::make_unique<GlRenderer>(window);
}
}
