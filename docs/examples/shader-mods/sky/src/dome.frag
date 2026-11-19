#version 460
#extension GL_GOOGLE_include_directive : require
// SR-24: the sky dome, 2FD6A7CBEBD253C5. Our code; it keeps the
// translation's interface.
//
// What the game's dome does, from its constants (`mb`): a gradient from a
// horizon colour (c7, pale blue) to a zenith colour (c6, deep blue) by a
// height term (interpolator 1 x, saturated) to a small power (c1.x, 0.049, so
// the pale band hugs the horizon), then towards the sun's colour (c2.xyz) by
// c2.w times a tight sun lobe: the cosine to the sun (c4) to the power c1.y
// (48.7), dimmed by 1 - (1 - c5.w) times the cosine to a second direction
// (c5). Times c100.x, written as its square root plus a rectangular dither of
// c3.z (1/256) from a 128-texel noise tile in screen space (fetch 0).
//
// NOISE=0 keeps the noise tile. NOISE=1 is a triangular dither of the same
// step from interleaved gradient noise (Jimenez 2014) at two offsets: no
// texture, and the error is independent of the signal, so gradients do not
// band at any brightness. NOISE=2 drops the dither (for SCENE_FLOAT, where the
// sky reaches the composite in float and the composite dithers the output).

#ifndef NOISE
#define NOISE 0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// c1, c2, c3, c4, c5, c6, c7, c100, c255, packed in order.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[9];
} xe_float;
#define POWERS xe_float.c[0]      // c1: height power, sun power
#define SUN_COLOUR xe_float.c[1]  // c2: colour, weight
#define NOISE_SCALE xe_float.c[2] // c3: tile scale xy, step z
#define SUN xe_float.c[3].xyz     // c4
#define SECOND xe_float.c[4]      // c5: direction, w
#define ZENITH xe_float.c[5].xyz  // c6
#define HORIZON xe_float.c[6].xyz // c7
#define INTENSITY xe_float.c[7].x // c100

layout(set = 3, binding = 0) uniform texture2DArray noise_texture;  // fetch 0
layout(set = 3, binding = 2) uniform sampler noise_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;  // xyz view direction
layout(location = 1) in vec4 xe_in_interpolator_1;  // x height term
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

float Ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }

void main() {
  const vec3 direction = normalize(xe_in_interpolator_0.xyz);
  const float height = clamp(xe_in_interpolator_1.x, 0.0, 1.0);
  vec3 sky = mix(HORIZON, ZENITH, pow(height, POWERS.x));
  const float to_sun = clamp(dot(direction, SUN), 0.0, 1.0);
  const float dimming =
      clamp(1.0 - (1.0 - SECOND.w) * clamp(dot(direction, SECOND.xyz), 0.0, 1.0), 0.0, 1.0);
  const float lobe = pow(to_sun * dimming, POWERS.y);
  sky = clamp(mix(sky, SUN_COLOUR.xyz, SUN_COLOUR.w * lobe), 0.0, 1.0) * INTENSITY;

  vec3 encoded = sqrt(abs(sky));
#if NOISE == 0
  // The guest's pixel position (the host's divided by the resolution scale).
  const vec2 guest_pixel = abs(gl_FragCoord.xy);
  const float tile = texture(sampler2DArray(noise_texture, noise_sampler),
                             vec3(guest_pixel * NOISE_SCALE.xy, 0.0)).y;
  encoded += (tile - 0.5) * NOISE_SCALE.z;
#elif NOISE == 1
  const vec2 p = floor(gl_FragCoord.xy);
  encoded += (Ign(p) + Ign(p + vec2(47.0, 17.0)) - 1.0) * NOISE_SCALE.z;
#endif
  xe_out_fragment_data_0 = vec4(encoded, 1.0) * xe_system.color_exp_bias;
}
