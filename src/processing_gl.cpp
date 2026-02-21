#include "processing_gl.hpp"

#include <spdlog/spdlog.h>
#include <fcntl.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <streambuf>
#include <cstring>

#ifndef GBM_BO_USE_LINEAR
#define GBM_BO_USE_LINEAR (1 << 4)
#endif

namespace {
const char* kVertexShader = R"GLSL(
attribute vec2 a_position;
attribute vec2 a_texcoord;
varying vec2 v_texcoord;
void main() {
    v_texcoord = a_texcoord;
    gl_Position = vec4(a_position, 0.0, 1.0);
}
)GLSL";

// Default fragment shader: NV12 -> RGB conversion and colortrans effect.
const char* kDefaultFragmentShader = R"GLSL(
precision mediump float;
varying vec2 v_texcoord;
uniform sampler2D texY;
uniform sampler2D texUV;

vec3 nv12_to_rgb(vec2 uv) {
    float y = texture2D(texY, uv).r;
    vec2 uv_s = texture2D(texUV, uv).rg - vec2(0.5, 0.5);
    float r = y + 1.402 * uv_s.y;
    float g = y - 0.344136 * uv_s.x - 0.714136 * uv_s.y;
    float b = y + 1.772 * uv_s.x;
    return vec3(r, g, b);
}

vec3 apply_effect(vec3 rgb) {
    return clamp((rgb + vec3(-0.15)) * 2.5, 0.0, 1.0);
}

void main() {
    vec3 rgb = nv12_to_rgb(v_texcoord);
    rgb = apply_effect(rgb);
    rgb = rgb.bgr;
    gl_FragColor = vec4(rgb, 1.0);
}
)GLSL";

const char* kDefaultFragmentShaderExternal = R"GLSL(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 v_texcoord;
uniform samplerExternalOES texExt;

vec3 apply_effect(vec3 rgb) {
    return clamp((rgb + vec3(-0.15)) * 2.5, 0.0, 1.0);
}

void main() {
    vec3 rgb = texture2D(texExt, v_texcoord).rgb;
    rgb = apply_effect(rgb);
    rgb = rgb.bgr;
    gl_FragColor = vec4(rgb, 1.0);
}
)GLSL";

std::string read_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string inject_define_after_version(const std::string& src, const char* define_line) {
    std::size_t pos = src.find("#version");
    if (pos == std::string::npos) {
        return std::string(define_line) + "\n" + src;
    }
    std::size_t eol = src.find('\n', pos);
    if (eol == std::string::npos) {
        return src + "\n" + define_line + "\n";
    }
    return src.substr(0, eol + 1) + define_line + "\n" + src.substr(eol + 1);
}

GLuint compile_shader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        spdlog::error("Shader compile failed: {}", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint link_program(GLuint vs, GLuint fs) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "a_position");
    glBindAttribLocation(program, 1, "a_texcoord");
    glLinkProgram(program);
    GLint status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[512];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        spdlog::error("Program link failed: {}", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}
} // namespace

GLProcessor::~GLProcessor() {
    deinit();
}

bool GLProcessor::ensure_functions() {
    eglGetPlatformDisplayEXT_ = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    eglCreateImageKHR_ = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    eglDestroyImageKHR_ = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    glEGLImageTargetTexture2DOES_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    eglQueryDmaBufFormatsEXT_ = (PFNEGLQUERYDMABUFFORMATSEXTPROC)eglGetProcAddress("eglQueryDmaBufFormatsEXT");
    eglQueryDmaBufModifiersEXT_ = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    eglCreateSyncKHR_ = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    eglDestroySyncKHR_ = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    eglClientWaitSyncKHR_ = (PFNEGLCLIENTWAITSYNCKHRPROC)eglGetProcAddress("eglClientWaitSyncKHR");
    if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ || !glEGLImageTargetTexture2DOES_) {
        spdlog::warn("Required EGL image functions not available.");
        return false;
    }
    return true;
}

