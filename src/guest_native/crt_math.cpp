// Native replacement of FH2's CRT pow (docs/PINYON_PARITY.md LP-4.3), Pinyon
// Shift's port (its src/guest_native/crt_math.cpp) with FH2's addresses.
//
// FH2 links the same pow as FH1 (TU v2 sub_82448D90, FH1 v4 sub_82468E50;
// 145 direct call sites): its 1,252 bytes and every function it calls match
// FH1 v4's word for word once branch targets and constant addresses are
// masked, and the one differing displacement is the constant 2.0 moving with
// the rest of the first data page. The natural log it calls for an exponent
// of 1 (TU v2 sub_82C04490, FH1 v4 sub_82B3DF18) matches the same way.
//
// The CRT pow is a Cody-Waite style pow: it reduces x to a mantissa and a
// sixteenth-step table entry, evaluates a log2 polynomial, splits y*log2(x)
// into whole sixteenths and a remainder and rebuilds the result from a
// 2^(k/16) table, an exp2 polynomial and the exponent bits; integral x and
// small positive integral y take repeated squaring instead.
//
// This port performs the same double operations in the same order, fused
// where the recompiled code uses std::fma, with the recompiled code's float
// to integer conversions, so its results are bit-identical to the
// recompiled function's. The coefficient tables are read from guest memory.
// Infinite, NaN and denormal arguments and negative x with non-integral y
// are left to the original, which also handles their error reporting.

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <simde/x86/sse2.h>

#include "guest_native/guest_native.h"

#pragma clang fp contract(off)

