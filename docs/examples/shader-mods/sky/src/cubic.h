// Bicubic B-spline filtering from four bilinear taps (Sigg and Hadwiger,
// GPU Gems 2 ch. 20): the weights of each pair of texels on an axis are
// folded into one bilinear tap between them. Smooth (C2) and never rings,
// at the cost of a slight blur; for textures magnified several times on
// screen, where bilinear shows its diamond pattern.
//
// SAMPLE_LAYER(texture, sampler, uv) is a macro because Vulkan GLSL takes a
// combined sampler only where it is built, in the texture call.

struct CubicTaps {
  vec2 p0, p1;  // the two bilinear positions on each axis
  vec2 g0, g1;  // their weights
};

CubicTaps Cubic(vec2 uv, vec2 size) {
  const vec2 texel = uv * size - 0.5;
  const vec2 f = fract(texel);
  const vec2 f2 = f * f, f3 = f2 * f;
  // Cubic B-spline weights of the four texels around the sample.
  const vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
  const vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
  const vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
  const vec2 w3 = f3 / 6.0;
  CubicTaps taps;
  taps.g0 = w0 + w1;
  taps.g1 = w2 + w3;
  const vec2 base = floor(texel) + 0.5;
  taps.p0 = (base - 1.0 + w1 / taps.g0) / size;
  taps.p1 = (base + 1.0 + w3 / taps.g1) / size;
  return taps;
}

#define TAP(T, S, x, y) texture(sampler2DArray(T, S), vec3(x, y, 0.0))
#define CUBIC_SAMPLE(T, S, taps)                                                     \
  ((taps).g0.y * ((taps).g0.x * TAP(T, S, (taps).p0.x, (taps).p0.y) +                \
                  (taps).g1.x * TAP(T, S, (taps).p1.x, (taps).p0.y)) +               \
   (taps).g1.y * ((taps).g0.x * TAP(T, S, (taps).p0.x, (taps).p1.y) +                \
                  (taps).g1.x * TAP(T, S, (taps).p1.x, (taps).p1.y)))

#if CUBIC
#define SAMPLE_LAYER(T, S, uv) \
  CUBIC_SAMPLE(T, S, Cubic(uv, vec2(textureSize(sampler2DArray(T, S), 0).xy)))
#else
#define SAMPLE_LAYER(T, S, uv) texture(sampler2DArray(T, S), vec3(uv, 0.0))
#endif
