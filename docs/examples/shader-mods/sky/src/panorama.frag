#version 460
#extension GL_GOOGLE_include_directive : require
// SR-24: the cloud panorama, FD9976C9E5DB9BB0. Our code; it keeps the
// translation's interface.
//
// What the game's layer does, from its constants (`mb`): three textures at
// one coordinate (interpolator 0 xy). Fetch 2 (2048x512) lights the clouds,
// from c3 (shadowed) to c4 (lit) by its y; fetch 1 carries two scattering
// amounts, and fetch 0 the coverage (x) and how much of the sun glow the
// cloud blocks (y). Three glows of a sun term (interpolator 1 x) are added:
// c7 times the term to the power c7.w (29.9) where the cloud lets it through
// (1 - fetch 0 y), c8 times the term to the power c8.w (11.2) times fetch 1
// x to the power c2.z, and c9 likewise with c9.w and fetch 1 y to the power
// c2.w. Fetches 0 and 1 are scaled by c2.x first. Written as its square
// root; alpha is the coverage times c1.x to c1.y by a height term
// (interpolator 1 y) to the power c1.z.
//
// CUBIC=1 samples the three textures with a bicubic B-spline: the panorama
// wraps 2048 texels round the horizon, so a 90-degree view at 1440p shows
// each texel about five pixels wide.

#ifndef CUBIC
#define CUBIC 0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// c1, c2, c3, c4, c7, c8, c9, c255, packed in order.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[8];
} xe_float;
#define FADE xe_float.c[0]          // c1: from, to, power
#define SCALES xe_float.c[1]        // c2: texture scale x, powers zw
#define SHADOWED xe_float.c[2].xyz  // c3
#define LIT xe_float.c[3].xyz       // c4
#define GLOW_THROUGH xe_float.c[4]  // c7: colour, power
#define GLOW_FIRST xe_float.c[5]    // c8: colour, power
#define GLOW_SECOND xe_float.c[6]   // c9: colour, power

// Bindings in the order the translation has them: texture 2 first.
layout(set = 3, binding = 0) uniform texture2DArray light_texture;    // fetch 2
layout(set = 3, binding = 2) uniform texture2DArray scatter_texture;  // fetch 1
layout(set = 3, binding = 4) uniform texture2DArray cover_texture;    // fetch 0
layout(set = 3, binding = 6) uniform sampler light_sampler;
layout(set = 3, binding = 7) uniform sampler scatter_sampler;
layout(set = 3, binding = 8) uniform sampler cover_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 1) in vec4 xe_in_interpolator_1;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

#include "cubic.h"

float Power(float x, float p) { return exp2(p * log2(abs(x))); }

void main() {
  const vec2 uv = xe_in_interpolator_0.xy;
  const float light = SAMPLE_LAYER(light_texture, light_sampler, uv).y;
  const vec2 scatter = SAMPLE_LAYER(scatter_texture, scatter_sampler, uv).xy * SCALES.x;
  const vec2 cover = SAMPLE_LAYER(cover_texture, cover_sampler, uv).xy * SCALES.x;
  const float sun = xe_in_interpolator_1.x;

  vec3 colour = mix(SHADOWED, LIT, light);
  colour += GLOW_THROUGH.xyz * Power(sun, GLOW_THROUGH.w) * (1.0 - cover.y);
  colour += GLOW_FIRST.xyz * Power(sun, GLOW_FIRST.w) * Power(scatter.x, SCALES.z);
  colour += GLOW_SECOND.xyz * Power(sun, GLOW_SECOND.w) * Power(scatter.y, SCALES.w);
  const float alpha = cover.x * mix(FADE.x, FADE.y, Power(xe_in_interpolator_1.y, FADE.z));
  xe_out_fragment_data_0 = vec4(sqrt(abs(colour)), alpha) * xe_system.color_exp_bias;
}
