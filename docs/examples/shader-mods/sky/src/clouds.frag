#version 460
#extension GL_GOOGLE_include_directive : require
// SR-24: the cloud sheets, 90CAB86BE8159DA8. Our code; it keeps the
// translation's interface.
//
// What the game's layer does, from its constants (`mb`): two cloud sheets,
// a front one over a back one, composited premultiplied and written
// un-premultiplied (colour as its square root, and alpha).
//  - Each sheet's density is the average of two taps of the 1024 texture
//    (fetch 0; the front at interpolator 0 xy and zw, the back at 2 xy and
//    zw, two scrolling copies), less a threshold (c1.x, c5.x), saturated;
//    its transmittance is c1.y (c5.y) to the power 255 times that, so each
//    8-bit step of density above the threshold takes off a fixed fraction.
//  - The front sheet's opacity is 1 - transmittance, times a coverage map
//    (the 128 texture, fetch 1, at interpolator 1 xy), times a term from
//    c4.x to c4.y by a sun glow (interpolator 1 z to the power c4.z, 81.5);
//    its colour goes from c3 to c2 with 1 - transmittance times coverage.
//  - The back sheet's opacity is 1 - transmittance times the coverage map
//    at interpolator 3 xy, times a term from c8.x to c8.y by interpolator 3
//    z to the power c8.z; its colour goes from c7 to c6 with that opacity
//    before the c8 term.
//
// CUBIC=1 samples both textures with a bicubic B-spline instead of bilinear:
// the 128 coverage map is stretched over a third of the screen, so bilinear
// shows its texel diamonds.

#ifndef CUBIC
#define CUBIC 0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// c1, c2, c3, c4, c5, c6, c7, c8, c255, packed in order.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[9];
} xe_float;
#define FRONT_DENSITY xe_float.c[0]   // c1: threshold, transmittance per step
#define FRONT_LIT xe_float.c[1].xyz   // c2
#define FRONT_DARK xe_float.c[2].xyz  // c3
#define FRONT_GLOW xe_float.c[3]      // c4: from, to, power
#define BACK_DENSITY xe_float.c[4]    // c5: threshold, transmittance per step
#define BACK_FAR xe_float.c[5].xyz    // c6
#define BACK_NEAR xe_float.c[6].xyz   // c7
#define BACK_SHAPE xe_float.c[7]      // c8: from, to, power

// Bindings in the order the translation has them: texture 1 first.
layout(set = 3, binding = 0) uniform texture2DArray coverage_texture;  // fetch 1
layout(set = 3, binding = 2) uniform texture2DArray density_texture;   // fetch 0
layout(set = 3, binding = 4) uniform sampler coverage_sampler;
layout(set = 3, binding = 5) uniform sampler density_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 1) in vec4 xe_in_interpolator_1;
layout(location = 2) in vec4 xe_in_interpolator_2;
layout(location = 3) in vec4 xe_in_interpolator_3;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

#include "cubic.h"

float Power(float x, float p) { return exp2(p * log2(abs(x))); }

float Transmittance(vec2 a, vec2 b, vec4 density) {
  const float average = 0.5 * (SAMPLE_LAYER(density_texture, density_sampler, a).x +
                               SAMPLE_LAYER(density_texture, density_sampler, b).x);
  return Power(density.y, 255.0 * clamp(average - density.x, 0.0, 1.0));
}

void main() {
  const float front_clear =
      Transmittance(xe_in_interpolator_0.xy, xe_in_interpolator_0.zw, FRONT_DENSITY);
  const float back_clear =
      Transmittance(xe_in_interpolator_2.xy, xe_in_interpolator_2.zw, BACK_DENSITY);

  const float coverage =
      SAMPLE_LAYER(coverage_texture, coverage_sampler, xe_in_interpolator_1.xy).x;
  const float front_cloud = (1.0 - front_clear) * coverage;
  const float glow = Power(xe_in_interpolator_1.z, FRONT_GLOW.z);
  const float front_alpha = front_cloud * mix(FRONT_GLOW.x, FRONT_GLOW.y, glow);
  const vec3 front_colour = mix(FRONT_DARK, FRONT_LIT, front_cloud);

  const float back_coverage =
      SAMPLE_LAYER(coverage_texture, coverage_sampler, xe_in_interpolator_3.xy).x;
  const float back_cloud = (1.0 - back_clear) * back_coverage;
  const float back_fade = Power(xe_in_interpolator_3.z, BACK_SHAPE.z);
  const float back_alpha = back_cloud * mix(BACK_SHAPE.x, BACK_SHAPE.y, back_fade);
  const vec3 back_colour = mix(BACK_NEAR, BACK_FAR, back_cloud);

  // Front over back, premultiplied, then un-premultiplied for the blend.
  const float alpha = front_alpha + back_alpha * (1.0 - front_alpha);
  const vec3 premultiplied =
      front_alpha * front_colour + (1.0 - front_alpha) * back_alpha * back_colour;
  const vec3 colour = alpha > 0.0 ? premultiplied / alpha : vec3(0.0);
  xe_out_fragment_data_0 = vec4(sqrt(abs(colour)), alpha) * xe_system.color_exp_bias;
}
