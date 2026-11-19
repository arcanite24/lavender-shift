#version 460
// SR-7: one of the screen-space shadow mask's shaders, 93626E75D17576C5
// (three draws a frame, one per shadow map). Our code; it keeps the
// translation's interface.
//
// The game's shader rebuilds the view position from the camera depth, takes
// it into the shadow map with the matrix rows c8/c9/c10 (bias folded in),
// and averages four manual depth compares on a ring whose rotation is a
// wrapped ramp across the screen, then adds c24.w. This one keeps the
// reconstruction, the matrices, the lit-if-stored-is-not-nearer test, the
// clamps and the c24.w term, and replaces the ring with a 12-tap Poisson
// disc of the same radius (the mean length of the game's offsets, from
// c32/c33), rotated per host pixel by interleaved gradient noise, each tap
// a bilinear-weighted compare of four texels. TAPS 8 is the cheaper set.
//
// HOST_NOISE=1 takes the rotation from the SR-10 blue noise instead (needs
// vulkan_shader_override_host_inputs), one noise texel per guest pixel, so
// the pattern is the same size at every resolution scale. HOST_NOISE=2 is a
// check that writes the noise itself as the mask.
//
// CONTACT=1 adds contact shadows (SR-18): a short march from the receiver
// towards the light through the camera depth this shader already reads,
// for the small gaps the shadow map is too coarse to see (tyres on the
// road, kerbs, the car's underside). The light's direction in view space is
// the shadow map's depth axis, the xyz of the depth row c10. CONTACT_LENGTH
// is the march length in view units (metres), CONTACT_STEPS the samples,
// CONTACT_FAR the distance where it has faded out.

#ifndef HOST_NOISE
#define HOST_NOISE 0
#endif

#ifndef CONTACT
#define CONTACT 0
#endif
#ifndef CONTACT_LENGTH
#define CONTACT_LENGTH 0.35
#endif
#ifndef CONTACT_STEPS
#define CONTACT_STEPS 12
#endif
#ifndef CONTACT_FAR
#define CONTACT_FAR 40.0
#endif

#ifndef TAPS
#define TAPS 12
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// Packed: c0 c1 c8 c9 c10 c24 c25 c27 c32 c33 c254 c255.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[12];
} xe_float;
#define C0 xe_float.c[0]
#define C1 xe_float.c[1]
#define C8 xe_float.c[2]
#define C9 xe_float.c[3]
#define C10 xe_float.c[4]
#define C24 xe_float.c[5]
#define C25 xe_float.c[6]
#define C32 xe_float.c[8]
#define C33 xe_float.c[9]
#define C255 xe_float.c[11]

layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 words[48];
} xe_fetch;

layout(set = 3, binding = 0) uniform texture2DArray depth_texture;   // fetch 0
layout(set = 3, binding = 2) uniform texture2DArray shadow_texture;  // fetch 1
layout(set = 3, binding = 4) uniform sampler depth_sampler;
layout(set = 3, binding = 5) uniform sampler shadow_sampler;

#if HOST_NOISE
// SR-10 host inputs, set 0 bindings 5 and 6.
layout(set = 0, binding = 5) uniform sampler2D xe_host_blue_noise;
layout(set = 0, binding = 6, std140) uniform XeHostInputs {
  vec2 draw_resolution_scale;
  float blue_noise_texel;
  uint version;
} xe_host;
#endif

layout(location = 0) in vec4 xe_in_interpolator_0;  // clip position
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

const vec2 kDisc[12] = vec2[](vec2(0.3156, -0.4162), vec2(0.0004, 0.8267), vec2(0.7468, -0.0023),
                              vec2(0.4076, 0.3270), vec2(-0.7194, 0.2011), vec2(-0.0511, 0.0427),
                              vec2(-0.1867, -0.6643), vec2(-0.4108, 0.4946),
                              vec2(0.7458, -0.6100), vec2(-0.6915, -0.7011),
                              vec2(-0.7661, -0.2826), vec2(0.6036, 0.7305));

uint FetchWord(uint slot, uint word) {
  const uint index = slot * 6u + word;
  return xe_fetch.words[index >> 2u][index & 3u];
}
vec2 GuestSize(uint slot) {
  const uint word = FetchWord(slot, 2u);
  return vec2(float(bitfieldExtract(word, 0, 13) + 1u), float(bitfieldExtract(word, 13, 13) + 1u));
}
float SafeDivide(float a, float b) { return b == 0.0 ? 0.0 : a / b; }

// 1 where the stored depth is not nearer than the receiver, bilinearly
// weighted over the four texels around the tap.
float Lit(vec2 uv, float receiver, ivec2 size) {
  const vec2 position = uv * vec2(size) - 0.5;
  const ivec2 base = ivec2(floor(position));
  const vec2 f = position - vec2(base);
  vec4 lit;
  for (int i = 0; i < 4; ++i) {
    const ivec2 at = clamp(base + ivec2(i & 1, i >> 1), ivec2(0), size - 1);
    lit[i] = texelFetch(sampler2DArray(shadow_texture, shadow_sampler), ivec3(at, 0), 0).r >=
                     receiver
                 ? 1.0
                 : 0.0;
  }
  return mix(mix(lit.x, lit.y, f.x), mix(lit.z, lit.w, f.x), f.y);
}

