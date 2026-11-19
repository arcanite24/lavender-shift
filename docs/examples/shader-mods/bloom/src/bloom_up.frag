#version 460
#extension GL_GOOGLE_include_directive : require
// SR-2: the bloom pyramid's upsample, 0F5712A5F9D13E62.
//
// The game's pass adds the finer level (texture 0, weight c32.z) to one
// tap of the coarser level (texture 1, weight c33.z), in linear light, each
// texture's UV scaled by c32.xy and c33.xy to its content rectangle. This
// one keeps the weights and the rectangles and reads the coarse level
// through a 3x3 tent of bilinear samples one coarse texel apart (a 4x4
// separable kernel), clamped to the content rectangle, so the coarse level
// no longer shows its texel grid.
#include "bloom_common.glsl"

layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[2];
} xe_float;

// Bindings in the order the translation has them: texture 1 first.
layout(set = 3, binding = 0) uniform texture2DArray coarse_texture;
layout(set = 3, binding = 2) uniform texture2DArray fine_texture;
layout(set = 3, binding = 4) uniform sampler coarse_sampler;
layout(set = 3, binding = 5) uniform sampler fine_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

// Weights of texels base-1 .. base+2 for a tent of three linear samples at
// -1, 0, +1 texels, f being the position's fraction past texel base.
vec4 TentWeights(float f) {
  return vec4(0.25 * (1.0 - f), 0.25 * f + 0.5 * (1.0 - f), 0.5 * f + 0.25 * (1.0 - f),
              0.25 * f);
}

vec3 Linear(texture2DArray t, sampler s, ivec2 at, float scale) {
  const vec3 v = texelFetch(sampler2DArray(t, s), ivec3(at, 0), 0).rgb * scale;
  return v * v;
}

void main() {
  const vec4 fine_c = xe_float.c[0];
  const vec4 coarse_c = xe_float.c[1];
  const vec2 uv = xe_in_interpolator_0.xy;

  // The finer level matches the output: one texel.
  const ivec2 fine_size = textureSize(sampler2DArray(fine_texture, fine_sampler), 0).xy;
  const ivec2 fine_at =
      clamp(ivec2(floor(uv * fine_c.xy * vec2(fine_size))), ivec2(0), fine_size - 1);
  const vec3 fine = Linear(fine_texture, fine_sampler, fine_at, ExpScale(0u));

  // The coarser level: a tent, kept inside its content rectangle.
  const ivec2 coarse_size = textureSize(sampler2DArray(coarse_texture, coarse_sampler), 0).xy;
  const vec2 position = uv * coarse_c.xy * vec2(coarse_size) - 0.5;
  const vec2 base = floor(position);
  const vec2 f = position - base;
  const vec4 wx = TentWeights(f.x);
  const vec4 wy = TentWeights(f.y);
  const ivec2 content_max =
      clamp(ivec2(ceil(coarse_c.xy * vec2(coarse_size))) - 1, ivec2(0), coarse_size - 1);
  const float coarse_scale = ExpScale(1u);
  vec3 coarse = vec3(0.0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      const ivec2 at = clamp(ivec2(base) + ivec2(x - 1, y - 1), ivec2(0), content_max);
      coarse += wx[x] * wy[y] * Linear(coarse_texture, coarse_sampler, at, coarse_scale);
    }
  }

  const vec3 linear = fine_c.z * fine + coarse_c.z * coarse;
  xe_out_fragment_data_0 = Finish(vec4(sqrt(max(linear, vec3(0.0))), 1.0));
}
