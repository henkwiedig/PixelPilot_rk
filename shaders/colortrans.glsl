#version 100
#ifdef PP_EXTERNAL
#extension GL_OES_EGL_image_external : require
#endif
precision mediump float;

varying vec2 v_texcoord;

#ifdef PP_EXTERNAL
uniform samplerExternalOES texExt;

vec3 sample_rgb(vec2 uv) {
    return texture2D(texExt, uv).rgb;
}
#else
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

vec3 sample_rgb(vec2 uv) {
    return nv12_to_rgb(uv);
}
#endif

void main() {
    vec3 rgb = sample_rgb(v_texcoord);
    // Stronger curve to match default shader output.
    rgb = clamp((rgb + vec3(-0.15)) * 2.5, 0.0, 1.0);
    rgb = rgb.bgr;
    gl_FragColor = vec4(rgb, 1.0);
}