float ViewDepth(float stored) { return SafeDivide(C25.y, (C255.z - C25.x) - stored); }

#if CONTACT
// 1 lit, 0 shadowed by something on screen between the receiver and the light.
float ContactLit(vec3 receiver_view, float jitter, ivec2 depth_size, vec2 depth_offset) {
  if (receiver_view.z <= 0.0 || receiver_view.z > CONTACT_FAR) return 1.0;
  const vec3 to_light = -normalize(C10.xyz);
  // Thicker at distance, where one depth texel covers more.
  const float thickness = 0.05 + 0.01 * receiver_view.z;
  float occlusion = 0.0;
  for (int i = 0; i < CONTACT_STEPS; ++i) {
    const float t = (float(i) + jitter) / float(CONTACT_STEPS);
    const vec3 q = receiver_view + to_light * (CONTACT_LENGTH * t + 0.02);
    if (q.z <= 0.0) break;
    const vec2 ndc = vec2(q.x / q.z * C0.x + C0.z, q.y / q.z * C1.y + C1.z);
    const vec2 uv = vec2(C255.y * ndc.x + C255.y, C255.x * ndc.y + C255.y) + depth_offset;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
    const ivec2 at = clamp(ivec2(floor(uv * vec2(depth_size))), ivec2(0), depth_size - 1);
    const float surface = ViewDepth(
        texelFetch(sampler2DArray(depth_texture, depth_sampler), ivec3(at, 0), 0).r);
    const float behind = q.z - surface;
    if (behind > 0.01 && behind < thickness) {
      occlusion = 1.0 - t * 0.5;  // nearer hits are darker
      break;
    }
  }
  const float fade = 1.0 - smoothstep(0.6 * CONTACT_FAR, CONTACT_FAR, receiver_view.z);
  return 1.0 - occlusion * fade;
}
#endif

void main() {
  const vec4 clip = xe_in_interpolator_0;
  const vec2 ndc = vec2(SafeDivide(clip.x, clip.w), SafeDivide(clip.y, clip.w));
  // The depth texel under the pixel (the game samples half a guest texel on).
  const vec2 depth_uv =
      vec2(C255.y * ndc.x + C255.y, C255.x * ndc.y + C255.y) + 0.5 / GuestSize(0u);
  const ivec2 depth_size = textureSize(sampler2DArray(depth_texture, depth_sampler), 0).xy;
  const ivec2 depth_at = clamp(ivec2(floor(depth_uv * vec2(depth_size))), ivec2(0), depth_size - 1);
  const float d = texelFetch(sampler2DArray(depth_texture, depth_sampler), ivec3(depth_at, 0), 0).r;

  const float z = ViewDepth(d);
  const vec4 view = vec4(z * SafeDivide(ndc.x - C0.z, C0.x), z * SafeDivide(ndc.y - C1.z, C1.y), z, 1.0);
  const vec2 st = clamp(vec2(dot(C8, view), dot(C9, view)), 0.0, 1.0);
  const float receiver = clamp(dot(C10.xyz, view.xyz) + C10.w, 0.0, 1.0);

  // The game's ring radius, in shadow-map UV.
  const float radius = 0.25 * (length(C32.xy) + length(C32.zw) + length(C33.xy) + length(C33.zw));
#if HOST_NOISE
  const vec2 guest_pixel = floor(gl_FragCoord.xy / max(xe_host.draw_resolution_scale, vec2(1.0)));
  const float noise = texture(xe_host_blue_noise, (guest_pixel + 0.5) * xe_host.blue_noise_texel).r;
#if HOST_NOISE == 2
  xe_out_fragment_data_0 = vec4(noise, 1.0, 0.0, 0.0) * xe_system.color_exp_bias.x;
  return;
#endif
  const float angle = 6.2831853 * noise;
#else
  const float angle =
      6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
#endif
  const mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
  const ivec2 shadow_size = textureSize(sampler2DArray(shadow_texture, shadow_sampler), 0).xy;
  float lit = 0.0;
  for (int i = 0; i < TAPS; ++i) {
    lit += Lit(st + rotation * kDisc[i] * radius, receiver, shadow_size);
  }
  float value = lit / float(TAPS);
#if CONTACT
  value = min(value, ContactLit(view.xyz, fract(angle * 0.1591549 + 0.5), depth_size,
                                0.5 / GuestSize(0u)));
#endif
  value += C24.w;
  xe_out_fragment_data_0 = vec4(value, 1.0, 0.0, 0.0) * xe_system.color_exp_bias.x;
}
