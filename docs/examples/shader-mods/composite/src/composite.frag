#version 460
// SR-1: our own composite for 614588022744BF6B, the pass that turns FH2's
// scene into the final image. Our code; it keeps the translation's
// interface (docs/GRAPHICS_GATES.md, SR-1) and, with every option at its
// default, the game's stages:
//
//   1. camera motion blur: five filtered taps along the velocity, jittered,
//      a tap whose object tag (alpha) differs from the centre's replaced by
//      the centre, so tagged objects (the player's car) do not smear;
//   2. the scene and the bloom, both stored as sqrt(colour), added in
//      linear light;
//   3. exposure (2^x from the vertex shader's metering);
//   4. the game's filmic curve (Hable's rational form, constants c43/c44),
//      normalised by its value at the white point;
//   5. the 16^3 grading LUT, or none left of the split c38.x;
//   6. sqrt encode, then the vignette (a tint and an amount, in that
//      space) and a 3x4 colour matrix in linear light.
//
// Options (defines, one build per choice; tools/build-shader-mod.py):
//   CURVE   0 the game's, 1 Uchimura ("Gran Turismo"), 2 Khronos PBR
//           Neutral, 3 an AgX-like log-sigmoid; 1-3 are matched to the
//           game's curve at mid-grey, so the LUT still sees its range
//   DITHER  1: triangular dither of one 10-bit step from procedural noise
//   BLUR_TAPS  taps of the motion blur (5 = the game's; more is smoother)
//   BLUR_SCALE the blur's length against the game's, as a shutter angle
//           would be (0.5 is half the game's, 0 none)
//   RECON   1: a reconstruction blur (SR-4) after McGuire et al. 2012,
//           velocity only (the composite has no depth): the blur runs along
//           the largest velocity in the neighbourhood, so a fast object's
//           smear reaches past its silhouette, and each tap counts when its
//           own blur or the centre's covers the distance between them.
//           RECON_TAPS taps (default 9). The tag rule still holds.
//   AO      1: multiply the scene by the host's ambient occlusion (SR-10
//           binding 7, computed at the SR-11 hook with
//           swap_ambient_occlusion_in_scene) before the bloom is added, so
//           the occlusion darkens light in the scene rather than the
//           finished, tone-mapped image. Needs
//           vulkan_shader_override_host_inputs; 1 when the host has none.
//   SCENE_FLOAT 1: read the scene from the host's 16-bit copies (SR-12:
//           scene_host_copy, scene_host_copy_pass and both passes in the
//           mid-frame hook list): the bands' float copy (binding 9) where it
//           is within one 10-bit step of the game's texel (where the
//           full-size pass passed the texel through), else the full-size
//           output's copy (binding 8, with unorm10_render_target_as_unorm16)
//           where that is, else the texel (no copy yet, a stale one). The
//           tag (alpha) is always the texel's. 2 and 3 are checks: they
//           write the band copy's difference from the texel, in 10-bit
//           steps, as 0.5 + d / 2, at the pixel (2) and filtered between
//           texels (3).

#ifndef CURVE
#define CURVE 0
#endif
#ifndef DITHER
#define DITHER 0
#endif
#ifndef BLUR_TAPS
#define BLUR_TAPS 5
#endif
#ifndef AO
#define AO 0
#endif
#ifndef RECON
#define RECON 0
#endif
#ifndef RECON_TAPS
#define RECON_TAPS 9
#endif
#ifndef BLUR_SCALE
#define BLUR_SCALE 1.0
#endif
#ifndef SCENE_FLOAT
#define SCENE_FLOAT 0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

// The registers the game's shader uses, packed in order: c32..c44, then
// c253, c254, c255.
layout(set = 1, binding = 2, std140) uniform XeFloatConstants {
  vec4 c[16];
} xe_float;
#define C(n) xe_float.c[(n) - 32]
#define C253 xe_float.c[13]
#define C254 xe_float.c[14]
#define C255 xe_float.c[15]

layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 words[48];
} xe_fetch;

layout(set = 3, binding = 0) uniform texture2DArray velocity_texture;  // fetch 5
layout(set = 3, binding = 2) uniform texture2DArray scene_texture;     // fetch 0
layout(set = 3, binding = 4) uniform texture2DArray bloom_texture;     // fetch 2
layout(set = 3, binding = 6) uniform texture2DArray lut_stacked;       // fetch 7
layout(set = 3, binding = 8) uniform texture3D lut_volume;             // fetch 7
layout(set = 3, binding = 10) uniform sampler velocity_sampler;
layout(set = 3, binding = 11) uniform sampler scene_sampler;
layout(set = 3, binding = 12) uniform sampler scene_point_sampler;
layout(set = 3, binding = 13) uniform sampler bloom_sampler;
layout(set = 3, binding = 14) uniform sampler lut_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;  // xy scene UV, zw bloom UV
layout(location = 1) in vec4 xe_in_interpolator_1;  // xy vignette, z f(white), w log2 exposure
layout(location = 2) in vec4 xe_in_interpolator_2;  // noise seed
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

uint FetchWord(uint slot, uint word) {
  const uint index = slot * 6u + word;
  return xe_fetch.words[index >> 2u][index & 3u];
}
#if AO
layout(set = 0, binding = 7) uniform sampler2D xe_host_scene_ao;
#endif
#if SCENE_FLOAT
layout(set = 0, binding = 8) uniform sampler2D xe_host_scene_copy;
layout(set = 0, binding = 9) uniform sampler2D xe_host_scene_bands;
layout(set = 0, binding = 6, std140) uniform XeHostInputs {
  vec2 draw_resolution_scale;
  float blue_noise_texel;
  uint version;
  // 2^ the resolve's exponent bias: the copy is the render target, the
  // texel the target times this.
  float scene_copy_scale;
  float scene_band_scale;
} xe_host;

// A 16-bit value where it agrees with the 10-bit one (one step, and a
// little for the filter's own rounding).
vec4 Refine(vec4 texel, vec3 band, vec3 copy) {
  const vec3 step = vec3(1.05 / 1023.0);
  band *= xe_host.scene_band_scale;
  copy *= xe_host.scene_copy_scale;
  if (all(lessThanEqual(abs(band - texel.rgb), step))) return vec4(band, texel.a);
  if (all(lessThanEqual(abs(copy - texel.rgb), step))) return vec4(copy, texel.a);
  return texel;
}

ivec2 CopyTexel(vec2 uv) {
  const ivec2 size = textureSize(xe_host_scene_copy, 0);
  return clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
}
#endif

float ExpScale(uint slot) {
  return exp2(float(bitfieldExtract(int(FetchWord(slot, 3u)), 13, 6)));
}

vec4 Scene(vec2 uv) {
  vec4 texel = texture(sampler2DArray(scene_texture, scene_sampler), vec3(uv, 0.0));
#if SCENE_FLOAT
  texel = Refine(texel, textureLod(xe_host_scene_bands, uv, 0.0).rgb,
                 textureLod(xe_host_scene_copy, uv, 0.0).rgb);
#endif
  return texel * ExpScale(0u);
}

vec4 ScenePoint(vec2 uv) {
  vec4 texel = texture(sampler2DArray(scene_texture, scene_point_sampler), vec3(uv, 0.0));
#if SCENE_FLOAT
  texel = Refine(texel, texelFetch(xe_host_scene_bands, CopyTexel(uv), 0).rgb,
                 texelFetch(xe_host_scene_copy, CopyTexel(uv), 0).rgb);
#endif
  return texel * ExpScale(0u);
}

// The game's curve, unnormalised.
vec3 GameCurve(vec3 x) {
  const float a = C(43).y, b = C(43).z;
  const float cb = C(44).x, df = C(44).y, de = C(44).z, e_over_f = C(44).w;
  return (x * (a * x + cb) + de) / (x * (a * x + b) + df) - e_over_f;
}