bool GLProcessor::init(int drm_fd, uint32_t width, uint32_t height) {
    spdlog::info("GLProcessor init start (drm_fd={}, {}x{})", drm_fd, width, height);
    drm_fd_ = drm_fd;
    if (!ensure_functions()) {
        return false;
    }

    gbm_dev_ = gbm_create_device(drm_fd_);
    if (!gbm_dev_) {
        spdlog::warn("Failed to create gbm device");
        return false;
    }

    egl_display_ = eglGetPlatformDisplayEXT_
        ? eglGetPlatformDisplayEXT_(EGL_PLATFORM_GBM_KHR, gbm_dev_, nullptr)
        : eglGetDisplay((EGLNativeDisplayType)gbm_dev_);
    if (egl_display_ == EGL_NO_DISPLAY) {
        spdlog::warn("Failed to get EGL display");
        return false;
    }
    if (!eglInitialize(egl_display_, nullptr, nullptr)) {
        spdlog::warn("eglInitialize failed");
        return false;
    }
    const char* egl_ext = eglQueryString(egl_display_, EGL_EXTENSIONS);
    if (egl_ext) {
        spdlog::info("EGL extensions: {}", egl_ext);
    }
    log_dma_buf_support();
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        spdlog::warn("eglBindAPI(EGL_OPENGL_ES_API) failed");
        return false;
    }

    EGLint attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 0,
        EGL_NONE
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(egl_display_, attribs, &egl_config_, 1, &num_configs) || num_configs == 0) {
        spdlog::warn("eglChooseConfig failed, retrying with relaxed attributes");
        EGLint attribs_relaxed[] = {
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        num_configs = 0;
        if (!eglChooseConfig(egl_display_, attribs_relaxed, &egl_config_, 1, &num_configs) || num_configs == 0) {
            spdlog::warn("eglChooseConfig failed (relaxed)");
            return false;
        }
        if (egl_ext && std::strstr(egl_ext, "EGL_KHR_surfaceless_context")) {
            surfaceless_ = true;
            spdlog::info("Using surfaceless EGL context");
        } else {
            spdlog::warn("Surfaceless EGL not supported; cannot continue without PBUFFER config");
            return false;
        }
    }

    EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    egl_ctx_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT, ctx_attrs);
    if (egl_ctx_ == EGL_NO_CONTEXT) {
        spdlog::warn("eglCreateContext failed");
        return false;
    }

    if (!surfaceless_) {
        EGLint pbuf_attribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        egl_surface_ = eglCreatePbufferSurface(egl_display_, egl_config_, pbuf_attribs);
        if (egl_surface_ == EGL_NO_SURFACE) {
            spdlog::warn("Failed to create pbuffer surface");
            return false;
        }
    }

    if (!eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_ctx_)) {
        spdlog::warn("eglMakeCurrent failed");
        return false;
    }

    const char* gl_vendor = reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const char* gl_renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* gl_version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* gl_ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (gl_vendor && gl_renderer && gl_version) {
        spdlog::info("GL: vendor={}, renderer={}, version={}", gl_vendor, gl_renderer, gl_version);
    }
    if (gl_ext && std::strstr(gl_ext, "GL_OES_EGL_image_external")) {
        external_supported_ = true;
        spdlog::info("GL_OES_EGL_image_external supported");
    }

    if (!create_targets(width, height)) {
        spdlog::warn("Failed to create render target");
        return false;
    }

    if (!compile_program(kDefaultFragmentShader)) {
        return false;
    }
    if (external_supported_ && !compile_external_program(kDefaultFragmentShaderExternal)) {
        spdlog::warn("Failed to compile external texture shader; continuing without external path");
        external_supported_ = false;
    }

    ready_ = true;
    spdlog::info("GLProcessor initialized for {}x{}", width, height);
    return true;
}

