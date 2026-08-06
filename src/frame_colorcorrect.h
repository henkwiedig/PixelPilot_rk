#pragma once
/**
 * frame_colorcorrect.h
 *
 * GPU-accelerated color correction for NV12 video frames.
 *
 * Applies the same formula as the DRM gamma LUT:
 *   y = clamp((x + offset) * gain, 0, 1)  (in RGB space)
 *
 * Pipeline per frame:
 *   1. Import src NV12 DMA-buf as GL_TEXTURE_EXTERNAL_OES.
 *      The driver converts YCbCr → RGB implicitly during sampling.
 *   2. Render fullscreen quad with correction shader into RGBA GBM BO target.
 *   3. RGA converts RGBA GBM BO → NV12 into the destination DMA-buf.
 *      The driver converts RGB → YCbCr implicitly.
 *
 * Intended to run on the __ENCPACER thread (init() must be called from
 * the same thread that will call process()).
 */

#include <cstdint>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

class FrameColorCorrect {
public:
    FrameColorCorrect() = default;
    ~FrameColorCorrect();

    // Init EGL context, compile shader, create RGBA render targets at OUTPUT size.
    // Must be called from the thread that will call process().
    // gain/offset apply the DRM gamma formula y = clamp((x+offset)*gain, 0,1);
    // pass gain=1 offset=0 when only OSD blend is needed.
    bool init(int drm_fd, uint32_t out_w, uint32_t out_h,
              float gain, float offset);

    void deinit();
    bool ready() const { return ready_; }

    // Update the correction parameters of an already-initialised context.
    // The uniforms are uploaded per frame in process(), so this takes effect
    // on the next frame.  Must be called from the processor thread.
    void set_params(float gain, float offset) { gain_ = gain; offset_ = offset; }

    // Register the current OSD DMA-buf for compositing.  Called whenever the
    // OSD double-buffer switches.  The EGLImage is cached and only re-imported
    // when prime_fd changes.  Must be called from the processor thread.
    void set_osd(int prime_fd, uint32_t w, uint32_t h, uint32_t stride_px);
    void clear_osd();  // disable OSD compositing

    // Apply color-correction + OSD blend: read src NV12 DMA-buf, render into
    // the RGBA GBM BO at output size (resizing via texture sampling if needed),
    // then RGA-CSC to the NV12 dst DMA-buf (no resize — sizes always match).
    // Returns false on failure — caller should fall back to plain copy.
    bool process(int src_fd, uint32_t src_w, uint32_t src_h,
                 uint32_t src_hs, uint32_t src_vs,
                 int dst_fd, uint32_t dst_hs, uint32_t dst_vs);

private:
    bool build_shader();
    bool create_targets();
    void destroy_targets();
    bool ensure_functions();

    int      drm_fd_{-1};
    uint32_t width_{0}, height_{0};
    float    gain_{1.f}, offset_{0.f};
    bool     ready_{false};

    gbm_device*  gbm_{nullptr};
    EGLDisplay   dpy_{EGL_NO_DISPLAY};
    EGLContext   ctx_{EGL_NO_CONTEXT};
    EGLSurface   surf_{EGL_NO_SURFACE};
    EGLConfig    cfg_{nullptr};

    GLuint prog_{0};
    GLint  loc_tex_{-1};
    GLint  loc_gain_{-1};
    GLint  loc_offset_{-1};
    GLint  loc_osd_tex_{-1};
    GLint  loc_has_osd_{-1};

    // OSD compositing state (processor-thread only)
    GLuint      osd_tex_gl_{0};           // 1×1 transparent fallback or live OSD
    int         osd_prime_fd_{-1};        // DMA-buf fd of the current OSD buffer
    EGLImageKHR osd_img_{EGL_NO_IMAGE_KHR};
    uint32_t    osd_w_{0}, osd_h_{0}, osd_stride_px_{0};

    // Double-buffered RGBA render targets
    static constexpr int kTargets = 2;
    struct Target {
        gbm_bo*     bo{nullptr};
        uint32_t    gem_handle{0};  // set instead of bo on the CONTIG (raw
                                     // dumb-buffer) allocation path -- see
                                     // create_targets() in the .cpp
        EGLImageKHR img{EGL_NO_IMAGE_KHR};
        GLuint      tex{0};
        GLuint      fbo{0};
        int         prime_fd{-1};  // DMA-buf fd kept open for RGA
        uint32_t    stride_px{0};  // RGBA row stride in pixels
    } targets_[kTargets];
    int target_idx_{0};

    PFNEGLCREATEIMAGEKHRPROC            eglCreateImageKHR_{nullptr};
    PFNEGLDESTROYIMAGEKHRPROC           eglDestroyImageKHR_{nullptr};
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_{nullptr};
    PFNEGLGETPLATFORMDISPLAYEXTPROC     eglGetPlatformDisplayEXT_{nullptr};
};