REXCVAR_DEFINE_BOOL(lavender_native_pow, true, "Lavender Shift",
                    "Run FH2's CRT pow as bit-identical native code")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace lavender_shift::guest_native {
namespace {

// Constant addresses in FH2 TU v2 (FH1 v4's, mapped through the lis/addi
// pairs that load them and checked by value).
constexpr uint32_t kZero = 0x82000C90;
constexpr uint32_t kOne = 0x82000BC0;
constexpr uint32_t kMinusOne = 0x82000C10;
constexpr uint32_t kSixteenth = 0x82000BF8;
constexpr uint32_t kTwo = 0x82000BB8;
constexpr uint32_t kHalf = 0x8207E190;

struct PowConstants {
  uint32_t integral_y_limit;  // largest y taking repeated squaring
  uint32_t huge_y;            // |y| above this saturates
  uint32_t overflow;          // the overflow result
  uint32_t sixteen;
  uint32_t table;             // 2^(-k/16) entries, their tails and both polynomials
  uint32_t ln_denormal_limit;
  uint32_t ln_table;          // sqrt(1/2) and the log polynomials
  uint32_t ln_c0;             // five consecutive log constants
};

// The three log constants sit at the table's FH1 v4 distance (-0x35B8 for
// all of them); found by value.
constexpr PowConstants kConstants = {0x82001C80, 0x833DBA68, 0x833DBCE8, 0x820D4F98,
                                     0x820E64B8, 0x820E66E0, 0x820E6660, 0x820E66B0};

struct Guest {
  const uint8_t* base;
  double operator()(uint32_t address) const {
    uint64_t bits;
    std::memcpy(&bits, base + address, sizeof(bits));
    return std::bit_cast<double>(std::byteswap(bits));
  }
  double table(uint32_t index) const { return (*this)(kConstants.table + index * 8); }
  double ln_table(uint32_t index) const { return (*this)(kConstants.ln_table + index * 8); }
};

uint32_t High16(double value) {
  return static_cast<uint32_t>(std::bit_cast<uint64_t>(value) >> 48);
}

double WithHigh16(double value, uint32_t high16) {
  const uint64_t bits = (std::bit_cast<uint64_t>(value) & 0x0000FFFFFFFFFFFFull) |
                        (static_cast<uint64_t>(high16 & 0xFFFF) << 48);
  return std::bit_cast<double>(bits);
}

// fctid as the recompiled code lowers it (current rounding mode).
int64_t Fctid(double value) {
  if (std::isnan(value)) return INT64_MIN;
  if (value > double(LLONG_MAX)) return LLONG_MAX;
  return simde_mm_cvtsd_si64(simde_mm_load_sd(&value));
}

double RoundToIntegral(double value) { return double(Fctid(value)); }

// fctiwz as the recompiled code lowers it, low word.
int32_t Fctiwz(double value) {
  if (std::isnan(value)) return INT32_MIN;
  if (value >= double(INT_MAX)) return INT_MAX;
  return simde_mm_cvttsd_si32(simde_mm_load_sd(&value));
}

// The CRT's integer test (sub_8244A250): 0 not integral, 1 odd, 2 even.
int Classify(const Guest& k, double value) {
  const uint64_t bits = std::bit_cast<uint64_t>(value);
  if ((High16(value) & 0x7FF0) == 0 && (bits & 0x000FFFFFFFFFFFFFull) != 0) return 0;
  if (RoundToIntegral(value) != value) return 0;
  const double half = value * k(kHalf);
  return RoundToIntegral(half) == half ? 2 : 1;
}

// The exponent the CRT reads from a double's bits (sub_82A84E48).
int32_t Exponent(double value) {
  return static_cast<int16_t>(((High16(value) >> 4) & 0x7FF) - 1022);
}

// Natural log of a normal positive x (sub_82A80B78); false when the CRT
// would take its denormal path.
bool NaturalLog(const Guest& k, double x, double& result) {
  const double one = k(kOne);
  if (x == one) {
    result = k(kZero);
    return true;
  }
  if (x < k(kConstants.ln_denormal_limit)) return false;
  const uint32_t high = High16(x);
  int32_t exponent = static_cast<int32_t>((high >> 4) & 0xFFF) - 1022;
  double mantissa = WithHigh16(x, (high & 0x800F) | 0x3FE0);
  const double half = k(kHalf);
  double numerator;
  double denominator;
  if (mantissa > k.ln_table(0)) {
    const double above = mantissa + one;
    const double less_half = mantissa - half;
    denominator = above * half;
    numerator = less_half - half;
  } else {
    exponent -= 1;
    numerator = mantissa - half;
    denominator = numerator + one;
    denominator = denominator * half;
  }
  const double z = numerator / denominator;
  const double c0 = k(kConstants.ln_c0);
  const double c1 = k(kConstants.ln_c0 + 8);
  const double c2 = k(kConstants.ln_c0 + 16);
  const double c3 = k(kConstants.ln_c0 + 24);
  const double c4 = k(kConstants.ln_c0 + 32);
  const double n = double(int64_t(exponent));
  const double w = z * z;
  const double n_c0 = n * c0;
  double p = -std::fma(w, c4, -k.ln_table(5));
  double q = w - c3;
  p = std::fma(p, w, -c2);
  q = std::fma(q, w, k.ln_table(8));
  p = p * w;
  q = std::fma(q, w, -c1);
  p = p / q;
  p = p + one;
  const double low = std::fma(p, z, -n_c0);
  result = std::fma(n, k.ln_table(1), low);
  return true;
}

// The result for an exponent and a mantissa-scaled value, as the CRT ends.
double Finish(const Guest& k, int32_t exponent, double value, double sign) {
  if (exponent > 1024) return k(kConstants.overflow) * sign;
  if (exponent < -1021) return sign * k(kZero);
  const double scaled =
      WithHigh16(value, (static_cast<uint32_t>(exponent + 1022) << 4) | (High16(value) & 0x800F));
  return scaled * sign;
}

bool Pow(const Guest& k, double x, double y, double& result) {
  const double zero = k(kZero);
  const double one = k(kOne);
  if (y == zero) {
    result = one;
    return true;
  }
  if (x == zero) {
    // Zero to a power: infinity (signed like x for odd integral y) for
    // negative y, x for odd integral y, otherwise zero; NaN y goes on to the
    // CRT's NaN handling.
    const int y_class = Classify(k, y);
    if (y < zero) {
      result = k(kConstants.overflow);
      if (y_class == 1) result = std::copysign(result, x);
      return true;
    }
    if (y > zero) {
      result = y_class == 1 ? x : zero;
      return true;
    }
    return false;
  }
  if ((High16(x) & 0x7FF0) == 0x7FF0 || (High16(y) & 0x7FF0) == 0x7FF0) return false;
  double sign = one;
  if (x < zero) {
    const int y_class = Classify(k, y);
    if (y_class == 0) return false;
    if (y_class == 1) sign = k(kMinusOne);
    x = -x;
  }
  if (std::fabs(y) > k(kConstants.huge_y)) {
    if (y < zero) x = one / x;
    if (x > one) {
      result = k(kConstants.overflow) * sign;
    } else if (x < one) {
      result = sign * zero;
    } else {
      result = sign;
    }
    return true;
  }
  // frexp; the CRT normalizes denormals first.
  const uint32_t x_high = High16(x);
  if ((x_high & 0x7FF0) == 0) return false;
  const int32_t x_exponent = static_cast<int32_t>((x_high & 0x7FF0) >> 4) - 1022;
  double mantissa = WithHigh16(x, (x_high & 0x800F) | 0x3FE0);

  if (!(y > k(kConstants.integral_y_limit)) && Classify(k, x) != 0 && Classify(k, y) != 0 &&
      y > zero) {
    int32_t n = Fctiwz(y);
    const int32_t scale = static_cast<int32_t>(static_cast<uint32_t>(n) *
                                               static_cast<uint32_t>(x_exponent));
    double product = one;
    while (n != 0) {
      if (n & 1) product = product * mantissa;
      n >>= 1;
      mantissa = mantissa * mantissa;
    }
    const int32_t exponent = Exponent(product) + scale;
    if (exponent > 2560) {
      const double overflow = k(kConstants.overflow) * product;
      result = overflow * sign;
      return true;
    }
    if (exponent < -2557) {
      const double signed_product = product * sign;
      result = signed_product * zero;
      return true;
    }
    result = Finish(k, exponent, product, sign);
    return true;
  }

  // Sixteenth-step reduction of the mantissa.
  int32_t p = mantissa > k.table(9) ? 1 : 9;
  if (!(mantissa > k.table(p + 4))) p += 4;
  if (!(mantissa > k.table(p + 2))) p += 2;
  const int32_t m = static_cast<int32_t>(static_cast<uint32_t>(x_exponent) << 4) - p;
  const double sixteenth = k(kSixteenth);
  double u1;
  double u2;
  if (m == 1 || m == -1) {
    double ln;
    if (!NaturalLog(k, x, ln)) return false;
    u1 = ln * k.table(27);
    u2 = zero;
  } else {
    const double a = k.table(p + 1);
    double s = mantissa - a;
    const double t = a + mantissa;
    const double whole = double(int64_t(m));
    const double tail = k.table(18 + (p + 1) / 2);
    u1 = whole * sixteenth;
    s = s - tail;
    s = s / t;
    const double z = s * k(kTwo);
    const double v = z * z;
    double r = z * k.table(28);
    double poly = std::fma(v, k.table(32), k.table(31));
    poly = std::fma(poly, v, k.table(30));
    poly = std::fma(poly, v, k.table(29));
    poly = poly * v;
    poly = poly * z;
    r = std::fma(poly, k.table(27), r);
    u2 = r + z;
  }

  // y * log2(x) in whole sixteenths and a remainder.
  const double sixteen = k(kConstants.sixteen);
  double w2 = u2 * y;
  double y1 = y * sixteen;
  y1 = RoundToIntegral(y1);
  y1 = y1 * sixteenth;
  const double y2 = y - y1;
  w2 = std::fma(y2, u1, w2);
  double w = w2 * sixteen;
  w = RoundToIntegral(w);
  w = w * sixteenth;
  double w1 = std::fma(y1, u1, w);
  w2 = w2 - w;
  double whole = w1 * sixteen;
  whole = RoundToIntegral(whole);
  whole = whole * sixteenth;
  w1 = w1 - whole;
  w1 = w1 + w2;
  double carry = w1 * sixteen;
  carry = RoundToIntegral(carry);
  carry = carry * sixteenth;
  whole = whole + carry;
  w1 = w1 - carry;
  const double sixteenths = whole * sixteen;
  if (sixteenths > k.table(40)) {
    result = k(kConstants.overflow) * sign;
    return true;
  }
  if (sixteenths < k.table(41)) {
    result = sign * zero;
    return true;
  }
  int32_t iw = Fctiwz(sixteenths);
  if (w1 > zero) {
    iw += 1;
    w1 = w1 - sixteenth;
  }
  double e = std::fma(w1, k.table(39), k.table(38));
  e = std::fma(e, w1, k.table(37));
  e = std::fma(e, w1, k.table(36));
  e = std::fma(e, w1, k.table(35));
  e = std::fma(e, w1, k.table(34));
  e = std::fma(e, w1, k.table(33));
  e = std::fma(e, w1, one);
  const int32_t mp = iw / 16 + (iw >= 0 ? 1 : 0);
  const int32_t index = mp * 16 - iw;
  const double value = e * k.table(index + 1);
  result = Finish(k, Exponent(value) + mp, value, sign);
  return true;
}

bool NativePow(PPCContext& ctx, uint8_t* base) {
  ctx.fpscr.disableFlushMode();
  double result;
  if (!Pow(Guest{base}, ctx.f1.f64, ctx.f2.f64, result)) return false;
  ctx.f1.f64 = result;
  return true;
}

bool PowEnabled() { return REXCVAR_GET(lavender_native_pow); }

Replacement pow_replacement = {
    .name = "crt.pow",
    .guest_address = 0x82448D90u,
    .guest_code_size = 1252,
    .guest_code_fnv1a = 0x25F163CCBB8A97ADull,
    .returns = kReturnF1,
    .enabled = PowEnabled,
    .native = NativePow,
    .written = nullptr,
};

}  // namespace
}  // namespace lavender_shift::guest_native

LAVENDER_GUEST_NATIVE(82448D90, ::lavender_shift::guest_native::pow_replacement)