bool GLProcessor::compile_program(const std::string& fragment_source) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source.c_str());
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    program_ = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program_) return false;

    y_sampler_loc_ = glGetUniformLocation(program_, "texY");
    uv_sampler_loc_ = glGetUniformLocation(program_, "texUV");

    return true;
}

bool GLProcessor::compile_external_program(const std::string& fragment_source) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, kVertexShader);
    if (!vs) return false;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment_source.c_str());
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    if (program_external_) {
        glDeleteProgram(program_external_);
        program_external_ = 0;
    }
    program_external_ = link_program(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!program_external_) return false;

    ext_sampler_loc_ = glGetUniformLocation(program_external_, "texExt");
    return true;
}

bool GLProcessor::reload_shader(const std::string& shader_path) {
    if (shader_path.empty()) {
        return true;
    }
    std::string src = read_file(shader_path);
    if (src.empty()) {
        spdlog::warn("Shader file empty: {}, keeping existing shader", shader_path);
        return true;
    }
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    custom_shader_loaded_ = true;
    custom_shader_source_ = src;
    bool ok = compile_program(src);
    if (external_supported_) {
        std::string ext_src = inject_define_after_version(src, "#define PP_EXTERNAL 1");
        if (compile_external_program(ext_src)) {
            external_uses_custom_ = true;
        } else {
            external_uses_custom_ = false;
            spdlog::warn("Custom shader failed for external NV12 path; using default external shader");
            compile_external_program(kDefaultFragmentShaderExternal);
        }
    }
    return ok;
}

bool GLProcessor::create_targets(uint32_t width, uint32_t height) {
    target_width_ = width;
    target_height_ = height;

    constexpr size_t kTargetCount = 2;
    targets_.clear();
    targets_.reserve(kTargetCount);
    for (size_t i = 0; i < kTargetCount; ++i) {
        RenderTarget t{};
        t.bo = gbm_bo_create(gbm_dev_, width, height, GBM_FORMAT_XRGB8888,
                             GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR | GBM_BO_USE_SCANOUT);
        if (!t.bo) {
            spdlog::warn("Failed to create GBM BO");
            destroy_targets();
            return false;
        }

        int bo_fd = gbm_bo_get_fd(t.bo);
        if (bo_fd < 0) {
            spdlog::warn("gbm_bo_get_fd failed");
            destroy_targets();
            return false;
        }

        EGLint attrs[] = {
            EGL_WIDTH, (EGLint)width,
            EGL_HEIGHT, (EGLint)height,
            EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888,
            EGL_DMA_BUF_PLANE0_FD_EXT, bo_fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)gbm_bo_get_stride(t.bo),
            EGL_NONE
        };
        t.image = eglCreateImageKHR_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
        if (t.image == EGL_NO_IMAGE_KHR) {
            spdlog::warn("Failed to create EGLImage for render target");
            close(bo_fd);
            destroy_targets();
            return false;
        }

        glGenTextures(1, &t.tex);
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, t.image);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            spdlog::warn("Framebuffer incomplete");
            close(bo_fd);
            destroy_targets();
            return false;
        }

        uint32_t handles[4] = {gbm_bo_get_handle(t.bo).u32, 0, 0, 0};
        uint32_t pitches[4] = {gbm_bo_get_stride(t.bo), 0, 0, 0};
        uint32_t offsets[4] = {0, 0, 0, 0};
        int ret = drmModeAddFB2(drm_fd_, width, height, DRM_FORMAT_XRGB8888,
                                handles, pitches, offsets, &t.fb_id, 0);
        if (ret) {
            spdlog::warn("drmModeAddFB2 failed for processed target");
            close(bo_fd);
            destroy_targets();
            return false;
        }
        close(bo_fd);
        targets_.push_back(t);
    }
    target_index_ = 0;
    return true;
}

bool GLProcessor::ensure_targets(uint32_t width, uint32_t height) {
    if (!targets_.empty() && width == target_width_ && height == target_height_) {
        return true;
    }
    destroy_targets();
    return create_targets(width, height);
}

