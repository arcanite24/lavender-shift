// Shared by the bloom replacements (SR-2). Our own code; it keeps only the
// interface the translations have (docs/GRAPHICS_GATES.md, SR-2).
//
// The chain stores sqrt(colour): every pass squares what it reads, filters
// in linear light and writes the square root back.

layout(set = 1, binding = 0, std140) uniform XeSystemConstants {
  layout(offset = 0) uint flags;
  layout(offset = 288) vec4 color_exp_bias;
} xe_system;

layout(set = 1, binding = 4, std140) uniform XeFetchConstants {
  uvec4 words[48];
} xe_fetch;

uint FetchWord(uint slot, uint word) {
  const uint index = slot * 6u + word;
  return xe_fetch.words[index >> 2u][index & 3u];
}

// The guest texture's size in guest texels (word 2: width - 1 in bits 0-12,
// height - 1 in bits 13-25).
vec2 GuestSize(uint slot) {
  const uint word = FetchWord(slot, 2u);
  return vec2(float(bitfieldExtract(word, 0, 13) + 1u),
              float(bitfieldExtract(word, 13, 13) + 1u));
}

// The fetch's exponent adjustment (word 3, bits 13-18, signed).
float ExpScale(uint slot) {
  return exp2(float(bitfieldExtract(int(FetchWord(slot, 3u)), 13, 6)));
}

// Every pass's output goes through the colour exponent bias, like a
// translation's.
vec4 Finish(vec4 colour) { return colour * xe_system.color_exp_bias.x; }