#if CURVE == 1
// Uchimura 2017 ("HDR theory and practice"), default parameters.
vec3 AltCurve(vec3 x) {
  const float P = 1.0, a = 1.0, m = 0.22, l = 0.4, c = 1.33, b = 0.0;
  const float l0 = ((P - m) * l) / a;
  const float S0 = m + l0, S1 = m + a * l0, C2 = (a * P) / (P - S1), CP = -C2 / P;
  const vec3 w0 = 1.0 - smoothstep(0.0, m, x);
  const vec3 w2 = step(m + l0, x);
  const vec3 w1 = 1.0 - w0 - w2;
  const vec3 T = m * pow(x / m, vec3(c)) + b;
  const vec3 L = m + a * (x - m);
  const vec3 S = P - (P - S1) * exp(CP * (x - S0));
  return T * w0 + L * w1 + S * w2;
}
#elif CURVE == 2
// Khronos PBR Neutral (2024), from its published description.
vec3 AltCurve(vec3 colour) {
  const float start = 0.8 - 0.04, desaturation = 0.15;
  const float x = min(colour.r, min(colour.g, colour.b));
  const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
  colour -= offset;
  const float peak = max(colour.r, max(colour.g, colour.b));
  if (peak < start) return colour;
  const float d = 1.0 - start;
  const float new_peak = 1.0 - d * d / (peak + d - start);
  colour *= new_peak / peak;
  const float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
  return mix(colour, vec3(new_peak), g);
}
#elif CURVE == 3
// An AgX-like curve: a log2 encoding over 16.5 stops around mid-grey and a
// logistic sigmoid, per channel (our own fit, no AgX matrices). Mid-grey
// (0.756 in the encoding) comes out near 0.23 linear.
vec3 AltCurve(vec3 x) {
  const float min_ev = -12.47, max_ev = 4.03, centre = 0.77, slope = 10.0;
  const vec3 v = clamp((log2(max(x, 1e-10) / 0.18) - min_ev) / (max_ev - min_ev), 0.0, 1.0);
  const vec3 s = 1.0 / (1.0 + exp(-slope * (v - centre)));
  const float lo = 1.0 / (1.0 + exp(slope * centre));
  const float hi = 1.0 / (1.0 + exp(-slope * (1.0 - centre)));
  return pow(clamp((s - lo) / (hi - lo), 0.0, 1.0), vec3(2.2));
}
#endif

vec3 Lut(vec3 coordinate) {
  // Word 5, bits 9-10, of fetch 7: 2 is a 3D texture, else stacked layers.
  if (bitfieldExtract(FetchWord(7u, 5u), 9, 2) == 2u) {
    return texture(sampler3D(lut_volume, lut_sampler), coordinate).rgb * ExpScale(7u);
  }
  const float layers = float(textureSize(sampler2DArray(lut_stacked, lut_sampler), 0).z);
  const float layer = coordinate.b * layers - 0.5;
  const float below = clamp(floor(layer), 0.0, layers - 1.0);
  const float above = min(below + 1.0, layers - 1.0);
  const vec3 a = texture(sampler2DArray(lut_stacked, lut_sampler), vec3(coordinate.rg, below)).rgb;
  const vec3 b = texture(sampler2DArray(lut_stacked, lut_sampler), vec3(coordinate.rg, above)).rgb;
  return mix(a, b, clamp(layer - below, 0.0, 1.0)) * ExpScale(7u);
}

// A tagged object's alpha differs from the background's; the blur takes no
// tap across a tag.
vec3 TapSquared(vec2 uv, float centre_tag, vec3 centre_squared) {
  const vec4 s = Scene(uv);
  return s.a == centre_tag ? s.rgb * s.rgb : centre_squared;
}