void GLProcessor::destroy_targets() {
    for (auto& t : targets_) {
        if (t.fbo) {
            glDeleteFramebuffers(1, &t.fbo);
            t.fbo = 0;
        }
        if (t.tex) {
            glDeleteTextures(1, &t.tex);
            t.tex = 0;
        }
        if (t.image != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(egl_display_, t.image);
            t.image = EGL_NO_IMAGE_KHR;
        }
        if (t.fb_id) {
            drmModeRmFB(drm_fd_, t.fb_id);
            t.fb_id = 0;
        }
        if (t.bo) {
            gbm_bo_destroy(t.bo);
            t.bo = nullptr;
        }
    }
    targets_.clear();
}

void GLProcessor::deinit() {
    for (auto& entry : tex_cache_) {
        Nv12Tex& tex = entry.second;
        if (tex.tex_y) {
            glDeleteTextures(1, &tex.tex_y);
            tex.tex_y = 0;
        }
        if (tex.tex_uv) {
            glDeleteTextures(1, &tex.tex_uv);
            tex.tex_uv = 0;
        }
        if (tex.img_y != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(egl_display_, tex.img_y);
            tex.img_y = EGL_NO_IMAGE_KHR;
        }
        if (tex.img_uv != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(egl_display_, tex.img_uv);
            tex.img_uv = EGL_NO_IMAGE_KHR;
        }
        if (tex.tex_ext) {
            glDeleteTextures(1, &tex.tex_ext);
            tex.tex_ext = 0;
        }
        if (tex.img_ext != EGL_NO_IMAGE_KHR) {
            eglDestroyImageKHR_(egl_display_, tex.img_ext);
            tex.img_ext = EGL_NO_IMAGE_KHR;
        }
    }
    tex_cache_.clear();
    destroy_targets();
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    if (program_external_) {
        glDeleteProgram(program_external_);
        program_external_ = 0;
    }
    if (egl_ctx_ != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display_, egl_ctx_);
        egl_ctx_ = EGL_NO_CONTEXT;
    }
    if (egl_surface_ != EGL_NO_SURFACE && !surfaceless_) {
        eglDestroySurface(egl_display_, egl_surface_);
        egl_surface_ = EGL_NO_SURFACE;
    }
    if (egl_display_ != EGL_NO_DISPLAY) {
        eglTerminate(egl_display_);
        egl_display_ = EGL_NO_DISPLAY;
    }
    if (gbm_dev_) {
        gbm_device_destroy(gbm_dev_);
        gbm_dev_ = nullptr;
    }
    ready_ = false;
}

std::optional<EGLImageKHR> GLProcessor::import_plane(int fd, uint32_t width, uint32_t height,
                                                     uint32_t fourcc, uint32_t stride, uint32_t offset) {
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)fourcc,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };
    EGLImageKHR img = eglCreateImageKHR_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        return std::nullopt;
    }
    return img;
}

std::optional<EGLImageKHR> GLProcessor::import_nv12_external(int fd, uint32_t width, uint32_t height,
                                                             uint32_t stride, uint32_t offset_y, uint32_t offset_uv) {
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)width,
        EGL_HEIGHT, (EGLint)height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset_y,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)stride,
        EGL_DMA_BUF_PLANE1_FD_EXT, fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)offset_uv,
        EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)stride,
        EGL_NONE
    };
    EGLImageKHR img = eglCreateImageKHR_(egl_display_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        return std::nullopt;
    }
    return img;
}

