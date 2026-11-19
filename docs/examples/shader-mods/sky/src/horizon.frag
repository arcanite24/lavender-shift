#version 460
// SR-24: the sky's horizon band, EE3FFB69590792A2. Our code; it keeps the
// translation's interface (system and float constants, interpolator 0, one
// output).
//
// What the game's band does, from its constants: interpolator 0 is the view
// direction (xyz) and a height (w). The colour runs from c40 (looking away
// from the sun) to c41 (towards it), weighted by the cosine to the sun
// direction (c11) to the power c42.w, times c100.x, and is written as its
// square root. Its alpha falls from c42.z to 0 as the height goes from c42.x
// to c42.y, along a smoothstep. In `mb` (late sun): c40 a dark grey-blue, c41
// orange, exponent 5, heights 0 to 1568.
//
// PHASE=0 is that model. PHASE=1 replaces the cosine power, which is cut to 0
// at 90 degrees from the sun, with a Henyey-Greenstein lobe (forward
// scattering by haze) normalised to 1 towards the sun, with its asymmetry g
// chosen so the lobe falls to half at the same angle as the game's: it is
// near the game's around the sun and leaves a faint glow on the far side.

#ifndef PHASE
#define PHASE 0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// The registers the game's shader uses, packed in order.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[6];
} xe_float;
#define SUN_DIRECTION xe_float.c[0].xyz  // c11
#define AWAY_COLOUR xe_float.c[1].xyz    // c40
#define TOWARDS_COLOUR xe_float.c[2].xyz // c41
#define BAND xe_float.c[3]               // c42: bottom, top, alpha, exponent
#define INTENSITY xe_float.c[4].x        // c100

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

float SunWeight(float cosine, float exponent) {
#if PHASE == 1
  // Half maximum of the game's lobe: cosine^exponent = 1/2.
  const float half_cosine = exp2(-1.0 / max(exponent, 1e-3));
  // HG(c) / HG(1) = ((1 - g)^2 / (1 + g^2 - 2gc))^(3/2) = 1/2 at the same c:
  // (q - 1)g^2 - 2(q - c)g + (q - 1) = 0 with q = 2^(2/3), smaller root.
  const float q = 1.5874011;
  const float b = q - half_cosine;
  const float g = (b - sqrt(max(b * b - (q - 1.0) * (q - 1.0), 0.0))) / (q - 1.0);
  const float ratio = (1.0 - g) * (1.0 - g) / max(1.0 + g * g - 2.0 * g * cosine, 1e-6);
  return ratio * sqrt(ratio);
#else
  return pow(clamp(cosine, 0.0, 1.0), exponent);
#endif
}

void main() {
  const vec3 direction = xe_in_interpolator_0.xyz;
  const float cosine = dot(direction, SUN_DIRECTION);
  const vec3 colour =
      mix(AWAY_COLOUR, TOWARDS_COLOUR, SunWeight(cosine, BAND.w)) * INTENSITY;

  const float t = clamp((xe_in_interpolator_0.w - BAND.x) / (BAND.y - BAND.x), 0.0, 1.0);
  const float alpha = (1.0 - t * t * (3.0 - 2.0 * t)) * BAND.z;

  xe_out_fragment_data_0 = vec4(sqrt(abs(colour)), alpha) * xe_system.color_exp_bias;
}
