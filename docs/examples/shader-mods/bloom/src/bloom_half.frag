#version 460
#extension GL_GOOGLE_include_directive : require
// SR-2: the bloom pyramid's half steps, D6652B8E4866F16B (AXIS 0, halves x)
// and 0732AB80912AE1A0 (AXIS 1, halves y).
//
// The game's pass is a symmetric 7-tap filter whose four weights are the
// float constant c32 (centre, +-1, +-2, +-3 guest texels), each tap falling
// on the edge between two texels, so each reads the average of a pair of
// sqrt-encoded texels. This one keeps that pair average (squaring an
// encoded average loses some energy on contrasty input, and the game's
// bloom strength is tuned to that loss; a linear-light kernel came out 25%
// brighter in a model of spiky input), its gain (the sum of the weights)
// and its spread (their second moment), and spends them on a Gaussian over
// every texel edge in the same footprint instead of seven. Above 1x the
// game's taps skip host texels and this does not. Texels are read exactly,
// so the guest sampler's filter mode does not matter.
#include "bloom_common.glsl"

#ifndef AXIS
#define AXIS 0
#endif

layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[1];
} xe_float;

layout(set = 3, binding = 0) uniform texture2DArray source_texture;
layout(set = 3, binding = 2) uniform sampler source_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

void main() {
  const vec4 w = xe_float.c[0];
  const float gain = w.x + 2.0 * (w.y + w.z + w.w);
  if (!(gain > 1e-6)) {
    xe_out_fragment_data_0 = Finish(vec4(0.0, 0.0, 0.0, 1.0));
    return;
  }
  const float variance = 2.0 * (w.y + 4.0 * w.z + 9.0 * w.w) / gain;
  const float inv_two_variance = 0.5 / max(variance, 0.05);

  const ivec2 host_size = textureSize(sampler2DArray(source_texture, source_sampler), 0).xy;
  const vec2 guest_size = GuestSize(0u);
  const float host_per_guest = float(host_size[AXIS]) / guest_size[AXIS];
  // The output centre in host texels of the source; it falls on an edge.
  const vec2 centre = xe_in_interpolator_0.xy * vec2(host_size);
  const float radius = 3.0 * host_per_guest;
  const int first = int(floor(centre[AXIS] - radius));
  const int last = int(ceil(centre[AXIS] + radius));
  ivec2 texel = clamp(ivec2(floor(centre)), ivec2(0), host_size - 1);
  const float scale = ExpScale(0u);

  vec3 sum = vec3(0.0);
  float weight_sum = 0.0;
  // Edge e lies between host texels e - 1 and e.
  for (int e = first; e <= last; ++e) {
    const float d = (float(e) - centre[AXIS]) / host_per_guest;
    if (abs(d) > 3.0001) continue;
    const float weight = exp(-d * d * inv_two_variance);
    ivec2 at0 = texel, at1 = texel;
    at0[AXIS] = clamp(e - 1, 0, host_size[AXIS] - 1);
    at1[AXIS] = clamp(e, 0, host_size[AXIS] - 1);
    const vec3 pair =
        0.5 * scale *
        (texelFetch(sampler2DArray(source_texture, source_sampler), ivec3(at0, 0), 0).rgb +
         texelFetch(sampler2DArray(source_texture, source_sampler), ivec3(at1, 0), 0).rgb);
    sum += weight * pair * pair;
    weight_sum += weight;
  }
  const vec3 linear = sum * (gain / max(weight_sum, 1e-6));
  xe_out_fragment_data_0 = Finish(vec4(sqrt(max(linear, vec3(0.0))), 1.0));
}