void GLProcessor::log_dma_buf_support() {
    if (!eglQueryDmaBufFormatsEXT_) {
        spdlog::info("eglQueryDmaBufFormatsEXT not available");
        return;
    }
    EGLint count = 0;
    if (!eglQueryDmaBufFormatsEXT_(egl_display_, 0, nullptr, &count) || count <= 0) {
        spdlog::info("eglQueryDmaBufFormatsEXT returned no formats");
        return;
    }
    std::vector<EGLint> formats(count);
    if (!eglQueryDmaBufFormatsEXT_(egl_display_, count, formats.data(), &count)) {
        spdlog::info("eglQueryDmaBufFormatsEXT query failed");
        return;
    }
    auto has_format = [&](EGLint fourcc) {
        return std::find(formats.begin(), formats.end(), fourcc) != formats.end();
    };
    spdlog::info("EGL dmabuf formats: R8={}, GR88={}, NV12={}",
                 has_format(DRM_FORMAT_R8), has_format(DRM_FORMAT_GR88), has_format(DRM_FORMAT_NV12));

    if (!eglQueryDmaBufModifiersEXT_) {
        spdlog::info("eglQueryDmaBufModifiersEXT not available");
        return;
    }
    for (EGLint fmt : {EGLint(DRM_FORMAT_R8), EGLint(DRM_FORMAT_GR88), EGLint(DRM_FORMAT_NV12)}) {
        EGLint mod_count = 0;
        if (!eglQueryDmaBufModifiersEXT_(egl_display_, fmt, 0, nullptr, nullptr, &mod_count) || mod_count <= 0) {
            continue;
        }
        std::vector<EGLuint64KHR> mods(mod_count);
        std::vector<EGLBoolean> external_only(mod_count);
        if (!eglQueryDmaBufModifiersEXT_(egl_display_, fmt, mod_count, mods.data(), external_only.data(), &mod_count)) {
            continue;
        }
        bool has_linear = false;
        for (int i = 0; i < mod_count; ++i) {
            if (mods[i] == DRM_FORMAT_MOD_LINEAR) {
                has_linear = true;
                break;
            }
        }
        spdlog::info("EGL dmabuf modifiers fmt=0x{:08x} count={} linear={}", fmt, mod_count, has_linear);
    }
}

