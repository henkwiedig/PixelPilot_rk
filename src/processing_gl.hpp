#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <rockchip/rk_mpi.h>
#include <drm_fourcc.h>
#include <xf86drmMode.h>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct ProcessedFrame {
    uint32_t fb_id{0};
    int prime_fd{-1};
    gbm_bo* bo{nullptr};
};

// Minimal GBM/EGL/GLES2 renderer that takes NV12 dmabuf frames, applies a simple
// shader, and renders into a single XRGB8888 GBM BO which is exported as a DRM FB.
// Designed to fail fast and fall back to passthrough if any step is unavailable.
class GLProcessor {
public:
    GLProcessor() = default;
    ~GLProcessor();

    bool init(int drm_fd, uint32_t width, uint32_t height);
    bool reload_shader(const std::string& shader_path);
    void deinit();

    // Renders the incoming frame; returns fb id on success.
    std::optional<ProcessedFrame> process(MppFrame frame);

    bool is_ready() const { return ready_; }
    uint32_t target_width() const { return target_width_; }
    uint32_t target_height() const { return target_height_; }

private:
    bool ensure_targets(uint32_t width, uint32_t height);
    bool create_targets(uint32_t width, uint32_t height);
    void destroy_targets();
    bool compile_program(const std::string& fragment_source);
    bool compile_external_program(const std::string& fragment_source);
    std::optional<EGLImageKHR> import_plane(int fd, uint32_t width, uint32_t height,
                                            uint32_t fourcc, uint32_t stride, uint32_t offset);
    std::optional<EGLImageKHR> import_nv12_external(int fd, uint32_t width, uint32_t height,
                                                    uint32_t stride, uint32_t offset_y, uint32_t offset_uv);
    bool ensure_functions();
    void log_dma_buf_support();

    int drm_fd_{-1};
    uint32_t target_width_{0};
    uint32_t target_height_{0};
    bool ready_{false};

    gbm_device* gbm_dev_{nullptr};
    EGLDisplay egl_display_{EGL_NO_DISPLAY};
    EGLContext egl_ctx_{EGL_NO_CONTEXT};
    EGLSurface egl_surface_{EGL_NO_SURFACE};
    EGLConfig egl_config_{nullptr};
    struct RenderTarget {
        gbm_bo* bo{nullptr};
        EGLImageKHR image{EGL_NO_IMAGE_KHR};
        GLuint tex{0};
        GLuint fbo{0};
        uint32_t fb_id{0};
    };
    std::vector<RenderTarget> targets_;
    size_t target_index_{0};
    bool surfaceless_{false};

    GLuint program_{0};
    GLuint program_external_{0};
    GLuint pos_attrib_{0};
    GLuint texcoord_attrib_{1};
    GLint y_sampler_loc_{-1};
    GLint uv_sampler_loc_{-1};
    GLint ext_sampler_loc_{-1};
    bool external_supported_{false};
    bool custom_shader_loaded_{false};
    bool warned_external_custom_{false};
    bool external_uses_custom_{false};
    std::string custom_shader_source_{};

    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};
    PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT_{nullptr};
    PFNEGLQUERYDMABUFFORMATSEXTPROC eglQueryDmaBufFormatsEXT_{nullptr};
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT_{nullptr};
    PFNEGLCREATESYNCKHRPROC eglCreateSyncKHR_{nullptr};
    PFNEGLDESTROYSYNCKHRPROC eglDestroySyncKHR_{nullptr};
    PFNEGLCLIENTWAITSYNCKHRPROC eglClientWaitSyncKHR_{nullptr};

    struct Nv12Tex {
        EGLImageKHR img_y{EGL_NO_IMAGE_KHR};
        EGLImageKHR img_uv{EGL_NO_IMAGE_KHR};
        GLuint tex_y{0};
        GLuint tex_uv{0};
        EGLImageKHR img_ext{EGL_NO_IMAGE_KHR};
        GLuint tex_ext{0};
        bool external{false};
        uint32_t width{0};
        uint32_t height{0};
        uint32_t stride{0};
    };
    std::unordered_map<int, Nv12Tex> tex_cache_;
};