vec2 Velocity(vec2 uv) {
  return (texture(sampler2DArray(velocity_texture, velocity_sampler), vec3(uv, 0.0)).xy *
              ExpScale(5u) * C255.w +
          C255.x) *
         C(42).x * float(BLUR_SCALE);
}

#if RECON
// 1 where a blur of length `reach` (in pixels) covers `distance`, falling to 0
// over one pixel.
float Cover(float distance, float reach) { return clamp(1.0 - distance / max(reach, 1.0), 0.0, 1.0); }
#endif

float Noise(vec2 pixel, float seed) {
  // Interleaved gradient noise (Jimenez 2014), offset per use.
  return fract(52.9829189 * fract(dot(pixel + seed * vec2(47.0, 17.0), vec2(0.06711056, 0.00583715))));
}

void main() {
  const vec2 uv = xe_in_interpolator_0.xy;

  // 1-2. Motion blur and the scene in linear light.
  const vec4 centre = ScenePoint(uv);
#if SCENE_FLOAT >= 2
  {
#if SCENE_FLOAT == 2
    const vec4 texel = texture(sampler2DArray(scene_texture, scene_point_sampler), vec3(uv, 0.0));
    const vec3 band = texelFetch(xe_host_scene_bands, CopyTexel(uv), 0).rgb;
#else
    const vec2 at = uv + vec2(0.37, 0.61) / vec2(textureSize(xe_host_scene_bands, 0));
    const vec4 texel = texture(sampler2DArray(scene_texture, scene_sampler), vec3(at, 0.0));
    const vec3 band = textureLod(xe_host_scene_bands, at, 0.0).rgb;
#endif
    xe_out_fragment_data_0 =
        vec4(clamp(0.5 + (band * xe_host.scene_band_scale - texel.rgb) * 1023.0 * 0.5, 0.0, 1.0),
             1.0) *
        xe_system.color_exp_bias.x;
    return;
  }
#endif
  const vec3 centre_squared = centre.rgb * centre.rgb;
  const vec2 velocity = Velocity(uv);
  const vec3 jitter =
      (fract(xe_in_interpolator_2.xyz * xe_in_interpolator_2.xyz) * C255.w + C253.x) * C(42).z;
  vec3 scene;
#if RECON
  {
    const vec2 pixels = vec2(textureSize(sampler2DArray(scene_texture, scene_point_sampler), 0).xy);
    const float span = max(max(abs(C254.x), abs(C254.y)), max(abs(C255.y), abs(C255.z)));
    // The neighbourhood's largest velocity: eight samples on a ring as wide
    // as the centre's blur or 16 pixels, whichever is larger.
    const float ring = max(length(velocity * pixels) * span, 16.0);
    vec2 dominant = velocity;
    float dominant_length = length(velocity * pixels);
    const float turn = Noise(gl_FragCoord.xy, 3.0) * 0.785398;
    for (int i = 0; i < 8; ++i) {
      const float a = turn + float(i) * 0.785398;
      const vec2 v = Velocity(uv + vec2(cos(a), sin(a)) * ring / pixels);
      const float l = length(v * pixels);
      if (l > dominant_length) {
        dominant = v;
        dominant_length = l;
      }
    }
    const float centre_reach = length(velocity * pixels) * span;
    // Only a velocity edge (an object much faster than the pixel) changes the
    // direction; on the smooth gradient of camera motion the pixel's own
    // velocity is kept, so the blur is the game's length there.
    if (dominant_length < 2.0 * length(velocity * pixels) + 2.0) dominant = velocity;
    const float jitter_t = Noise(gl_FragCoord.xy, 5.0) - 0.5;
    vec3 sum = centre_squared / max(centre_reach, 1.0);
    float weight = 1.0 / max(centre_reach, 1.0);
    for (int i = 0; i < RECON_TAPS; ++i) {
      if (i == RECON_TAPS / 2) continue;
      const float t = mix(-span, span, (float(i) + 0.5 + jitter_t) / float(RECON_TAPS));
      const vec2 at = uv + dominant * t;
      const vec4 s = Scene(at);
      if (s.a != centre.a) continue;
      const float distance = length((at - uv) * pixels);
      const float reach = length(Velocity(at) * pixels) * span;
      const float w = max(Cover(distance, reach), Cover(distance, centre_reach));
      sum += w * s.rgb * s.rgb;
      weight += w;
    }
    scene = sum / weight;
  }
#elif BLUR_TAPS == 5
  scene = TapSquared(uv + velocity * (C254.x + jitter.x), centre.a, centre_squared) +
          TapSquared(uv + velocity * (C254.y + jitter.x), centre.a, centre_squared) +
          TapSquared(uv + velocity * (C255.y + jitter.y), centre.a, centre_squared) +
          TapSquared(uv + velocity * (C255.z + jitter.y), centre.a, centre_squared) +
          TapSquared(uv + velocity * jitter.z, centre.a, centre_squared);
  scene *= C255.w;
#else
  // Evenly spread over the game's span (its outermost offsets), with one
  // jitter for all taps.
  const float span_lo = min(min(C254.x, C254.y), min(C255.y, C255.z));
  const float span_hi = max(max(C254.x, C254.y), max(C255.y, C255.z));
  scene = vec3(0.0);
  for (int i = 0; i < BLUR_TAPS; ++i) {
    const float t = (float(i) + fract(jitter.x * 7.0 + 0.5)) / float(BLUR_TAPS);
    scene += TapSquared(uv + velocity * mix(span_lo, span_hi, t), centre.a, centre_squared);
  }
  scene /= float(BLUR_TAPS);
#endif
  const vec3 bloom =
      texture(sampler2DArray(bloom_texture, bloom_sampler), vec3(xe_in_interpolator_0.zw, 0.0))
          .rgb *
      ExpScale(2u);
#if AO
  scene *= texture(xe_host_scene_ao, uv).r;
#endif
  const vec3 linear = scene + bloom * bloom;

  // 3-5. Exposure, the curve, the LUT.
  const vec3 exposed = exp2(xe_in_interpolator_1.w) * linear;
  const float white = xe_in_interpolator_1.z;
  vec3 curved = GameCurve(exposed);
#if CURVE != 0
  {
    const float grey = GameCurve(vec3(0.18)).g;
    curved = AltCurve(exposed) * (grey / max(AltCurve(vec3(0.18)).g, 1e-6));
    curved = min(curved, vec3(white));
  }
#endif
  vec3 colour;
  if (C(38).x > uv.x) {
    colour = Lut((C(36).x / white) * curved + C(37).x);
  } else {
    colour = curved / white;
  }

  // 6. Encode, vignette, colour matrix.
  vec3 s = sqrt(abs(colour));
  const vec2 q = mat2(C(32).x, C(32).z, C(32).y, C(32).w) * (xe_in_interpolator_1.xy + C(35).xy);
  const float v = clamp(pow(dot(q, q) + C253.y, C(33).w), 0.0, 1.0);
  s = mix(s, clamp(s + C254.w + C(34).rgb, 0.0, 1.0), v * C(34).w);
  const vec4 l = vec4(s * s, 1.0);
  vec3 result = sqrt(clamp(vec3(dot(C(39), l), dot(vec4(C(40).xyz, C(40).w * C254.z), l),
                                dot(vec4(C(41).xyz, C(41).w * C254.z), l)),
                           0.0, 1.0));

#if DITHER
  // One 10-bit step, triangular (the sum of two uniform noises).
  const float n = Noise(gl_FragCoord.xy, 0.0) + Noise(gl_FragCoord.xy, 1.0) - 1.0;
  result = clamp(result + n / 1023.0, 0.0, 1.0);
#endif

  xe_out_fragment_data_0 = vec4(result, 1.0) * xe_system.color_exp_bias.x;
}