std::optional<ProcessedFrame> GLProcessor::process(MppFrame frame) {
    if (!ready_) return std::nullopt;

    uint32_t width = mpp_frame_get_width(frame);
    uint32_t height = mpp_frame_get_height(frame);
    uint32_t hor_stride = mpp_frame_get_hor_stride(frame);
    uint32_t ver_stride = mpp_frame_get_ver_stride(frame);
    MppFrameFormat fmt = mpp_frame_get_fmt(frame);
    if (fmt != MPP_FMT_YUV420SP) {
        spdlog::warn("GLProcessor only supports NV12 (YUV420SP) for now; passthrough.");
        return std::nullopt;
    }

    if (!ensure_targets(width, height)) {
        return std::nullopt;
    }

    MppBuffer buffer = mpp_frame_get_buffer(frame);
    if (!buffer) return std::nullopt;
    MppBufferInfo info;
    if (mpp_buffer_info_get(buffer, &info)) {
        return std::nullopt;
    }
    int fd = info.fd;
    uint32_t offset_y = 0;
    uint32_t offset_uv = hor_stride * ver_stride;

    auto it = tex_cache_.find(fd);
    bool needs_new = (it == tex_cache_.end());
    if (!needs_new) {
        const Nv12Tex& existing = it->second;
        if (existing.width != width || existing.height != height || existing.stride != hor_stride) {
            needs_new = true;
        }
    }
    if (needs_new) {
        if (it != tex_cache_.end()) {
            Nv12Tex& old = it->second;
            if (old.tex_y) glDeleteTextures(1, &old.tex_y);
            if (old.tex_uv) glDeleteTextures(1, &old.tex_uv);
            if (old.img_y != EGL_NO_IMAGE_KHR) eglDestroyImageKHR_(egl_display_, old.img_y);
            if (old.img_uv != EGL_NO_IMAGE_KHR) eglDestroyImageKHR_(egl_display_, old.img_uv);
            if (old.tex_ext) glDeleteTextures(1, &old.tex_ext);
            if (old.img_ext != EGL_NO_IMAGE_KHR) eglDestroyImageKHR_(egl_display_, old.img_ext);
            tex_cache_.erase(it);
        }
        auto img_y = import_plane(fd, width, height, DRM_FORMAT_R8, hor_stride, offset_y);
        auto img_uv = import_plane(fd, width / 2, height / 2, DRM_FORMAT_GR88, hor_stride, offset_uv);
        Nv12Tex tex{};
        tex.width = width;
        tex.height = height;
        tex.stride = hor_stride;
        if (img_y && img_uv) {
            tex.img_y = *img_y;
            tex.img_uv = *img_uv;
            glGenTextures(1, &tex.tex_y);
            glBindTexture(GL_TEXTURE_2D, tex.tex_y);
            glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, tex.img_y);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glGenTextures(1, &tex.tex_uv);
            glBindTexture(GL_TEXTURE_2D, tex.tex_uv);
            glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D, tex.img_uv);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        } else if (external_supported_) {
            if (img_y) eglDestroyImageKHR_(egl_display_, *img_y);
            if (img_uv) eglDestroyImageKHR_(egl_display_, *img_uv);
            auto img_ext = import_nv12_external(fd, width, height, hor_stride, offset_y, offset_uv);
            if (!img_ext) {
                spdlog::warn("Failed to import NV12 planes and external NV12");
                return std::nullopt;
            }
            tex.external = true;
            tex.img_ext = *img_ext;
            glGenTextures(1, &tex.tex_ext);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex.tex_ext);
            glEGLImageTargetTexture2DOES_(GL_TEXTURE_EXTERNAL_OES, tex.img_ext);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (custom_shader_loaded_ && !external_uses_custom_ && !warned_external_custom_) {
                spdlog::warn("External NV12 path is using the default shader (custom shader not compatible)");
                warned_external_custom_ = true;
            }
        } else {
            spdlog::warn("Failed to import NV12 planes");
            if (img_y) eglDestroyImageKHR_(egl_display_, *img_y);
            if (img_uv) eglDestroyImageKHR_(egl_display_, *img_uv);
            return std::nullopt;
        }

        tex_cache_.emplace(fd, tex);
    }

    Nv12Tex& tex = tex_cache_.at(fd);

    if (targets_.empty()) {
        return std::nullopt;
    }
    target_index_ = (target_index_ + 1) % targets_.size();
    RenderTarget& tgt = targets_[target_index_];
    glBindFramebuffer(GL_FRAMEBUFFER, tgt.fbo);
    glViewport(0, 0, width, height);
    if (tex.external) {
        glUseProgram(program_external_);
    } else {
        glUseProgram(program_);
    }

    GLfloat verts[] = {
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f
    };
    glVertexAttribPointer(pos_attrib_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts);
    glEnableVertexAttribArray(pos_attrib_);
    glVertexAttribPointer(texcoord_attrib_, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), verts + 2);
    glEnableVertexAttribArray(texcoord_attrib_);

    if (tex.external) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex.tex_ext);
        glUniform1i(ext_sampler_loc_, 0);
    } else {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex.tex_y);
        glUniform1i(y_sampler_loc_, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex.tex_uv);
        glUniform1i(uv_sampler_loc_, 1);
    }

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (eglCreateSyncKHR_ && eglClientWaitSyncKHR_ && eglDestroySyncKHR_) {
        EGLSyncKHR sync = eglCreateSyncKHR_(egl_display_, EGL_SYNC_FENCE_KHR, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            eglClientWaitSyncKHR_(egl_display_, sync, EGL_SYNC_FLUSH_COMMANDS_BIT_KHR, 100000000);
            eglDestroySyncKHR_(egl_display_, sync);
        } else {
            glFinish();
        }
    } else {
        glFinish();
    }

    ProcessedFrame pf;
    pf.bo = tgt.bo;
    pf.prime_fd = -1;
    pf.fb_id = tgt.fb_id;
    return pf;
}
