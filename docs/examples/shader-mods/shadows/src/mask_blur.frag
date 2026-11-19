#version 460
// SR-7: the screen-space shadow mask's blur, 58CE51CE56247C16. Our code;
// it keeps the translation's interface.
//
// The game's blur averages four filtered taps half a guest texel from the
// pixel, red only, and writes (lit, 1, 0, 0). At 1x with linear filtering
// that is a 3x3 tent; above 1x the offsets are half a host texel, so the
// blur covers half the screen width it covers at 1x and the mask's dither
// shows. This one is a tent sized in guest texels (RADIUS, 1 = the game's
// 3x3 at 1x) over every host texel in its footprint, so it is the same
// softness at every resolution scale; RADIUS 2 is twice as wide.

#ifndef RADIUS
#define RADIUS 1.0
#endif

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 words[48];
} xe_fetch;

layout(set = 3, binding = 0) uniform texture2DArray mask_texture;  // fetch 1
layout(set = 3, binding = 2) uniform sampler mask_sampler;

layout(location = 0) in vec4 xe_in_interpolator_0;
layout(location = 0) invariant out vec4 xe_out_fragment_data_0;

uint FetchWord(uint slot, uint word) {
  const uint index = slot * 6u + word;
  return xe_fetch.words[index >> 2u][index & 3u];
}

void main() {
  const uint size_word = FetchWord(1u, 2u);
  const vec2 guest_size = vec2(float(bitfieldExtract(size_word, 0, 13) + 1u),
                               float(bitfieldExtract(size_word, 13, 13) + 1u));
  const ivec2 host_size = textureSize(sampler2DArray(mask_texture, mask_sampler), 0).xy;
  const vec2 host_per_guest = vec2(host_size) / guest_size;
  const float scale = exp2(float(bitfieldExtract(int(FetchWord(1u, 3u)), 13, 6)));

  const vec2 centre = xe_in_interpolator_0.xy * vec2(host_size);
  const float half_width = 2.0 * float(RADIUS);
  const ivec2 first = ivec2(floor(centre - half_width * host_per_guest));
  const ivec2 last = ivec2(ceil(centre + half_width * host_per_guest));
  float sum = 0.0, weight_sum = 0.0;
  for (int y = first.y; y <= last.y; ++y) {
    const float dy = abs(float(y) + 0.5 - centre.y) / host_per_guest.y;
    const float wy = max(0.0, 1.0 - dy / half_width);
    if (wy <= 0.0) continue;
    for (int x = first.x; x <= last.x; ++x) {
      const float dx = abs(float(x) + 0.5 - centre.x) / host_per_guest.x;
      const float w = wy * max(0.0, 1.0 - dx / half_width);
      if (w <= 0.0) continue;
      const ivec2 at = clamp(ivec2(x, y), ivec2(0), host_size - 1);
      sum += w * texelFetch(sampler2DArray(mask_texture, mask_sampler), ivec3(at, 0), 0).r;
      weight_sum += w;
    }
  }
  const float lit = sum * scale / max(weight_sum, 1e-6);
  xe_out_fragment_data_0 = vec4(lit, 1.0, 0.0, 0.0) * xe_system.color_exp_bias.x;
}
