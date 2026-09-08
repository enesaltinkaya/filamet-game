#version 460
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_ARB_shading_language_include : enable

layout(location = 0) in vec2 inUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    uint colorTextureIndex;
    uint bloomTextureIndex;
    float bloomStrength;
    uint pad[4];
};

#include "../../includes/utils.shader"
#include "../../includes/globalset.shader"

/* This pass composites the input for the FFX LPM tone/gamut mapper:
 * scene HDR + bloom + exposure, still LINEAR. The LPM pass (lpm) applies
 * the tone curve + display gamma and writes the final 8-bit image, which
 * is blitted into the swapchain (or the lens input). The custom
 * tonemapping curves (AgX/ACES/...) that used to run here were replaced
 * by LPM. */

vec3 sampleSceneHdr(vec2 uv) {
    vec3 hdr = texture(sampler2D(textures[nonuniformEXT(colorTextureIndex)],
                                 samplers[SAMPLER_CLAMP_LINEAR]),
                       uv)
                   .rgb;

    if (bloomTextureIndex != 0u) {
        vec3 bloom = texture(sampler2D(textures[nonuniformEXT(bloomTextureIndex)],
                                       samplers[SAMPLER_CLAMP_LINEAR]),
                             uv)
                         .rgb;
        hdr += bloom * bloomStrength;
    }

    return hdr * sceneBuffer.cameras[0].exposure;
}

void main() {
    vec2 uv = vec2(inUV.x, 1.0 - inUV.y);

    vec3 hdr = sampleSceneHdr(uv);

    /* R16F attachment: store the linear HDR composite as-is — LPM does
     * the tone curve + gamma. */
    outColor = vec4(hdr, 1.0);
}