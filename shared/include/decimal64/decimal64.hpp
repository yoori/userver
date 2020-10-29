#pragma once

/// @file decimal64/decimal64.hpp
/// @brief Decimal data type for fixed-point arithmetic

// Original source taken from https://github.com/vpiotr/decimal_for_cpp
// Original license:
//
// ==================================================================
// Name:        decimal.h
// Purpose:     Decimal data type support, for COBOL-like fixed-point
//              operations on currency values.
// Author:      Piotr Likus
// Created:     03/01/2011
// Modified:    23/09/2018
// Version:     1.16
// Licence:     BSD
// ==================================================================

#include <array>
#include <cassert>
#include <cstdint>
#include <ios>
#include <iosfwd>
#include <istream>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/compile.h>
#include <fmt/format.h>

#include <formats/common/meta.hpp>
#include <utils/assert.hpp>
#include <utils/flags.hpp>
#include <utils/meta.hpp>

namespace logging {

class LogHelper;

}  // namespace logging

namespace decimal64 {

namespace impl {

// 0 for strong typing (same precision required for
// both arguments),
// 1 for allowing to mix lower or equal precision types
// 2 for automatic rounding when different precision is mixed
inline constexpr int kTypeLevel = 2;

template <int PrecFrom, int PrecTo>
constexpr void CheckPrecCast() {
  static_assert(
      kTypeLevel >= 2 || (kTypeLevel >= 1 && PrecFrom <= PrecTo),
      "This implicit decimal cast is not allowed under current settings");
}

inline constexpr auto kMaxInt64 = std::numeric_limits<int64_t>::max();
inline constexpr auto kMinInt64 = std::numeric_limits<int64_t>::min();

template <typename T>
using EnableIfInt = std::enable_if_t<meta::kIsInteger<T>, int>;

template <typename T>
using EnableIfFloat = std::enable_if_t<std::is_floating_point_v<T>, int>;

template <int MaxExp>
constexpr std::array<int64_t, MaxExp + 1> PowSeries(int64_t base) {
  int64_t pow = 1;
  std::array<int64_t, MaxExp + 1> result{};
  for (int i = 0; i < MaxExp; ++i) {
    result[i] = pow;
    if (pow > kMaxInt64 / base) {
      throw std::runtime_error("Overflow");
    }
    pow *= base;
  }
  result[MaxExp] = pow;
  return result;
}

inline constexpr int kMaxDecimalDigits = 18;
inline constexpr auto kPowSeries10 = PowSeries<kMaxDecimalDigits>(10);

// Check that kMaxDecimalDigits is indeed max integer x such that 10^x is valid
static_assert(kMaxInt64 / 10 < kPowSeries10[kMaxDecimalDigits]);

constexpr bool IsMultOverflowPositive(int64_t value1, int64_t value2) {
  UASSERT(value1 > 0 && value2 > 0);
  return value1 > impl::kMaxInt64 / value2 && value2 > impl::kMaxInt64 / value1;
}

constexpr bool IsMultOverflow(int64_t value1, int64_t value2) {
  if (value1 == 0 || value2 == 0) {
    return false;
  }

  if ((value1 < 0) != (value2 < 0)) {  // different sign
    if (value1 == impl::kMinInt64) {
      return value2 > 1;
    } else if (value2 == impl::kMinInt64) {
      return value1 > 1;
    }
    if (value1 < 0) {
      return IsMultOverflowPositive(-value1, value2);
    }
    if (value2 < 0) {
      return IsMultOverflowPositive(value1, -value2);
    }
  } else if (value1 < 0 && value2 < 0) {
    if (value1 == impl::kMinInt64) {
      return value2 < -1;
    } else if (value2 == impl::kMinInt64) {
      return value1 < -1;
    }
    return IsMultOverflowPositive(-value1, -value2);
  }

  return IsMultOverflowPositive(value1, value2);
}

// result = (value1 * value2) / divisor
template <class RoundPolicy>
constexpr int64_t MultDiv(int64_t value1, int64_t value2, int64_t divisor) {
  // we don't check for division by zero, the caller should - the next line
  // will throw.
  const int64_t value1int = value1 / divisor;
  int64_t value1dec = value1 % divisor;
  const int64_t value2int = value2 / divisor;
  int64_t value2dec = value2 % divisor;

  int64_t result = value1 * value2int + value1int * value2dec;

  if (value1dec == 0 || value2dec == 0) {
    return result;
  }

  if (!IsMultOverflow(value1dec, value2dec)) {  // no overflow
    int64_t res_dec_part = value1dec * value2dec;
    if (!RoundPolicy::DivRounded(res_dec_part, res_dec_part, divisor)) {
      res_dec_part = 0;
    }
    result += res_dec_part;
    return result;
  }

  // reduce value1 & divisor
  {
    const int64_t c = std::gcd(value1dec, divisor);
    if (c != 1) {
      value1dec /= c;
      divisor /= c;
    }
  }

  // reduce value2 & divisor
  {
    const int64_t c = std::gcd(value2dec, divisor);
    if (c != 1) {
      value2dec /= c;
      divisor /= c;
    }
  }

  if (!IsMultOverflow(value1dec, value2dec)) {  // no overflow
    int64_t res_dec_part = value1dec * value2dec;
    if (RoundPolicy::DivRounded(res_dec_part, res_dec_part, divisor)) {
      result += res_dec_part;
      return result;
    }
  }

  // overflow can occur - use less precise version
  result += RoundPolicy::Round(static_cast<long double>(value1dec) *
                               static_cast<long double>(value2dec) /
                               static_cast<long double>(divisor));
  return result;
}

// Needed because std::abs is not constexpr
template <typename T>
constexpr T Abs(T value) {
  return value >= T(0) ? value : -value;
}

// Needed because std::floor is not constexpr
// Insignificantly less performant
template <typename T>
constexpr int64_t Floor(T value) {
  if (static_cast<int64_t>(value) <= value) {  // whole or positive
    return static_cast<int64_t>(value);
  } else {
    return static_cast<int64_t>(value) - 1;
  }
}

// Needed because std::ceil is not constexpr
// Insignificantly less performant
template <typename T>
constexpr int64_t Ceil(T value) {
  if (static_cast<int64_t>(value) >= value) {  // whole or negative
    return static_cast<int64_t>(value);
  } else {
    return static_cast<int64_t>(value) + 1;
  }
}

}  // namespace impl

/// A fast, constexpr-friendly power of 10
constexpr int64_t Pow10(int exp) {
  if (exp < 0 || exp > impl::kMaxDecimalDigits) {
    throw std::runtime_error("Pow10: invalid power of 10");
  }
  return impl::kPowSeries10[static_cast<size_t>(exp)];
}

/// A guaranteed-compile-time power of 10
template <int Exp>
inline constexpr int64_t kPow10 = Pow10(Exp);

/// The fastest rounding. Rounds towards zero.
class NullRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    return static_cast<int64_t>(value);
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    output = a / b;
    return true;
  }
};

/// @brief Default rounding. Fast, rounds to nearest.
///
/// On 0.5, rounds away from zero. Also, sometimes rounds up numbers
/// in the neighborhood of 0.5, e.g. 0.49999999999999994 -> 1.
class DefRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    return static_cast<int64_t>(value + (value < 0 ? -0.5 : 0.5));
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t divisor_corr = impl::Abs(b / 2);
    if (a >= 0) {
      if (impl::kMaxInt64 - a >= divisor_corr) {
        output = (a + divisor_corr) / b;
        return true;
      }
    } else {
      if (-(impl::kMinInt64 - a) >= divisor_corr) {
        output = (a - divisor_corr) / b;
        return true;
      }
    }

    output = 0;
    return false;
  }
};

/// Round to nearest, 0.5 towards zero
class HalfDownRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    if (value >= 0.0) {
      const T decimals = value - impl::Floor(value);
      if (decimals > 0.5) {
        return impl::Ceil(value);
      } else {
        return impl::Floor(value);
      }
    } else {
      const T decimals = impl::Ceil(value) - value;
      if (decimals < 0.5) {
        return impl::Ceil(value);
      } else {
        return impl::Floor(value);
      }
    }
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    int64_t divisor_corr = impl::Abs(b) / 2;
    int64_t remainder = impl::Abs(a) % impl::Abs(b);

    if (a >= 0) {
      if (impl::kMaxInt64 - a >= divisor_corr) {
        if (remainder > divisor_corr) {
          output = (a + divisor_corr) / b;
        } else {
          output = a / b;
        }
        return true;
      }
    } else {
      if (-(impl::kMinInt64 - a) >= divisor_corr) {
        output = (a - divisor_corr) / b;
        return true;
      }
    }

    output = 0;
    return false;
  }
};

/// Round to nearest, 0.5 away from zero
class HalfUpRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    if (value >= 0.0) {
      const T decimals = value - impl::Floor(value);
      if (decimals >= 0.5) {
        return impl::Ceil(value);
      } else {
        return impl::Floor(value);
      }
    } else {
      const T decimals = impl::Ceil(value) - value;
      if (decimals <= 0.5) {
        return impl::Ceil(value);
      } else {
        return impl::Floor(value);
      }
    }
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t divisor_corr = impl::Abs(b) / 2;
    const int64_t remainder = impl::Abs(a) % impl::Abs(b);

    if (a >= 0) {
      if (impl::kMaxInt64 - a >= divisor_corr) {
        if (remainder >= divisor_corr) {
          output = (a + divisor_corr) / b;
        } else {
          output = a / b;
        }
        return true;
      }
    } else {
      if (-(impl::kMinInt64 - a) >= divisor_corr) {
        if (remainder < divisor_corr) {
          output = (a - remainder) / b;
        } else if (remainder == divisor_corr) {
          output = (a + divisor_corr) / b;
        } else {
          output = (a + remainder - impl::Abs(b)) / b;
        }
        return true;
      }
    }

    output = 0;
    return false;
  }
};

/// Round to nearest, 0.5 towards number with even last digit
class HalfEvenRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    if (value >= 0.0) {
      const T decimals = value - impl::Floor(value);
      if (decimals > 0.5) {
        return impl::Ceil(value);
      } else if (decimals < 0.5) {
        return impl::Floor(value);
      } else {
        const bool is_even = impl::Floor(value) % 2 == 0;
        if (is_even) {
          return impl::Floor(value);
        } else {
          return impl::Ceil(value);
        }
      }
    } else {
      const T decimals = impl::Ceil(value) - value;
      if (decimals > 0.5) {
        return impl::Floor(value);
      } else if (decimals < 0.5) {
        return impl::Ceil(value);
      } else {
        const bool is_even = impl::Ceil(value) % 2 == 0;
        if (is_even) {
          return impl::Ceil(value);
        } else {
          return impl::Floor(value);
        }
      }
    }
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t divisor_div2 = impl::Abs(b) / 2;
    const int64_t remainder = impl::Abs(a) % impl::Abs(b);

    if (remainder == 0) {
      output = a / b;
    } else {
      if (a >= 0) {
        if (remainder > divisor_div2) {
          output = (a - remainder + impl::Abs(b)) / b;
        } else if (remainder < divisor_div2) {
          output = (a - remainder) / b;
        } else {
          const bool is_even = impl::Abs(a / b) % 2 == 0;
          if (is_even) {
            output = a / b;
          } else {
            output = (a - remainder + impl::Abs(b)) / b;
          }
        }
      } else {
        // negative value
        if (remainder > divisor_div2) {
          output = (a + remainder - impl::Abs(b)) / b;
        } else if (remainder < divisor_div2) {
          output = (a + remainder) / b;
        } else {
          const bool is_even = impl::Abs(a / b) % 2 == 0;
          if (is_even) {
            output = a / b;
          } else {
            output = (a + remainder - impl::Abs(b)) / b;
          }
        }
      }
    }

    return true;
  }
};

/// Round towards +infinity
class CeilingRoundPolicy {
 public:
  template <class T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    return impl::Ceil(value);
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t remainder = impl::Abs(a) % impl::Abs(b);
    if (remainder == 0) {
      output = a / b;
    } else {
      if (a >= 0) {
        output = (a + impl::Abs(b)) / b;
      } else {
        output = a / b;
      }
    }
    return true;
  }
};

/// Round towards -infinity
class FloorRoundPolicy {
 public:
  template <typename T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    return impl::Floor(value);
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t remainder = impl::Abs(a) % impl::Abs(b);
    if (remainder == 0) {
      output = a / b;
    } else {
      if (a >= 0) {
        output = (a - remainder) / b;
      } else {
        output = (a + remainder - impl::Abs(b)) / b;
      }
    }
    return true;
  }
};

/// Round towards zero
class RoundDownRoundPolicy : public NullRoundPolicy {};

/// Round away from zero
class RoundUpRoundPolicy {
 public:
  template <typename T>
  [[nodiscard]] static constexpr int64_t Round(T value) {
    if (value >= 0.0) {
      return impl::Ceil(value);
    } else {
      return impl::Floor(value);
    }
  }

  [[nodiscard]] static constexpr bool DivRounded(int64_t& output, int64_t a,
                                                 int64_t b) {
    const int64_t remainder = impl::Abs(a) % impl::Abs(b);
    if (remainder == 0) {
      output = a / b;
    } else {
      if (a >= 0) {
        output = (a + impl::Abs(b)) / b;
      } else {
        output = (a - impl::Abs(b)) / b;
      }
    }
    return true;
  }
};

/// @brief Fixed-point decimal data type for use in deterministic calculations,
/// oftentimes involving money
///
/// @tparam Prec The number of fractional digits
/// @tparam RoundPolicy Specifies how to round in lossy operations
///
/// Decimal is internally represented as `int64_t`. It means that it can be
/// passed around by value. It also means that operations with huge
/// numbers can overflow and trap. For example, with `Prec == 6`, the maximum
/// representable number is about 10 trillion.
///
/// Decimal should be serialized and stored as a string, NOT as `double`. Use
/// `Decimal{str}` constructor (or `Decimal::FromStringPermissive` if rounding
/// is allowed) to read a `Decimal`, and `ToString(dec)`
/// (or `ToStringTrailingZeros(dec)`) to write a `Decimal`.
///
/// Use arithmetic with caution! Multiplication and division operations involve
/// rounding. You may want to cast to `Decimal` with another `Prec`
/// or `RoundPolicy` beforehand. For that purpose you can use
/// `decimal64::decimal_cast<NewDec>(dec)`.
///
/// Usage example:
/// @code{.cpp}
/// // create a single alias instead of specifying Decimal everywhere
/// using Money = decimal64::Decimal<4, decimal64::HalfEvenRoundPolicy>;
///
/// std::vector<std::string> cart = ...;
/// Money sum{0};
/// for (const std::string& cost_string : cart) {
///   // or use FromStringPermissive to enable rounding
///   sum += Money{cost_string};
/// }
/// return ToString(sum);
/// @endcode
template <int Prec, typename RoundPolicy_ = DefRoundPolicy>
class Decimal {
 public:
  /// The number of fractional digits
  static constexpr int kDecimalPoints = Prec;

  /// Specifies how to round in lossy operations
  using RoundPolicy = RoundPolicy_;

  /// The denominator of the decimal fraction
  static constexpr int64_t kDecimalFactor = kPow10<Prec>;

  /// Zero by default
  constexpr Decimal() noexcept : value_(0) {}

  /// @brief Convert from an integer
  template <typename T, impl::EnableIfInt<T> = 0>
  explicit constexpr Decimal(T value) : Decimal(FromIntegerImpl(value)) {}

  /// @brief Convert from a string
  ///
  /// The string must match the following regexp exactly:
  ///
  ///     [+-]?\d+(\.\d+)?
  ///
  /// No extra characters, including spaces, are allowed. Extra leading
  /// and trailing zeros (within `Prec`) are discarded. Input containing more
  /// fractional digits that `Prec` is not allowed (no implicit rounding).
  ///
  /// @throw decimal64::ParseError on invalid input
  /// @see FromStringPermissive
  explicit constexpr Decimal(std::string_view value);

  /// @brief Lossy conversion from a floating-point number
  ///
  /// To somewhat resist the accumulated error, the number is always rounded
  /// to the nearest Decimal, regardless of `RoundPolicy`.
  ///
  /// @warning Prefer storing and sending `Decimal` as string, and performing
  /// the computations between `Decimal`s.
  template <typename T>
  static constexpr Decimal FromFloatInexact(T value) {
    static_assert(std::is_floating_point_v<T>);
    return FromUnbiased(DefRoundPolicy::Round(static_cast<long double>(value) *
                                              kDecimalFactor));
  }

  /// @brief Convert from a string, allowing rounding, spaces and boundary dot
  ///
  /// In addition to the `Decimal(str)` constructor, allows:
  /// - rounding (as per `RoundPolicy`), e.g. "12.3456789" with `Prec == 2`
  /// - space characters, e.g. " \t42  \n"
  /// - leading and trailing dot, e.g. "5." and ".5"
  ///
  /// @throw decimal64::ParseError on invalid input
  /// @see Decimal(std::string_view)
  static constexpr Decimal FromStringPermissive(std::string_view value);

  /// @brief Reconstruct from the internal representation, as acquired
  /// with `AsUnbiased`
  ///
  /// The Decimal value will be equal to `value/kDecimalFactor`.
  ///
  /// @see AsUnbiased
  static constexpr Decimal FromUnbiased(int64_t value) noexcept {
    Decimal result;
    result.value_ = value;
    return result;
  }

  /// @brief Convert from `original_unbiased * 10^original_precision`, rounding
  /// according to `RoundPolicy` if necessary
  ///
  /// Usage examples:
  ///
  ///     Decimal<4>::FromBiased(123, 6) -> 0.0001
  ///     Decimal<4>::FromBiased(123, 2) -> 1.23
  ///     Decimal<4>::FromBiased(123, -1) -> 1230
  ///
  /// @param original_unbiased The original mantissa
  /// @param original_precision The original precision (negated exponent)
  static constexpr Decimal FromBiased(int64_t original_unbiased,
                                      int original_precision) {
    const int exponent_for_pack = Prec - original_precision;

    if (exponent_for_pack >= 0) {
      return FromUnbiased(original_unbiased * Pow10(exponent_for_pack));
    } else {
      int64_t new_value{};

      if (!RoundPolicy::DivRounded(new_value, original_unbiased,
                                   Pow10(-exponent_for_pack))) {
        new_value = 0;
      }

      return FromUnbiased(new_value);
    }
  }

  /// @brief Assignment from another `Decimal`
  ///
  /// The assignment is allowed as long as `RoundPolicy` is the same. Rounding
  /// will be performed according to `RoundPolicy` if necessary.
  template <int Prec2>
  Decimal& operator=(Decimal<Prec2, RoundPolicy> rhs) {
    impl::CheckPrecCast<Prec2, Prec>();
    if constexpr (Prec2 <= Prec) {
      value_ = rhs.value_ * kPow10<Prec - Prec2>;
    } else {
      if (!RoundPolicy::DivRounded(value_, rhs.value_, kPow10<Prec2 - Prec>)) {
        value_ = 0;
      }
    }
    return *this;
  }

  /// @brief Assignment from an integer
  template <typename T, typename = impl::EnableIfInt<T>>
  constexpr Decimal& operator=(T rhs) {
    return *this = Decimal{rhs};
  }

  constexpr bool operator==(Decimal rhs) const { return value_ == rhs.value_; }

  constexpr bool operator!=(Decimal rhs) const { return value_ != rhs.value_; }

  constexpr bool operator<(Decimal rhs) const { return value_ < rhs.value_; }

  constexpr bool operator<=(Decimal rhs) const { return value_ <= rhs.value_; }

  constexpr bool operator>(Decimal rhs) const { return value_ > rhs.value_; }

  constexpr bool operator>=(Decimal rhs) const { return value_ >= rhs.value_; }

  constexpr Decimal operator+(Decimal rhs) const {
    Decimal result = *this;
    result.value_ += rhs.value_;
    return result;
  }

  template <int Prec2>
  constexpr Decimal operator+(Decimal<Prec2, RoundPolicy> rhs) const {
    Decimal result = *this;
    result += rhs;
    return result;
  }

  constexpr Decimal& operator+=(Decimal rhs) {
    value_ += rhs.value_;
    return *this;
  }

  template <int Prec2>
  constexpr Decimal& operator+=(Decimal<Prec2, RoundPolicy> rhs) {
    impl::CheckPrecCast<Prec2, Prec>();
    if constexpr (Prec2 <= Prec) {
      value_ += rhs.value_ * kPow10<Prec - Prec2>;
    } else {
      int64_t val{};
      if (!RoundPolicy::DivRounded(val, rhs.value_, kPow10<Prec2 - Prec>)) {
        val = 0;
      }
      value_ += val;
    }
    return *this;
  }

  constexpr Decimal operator+() const { return *this; }

  constexpr Decimal operator-() const {
    Decimal result = *this;
    result.value_ = -result.value_;
    return result;
  }

  constexpr Decimal operator-(Decimal rhs) const {
    Decimal result = *this;
    result.value_ -= rhs.value_;
    return result;
  }

  template <int Prec2>
  constexpr Decimal operator-(Decimal<Prec2, RoundPolicy> rhs) const {
    Decimal result = *this;
    result -= rhs;
    return result;
  }

  constexpr Decimal& operator-=(Decimal rhs) {
    value_ -= rhs.value_;
    return *this;
  }

  template <int Prec2>
  constexpr Decimal& operator-=(Decimal<Prec2, RoundPolicy> rhs) {
    impl::CheckPrecCast<Prec2, Prec>();
    if (Prec2 <= Prec) {
      value_ -= rhs.value_ * kPow10<Prec - Prec2>;
    } else {
      int64_t val{};
      if (!RoundPolicy::DivRounded(val, rhs.value_, kPow10<Prec2 - Prec>)) {
        val = 0;
      }
      value_ -= val;
    }
    return *this;
  }

  template <typename T, typename = impl::EnableIfInt<T>>
  constexpr Decimal operator*(T rhs) const {
    Decimal result = *this;
    result.value_ *= rhs;
    return result;
  }

  constexpr Decimal operator*(Decimal rhs) const {
    Decimal result = *this;
    result *= rhs;
    return result;
  }

  template <int Prec2>
  constexpr Decimal operator*(Decimal<Prec2, RoundPolicy> rhs) const {
    Decimal result = *this;
    result *= rhs;
    return result;
  }

  template <typename T, typename = impl::EnableIfInt<T>>
  constexpr Decimal& operator*=(T rhs) {
    value_ *= rhs;
    return *this;
  }

  constexpr Decimal& operator*=(Decimal rhs) {
    value_ = impl::MultDiv<RoundPolicy>(value_, rhs.value_, kPow10<Prec>);
    return *this;
  }

  template <int Prec2>
  constexpr Decimal& operator*=(Decimal<Prec2, RoundPolicy> rhs) {
    impl::CheckPrecCast<Prec2, Prec>();
    value_ = impl::MultDiv<RoundPolicy>(value_, rhs.value_, kPow10<Prec2>);
    return *this;
  }

  template <typename T, typename = impl::EnableIfInt<T>>
  constexpr Decimal operator/(T rhs) const {
    Decimal result = *this;
    result /= rhs;
    return result;
  }

  constexpr Decimal operator/(Decimal rhs) const {
    Decimal result = *this;
    result /= rhs;
    return result;
  }

  template <int Prec2>
  constexpr Decimal operator/(Decimal<Prec2, RoundPolicy> rhs) const {
    Decimal result = *this;
    result /= rhs;
    return result;
  }

  template <typename T, typename = impl::EnableIfInt<T>>
  constexpr Decimal& operator/=(T rhs) {
    if (!RoundPolicy::DivRounded(value_, value_, rhs)) {
      value_ = impl::MultDiv<RoundPolicy>(value_, 1, rhs);
    }
    return *this;
  }

  constexpr Decimal& operator/=(Decimal rhs) {
    value_ = impl::MultDiv<RoundPolicy>(value_, kPow10<Prec>, rhs.value_);
    return *this;
  }

  template <int Prec2>
  constexpr Decimal& operator/=(Decimal<Prec2, RoundPolicy> rhs) {
    impl::CheckPrecCast<Prec2, Prec>();
    value_ = impl::MultDiv<RoundPolicy>(value_, kPow10<Prec2>, rhs.value_);
    return *this;
  }

  /// Returns one of {-1, 0, +1}, depending on the sign of the `Decimal`
  constexpr int Sign() const {
    return (value_ > 0) ? 1 : ((value_ < 0) ? -1 : 0);
  }

  /// Returns the absolute value of the `Decimal`
  constexpr Decimal Abs() const { return FromUnbiased(impl::Abs(value_)); }

  /// Returns the value rounded to integer using the active rounding policy
  constexpr int64_t ToInteger() const {
    int64_t result{};
    if (!RoundPolicy::DivRounded(result, value_, kDecimalFactor)) {
      result = 0;
    }
    return result;
  }

  /// @brief Returns the value converted to `double`
  ///
  /// @warning Operations with `double`, and even the returned value,
  /// is inexact. Prefer storing and sending `Decimal` as string, and performing
  /// the computations between `Decimal`s.
  ///
  /// @see FromFloatInexact
  constexpr double ToDoubleInexact() const {
    return static_cast<double>(value_) / kDecimalFactor;
  }

  /// @brief Retrieve the internal representation
  ///
  /// The internal representation of `Decimal` is `real_value * kDecimalFactor`.
  /// Use for storing the value of Decimal efficiently when `Prec` is guaranteed
  /// not to change.
  ///
  /// @see FromUnbiased
  constexpr int64_t AsUnbiased() const { return value_; }

 private:
  template <typename T>
  static constexpr Decimal FromIntegerImpl(T value) {
    return FromUnbiased(static_cast<int64_t>(value) * kDecimalFactor);
  }

  int64_t value_;
};

namespace impl {

template <typename T>
struct IsDecimal : std::false_type {};

template <int Prec, typename RoundPolicy>
struct IsDecimal<Decimal<Prec, RoundPolicy>> : std::true_type {};

}  // namespace impl

/// `true` if the type is an instantiation of `Decimal`
template <typename T>
inline constexpr bool kIsDecimal = impl::IsDecimal<T>::value;

/// @brief Cast one `Decimal` to another `Decimal` type
///
/// When casting to a `Decimal` with a lower `Prec`, rounding is performed
/// according to the new `RoundPolicy`.
///
/// Usage example:
/// @code{.cpp}
/// using Money = decimal64::Decimal<4>;
/// using Discount = decimal64::Decimal<4, FloorRoundPolicy>;
///
/// Money cost = ...;
/// auto discount = decimal64::decimal_cast<Discount>(cost) * Discount{"0.05"};
/// @endcode
template <typename T, int OldPrec, typename OldRound>
constexpr T decimal_cast(Decimal<OldPrec, OldRound> arg) {
  static_assert(kIsDecimal<T>);
  return T::FromBiased(arg.AsUnbiased(), OldPrec);
}

/// @brief Cast to a `Decimal` type with another `Prec`
///
/// Typically used when rounding, sometimes together with
/// `ToStringTrailingZeros`.
///
/// Usage example:
///
///     decimal64::decimal_cast<2>(higher_precision)
template <int Prec, int OldPrec, typename Round>
constexpr Decimal<Prec, Round> decimal_cast(Decimal<OldPrec, Round> arg) {
  return Decimal<Prec, Round>::FromBiased(arg.AsUnbiased(), OldPrec);
}

/// The base class for all errors related to parsing `Decimal` from string
class ParseError : public std::runtime_error {
 public:
  ParseError(std::string message);
};

namespace impl {

// FromUnpacked<Decimal<4>>(12, 34) -> 12.0034
// FromUnpacked<Decimal<4>>(-12, -34) -> -12.0034
// FromUnpacked<Decimal<4>>(0, -34) -> -0.0034
template <int Prec, typename RoundPolicy>
constexpr Decimal<Prec, RoundPolicy> FromUnpacked(int64_t before,
                                                  int64_t after) {
  using Dec = Decimal<Prec, RoundPolicy>;
  UASSERT(((before >= 0) && (after >= 0)) || ((before <= 0) && (after <= 0)));
  UASSERT(after > -Dec::kDecimalFactor && after < Dec::kDecimalFactor);

  if constexpr (Prec > 0) {
    return Dec::FromUnbiased(before * Dec::kDecimalFactor + after);
  } else {
    return Dec::FromUnbiased(before * Dec::kDecimalFactor);
  }
}

// FromUnpacked<Decimal<4>>(12, 34, 3) -> 12.034
template <int Prec, typename RoundPolicy>
constexpr Decimal<Prec, RoundPolicy> FromUnpacked(int64_t before, int64_t after,
                                                  int original_precision) {
  using Dec = Decimal<Prec, RoundPolicy>;
  UASSERT(((before >= 0) && (after >= 0)) || ((before <= 0) && (after <= 0)));
  UASSERT(after > -Pow10(original_precision) &&
          after < Pow10(original_precision));

  if (original_precision <= Prec) {
    // direct mode
    const int missing_digits = Prec - original_precision;
    const int64_t factor = Pow10(missing_digits);
    return FromUnpacked<Prec, RoundPolicy>(before, after * factor);
  } else {
    // rounding mode
    const int extra_digits = original_precision - Prec;
    const int64_t factor = Pow10(extra_digits);
    int64_t rounded_after{};
    if (!RoundPolicy::DivRounded(rounded_after, after, factor)) {
      return Dec{0};
    }
    return FromUnpacked<Prec, RoundPolicy>(before, rounded_after);
  }
}

struct UnpackedDecimal {
  int64_t before;
  int64_t after;
};

// AsUnpacked(Decimal<4>{"3.14"}) -> {3, 1400}
// AsUnpacked(Decimal<4>{"-3.14"}) -> {-3, -1400}
// AsUnpacked(Decimal<4>{"-0.14"}) -> {0, -1400}
template <int Prec, typename RoundPolicy>
constexpr UnpackedDecimal AsUnpacked(Decimal<Prec, RoundPolicy> dec) {
  using Dec = Decimal<Prec, RoundPolicy>;
  return {dec.AsUnbiased() / Dec::kDecimalFactor,
          dec.AsUnbiased() % Dec::kDecimalFactor};
}

template <typename CharT>
constexpr bool IsSpace(CharT c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v';
}

template <typename CharT, typename Traits>
class StringCharSequence {
 public:
  explicit constexpr StringCharSequence(
      std::basic_string_view<CharT, Traits> sv)
      : current_(sv.begin()), end_(sv.end()) {}

  // on sequence end, returns '\0'
  constexpr CharT Get() { return current_ == end_ ? CharT{'\0'} : *current_++; }

  constexpr void Unget() { --current_; }

 private:
  typename std::basic_string_view<CharT, Traits>::iterator current_;
  typename std::basic_string_view<CharT, Traits>::iterator end_;
};

template <typename CharT, typename Traits>
class StreamCharSequence {
 public:
  explicit StreamCharSequence(std::basic_istream<CharT, Traits>& in)
      : in_(&in) {}

  // on sequence end, returns '\0'
  CharT Get() {
    constexpr CharT kEof =
        std::basic_istream<CharT, Traits>::traits_type::eof();
    if (!in_->good()) {
      return CharT{'\0'};
    }
    const CharT c = in_->peek();
    if (c == kEof) {
      return CharT{'\0'};
    }
    in_->ignore();
    return c;
  }

  void Unget() { in_->unget(); }

 private:
  std::basic_istream<CharT, Traits>* in_;
};

enum class ParseOptions {
  kNone = 0,

  /// Allow space characters in the beginning or in the end
  /// " 42  "
  kAllowSpaces = 1 << 0,

  /// Allow any trailing characters
  /// "42ABC"
  kAllowTrailingJunk = 1 << 1,

  /// Allow leading or trailing dot
  /// "42.", ".42"
  kAllowBoundaryDot = 1 << 2,

  /// Allow decimal digits beyond Prec, round according to RoundPolicy
  /// "0.123456" -> "0.1234" or "0.1235"
  kAllowRounding = 1 << 3
};

enum class ParseErrorCode : uint8_t {
  /// An unexpected character has been met
  kWrongChar,

  /// No digits before or after dot
  kNoDigits,

  /// The integral part does not fit in a Decimal
  kOverflow,

  /// The string contains leading spaces, while disallowed by options
  kSpace,

  /// The string contains trailing junk, while disallowed by options
  kTrailingJunk,

  /// On inputs like "42." or ".42" if disallowed by options
  kBoundaryDot,

  /// When there are more decimal digits than in any Decimal and rounding is
  /// disallowed by options
  kRounding,
};

struct ParseUnpackedResult {
  int64_t before;
  int64_t after;
  uint8_t decimal_digits;
  bool is_negative;
  std::optional<ParseErrorCode> error;
  uint32_t error_position;
};

enum class ParseState {
  /// Before reading any part of the Decimal
  kSign,

  /// After reading a sign
  kBeforeFirstDig,

  /// Only leading zeros (at least one) have been met
  kLeadingZeros,

  /// At least one digit before dot has been met
  kBeforeDec,

  /// Reading fractional digits
  kAfterDec,

  /// Reading and rounding extra fractional digits
  kIgnoringAfterDec,

  /// A character unrelated to the Decimal has been met
  kEnd
};

/// Extract values from a CharSequence ready to be packed to Decimal
template <typename CharSequence>
[[nodiscard]] constexpr ParseUnpackedResult ParseUnpacked(
    CharSequence input, utils::Flags<ParseOptions> options) {
  constexpr char dec_point = '.';

  int64_t before = 0;
  int64_t after = 0;
  bool is_negative = false;

  ptrdiff_t position = -1;
  auto state = ParseState::kSign;
  std::optional<ParseErrorCode> error;
  int before_digit_count = 0;
  uint8_t after_digit_count = 0;

  while (state != ParseState::kEnd) {
    const auto c = input.Get();
    if (c == '\0') break;
    if (!error) ++position;

    switch (state) {
      case ParseState::kSign:
        if (c == '-') {
          is_negative = true;
          state = ParseState::kBeforeFirstDig;
        } else if (c == '+') {
          state = ParseState::kBeforeFirstDig;
        } else if (c == '0') {
          state = ParseState::kLeadingZeros;
          before_digit_count = 1;
        } else if ((c >= '1') && (c <= '9')) {
          state = ParseState::kBeforeDec;
          before = static_cast<int>(c - '0');
          before_digit_count = 1;
        } else if (c == dec_point) {
          if (!(options & ParseOptions::kAllowBoundaryDot) && !error) {
            error = ParseErrorCode::kBoundaryDot;  // keep reading digits
          }
          state = ParseState::kAfterDec;
        } else if (IsSpace(c)) {
          if (!(options & ParseOptions::kAllowSpaces)) {
            state = ParseState::kEnd;
            error = ParseErrorCode::kSpace;
          }
        } else {
          state = ParseState::kEnd;
          error = ParseErrorCode::kWrongChar;
        }
        break;
      case ParseState::kBeforeFirstDig:
        if (c == '0') {
          state = ParseState::kLeadingZeros;
          before_digit_count = 1;
        } else if ((c >= '1') && (c <= '9')) {
          state = ParseState::kBeforeDec;
          before = static_cast<int>(c - '0');
          before_digit_count = 1;
        } else if (c == dec_point) {
          if (!(options & ParseOptions::kAllowBoundaryDot) && !error) {
            error = ParseErrorCode::kBoundaryDot;  // keep reading digits
          }
          state = ParseState::kAfterDec;
        } else {
          state = ParseState::kEnd;
          error = ParseErrorCode::kWrongChar;
        }
        break;
      case ParseState::kLeadingZeros:
        if (c == '0') {
          // skip
        } else if ((c >= '1') && (c <= '9')) {
          state = ParseState::kBeforeDec;
          before = static_cast<int>(c - '0');
        } else if (c == dec_point) {
          state = ParseState::kAfterDec;
        } else {
          state = ParseState::kEnd;
        }
        break;
      case ParseState::kBeforeDec:
        if ((c >= '0') && (c <= '9')) {
          if (before_digit_count < kMaxDecimalDigits) {
            before = 10 * before + static_cast<int>(c - '0');
            before_digit_count++;
          } else if (!error) {
            error = ParseErrorCode::kOverflow;  // keep reading digits
          }
        } else if (c == dec_point) {
          state = ParseState::kAfterDec;
        } else {
          state = ParseState::kEnd;
        }
        break;
      case ParseState::kAfterDec:
        if ((c >= '0') && (c <= '9')) {
          if (after_digit_count < kMaxDecimalDigits) {
            after = 10 * after + static_cast<int>(c - '0');
            after_digit_count++;
          } else {
            if (!(options & ParseOptions::kAllowRounding) && !error) {
              error = ParseErrorCode::kRounding;  // keep reading digits
            }
            state = ParseState::kIgnoringAfterDec;
            if (c >= '5') {
              // round half up
              after++;
            }
          }
        } else {
          if (!(options & ParseOptions::kAllowBoundaryDot) &&
              after_digit_count == 0 && !error) {
            error = ParseErrorCode::kBoundaryDot;
          }
          state = ParseState::kEnd;
        }
        break;
      case ParseState::kIgnoringAfterDec:
        if ((c >= '0') && (c <= '9')) {
          // skip
        } else {
          state = ParseState::kEnd;
        }
        break;
      case ParseState::kEnd:
        UASSERT(false);
        break;
    }  // switch state
  }    // while has more chars & not end

  if (state == ParseState::kEnd) {
    input.Unget();

    if (!error && !(options & ParseOptions::kAllowTrailingJunk)) {
      if (!(options & ParseOptions::kAllowSpaces)) {
        error = ParseErrorCode::kSpace;
      }
      --position;

      while (true) {
        const auto c = input.Get();
        if (c == '\0') break;
        ++position;
        if (!IsSpace(c)) {
          error = ParseErrorCode::kTrailingJunk;
          input.Unget();
          break;
        }
      }
    }
  }

  if (!error && before_digit_count == 0 && after_digit_count == 0) {
    error = ParseErrorCode::kNoDigits;
  }

  if (!error && state == ParseState::kAfterDec &&
      !(options & ParseOptions::kAllowBoundaryDot) && after_digit_count == 0) {
    error = ParseErrorCode::kBoundaryDot;
  }

  return {before,      after, after_digit_count,
          is_negative, error, static_cast<uint32_t>(position)};
}

template <int Prec, typename RoundPolicy>
struct ParseResult {
  Decimal<Prec, RoundPolicy> decimal;
  std::optional<ParseErrorCode> error;
  uint32_t error_position;
};

/// Parse Decimal from a CharSequence
template <int Prec, typename RoundPolicy, typename CharSequence>
[[nodiscard]] constexpr ParseResult<Prec, RoundPolicy> Parse(
    CharSequence input, utils::Flags<ParseOptions> options) {
  ParseUnpackedResult parsed = ParseUnpacked(input, options);

  if (parsed.error) {
    return {{}, parsed.error, parsed.error_position};
  }

  if (parsed.before >= kMaxInt64 / kPow10<Prec>) {
    return {{}, ParseErrorCode::kOverflow, 0};
  }

  if (!(options & ParseOptions::kAllowRounding) &&
      parsed.decimal_digits > Prec) {
    return {{}, ParseErrorCode::kRounding, 0};
  }

  if (parsed.is_negative) {
    parsed.before = -parsed.before;
    parsed.after = -parsed.after;
  }

  return {FromUnpacked<Prec, RoundPolicy>(parsed.before, parsed.after,
                                          parsed.decimal_digits),
          {},
          0};
}

std::string GetErrorMessage(std::string_view source, std::string_view path,
                            size_t position, ParseErrorCode reason);

/// Returns the number of zeros trimmed
template <int Prec>
int TrimTrailingZeros(int64_t& after) {
  if constexpr (Prec == 0) {
    return 0;
  }
  if (after == 0) {
    return Prec;
  }

  int n_trimmed = 0;
  if constexpr (Prec >= 17) {
    if (after % kPow10<16> == 0) {
      after /= kPow10<16>;
      n_trimmed += 16;
    }
  }
  if constexpr (Prec >= 9) {
    if (after % kPow10<8> == 0) {
      after /= kPow10<8>;
      n_trimmed += 8;
    }
  }
  if constexpr (Prec >= 5) {
    if (after % kPow10<4> == 0) {
      after /= kPow10<4>;
      n_trimmed += 4;
    }
  }
  if constexpr (Prec >= 3) {
    if (after % kPow10<2> == 0) {
      after /= kPow10<2>;
      n_trimmed += 2;
    }
  }
  if (after % kPow10<1> == 0) {
    after /= kPow10<1>;
    n_trimmed += 1;
  }
  return n_trimmed;
}

}  // namespace impl

template <int Prec, typename RoundPolicy>
constexpr Decimal<Prec, RoundPolicy>::Decimal(std::string_view value) {
  const auto result = impl::Parse<Prec, RoundPolicy>(
      impl::StringCharSequence(value), impl::ParseOptions::kNone);

  if (result.error) {
    throw ParseError(impl::GetErrorMessage(
        value, "<string>", result.error_position, *result.error));
  }
  *this = result.decimal;
}

template <int Prec, typename RoundPolicy>
constexpr Decimal<Prec, RoundPolicy>
Decimal<Prec, RoundPolicy>::FromStringPermissive(std::string_view input) {
  const auto result = impl::Parse<Prec, RoundPolicy>(
      impl::StringCharSequence(input),
      {impl::ParseOptions::kAllowSpaces, impl::ParseOptions::kAllowBoundaryDot,
       impl::ParseOptions::kAllowRounding});

  if (result.error) {
    throw ParseError(impl::GetErrorMessage(
        input, "<string>", result.error_position, *result.error));
  }
  return result.decimal;
}

/// @brief Converts Decimal to a string
///
/// Usage example:
///
///     ToString(decimal64::Decimal<4>{"1.5"}) -> 1.5
///
/// @see ToStringTrailingZeros
template <int Prec, typename RoundPolicy>
std::string ToString(Decimal<Prec, RoundPolicy> dec) {
  return fmt::format(FMT_COMPILE("{}"), dec);
}

/// @brief Converts Decimal to a string, writing exactly `Prec` decimal digits
///
/// Usage example:
///
///     ToStringTrailingZeros(decimal64::Decimal<4>{"1.5"}) -> 1.5000
///
/// @see ToString
template <int Prec, typename RoundPolicy>
std::string ToStringTrailingZeros(Decimal<Prec, RoundPolicy> dec) {
  return fmt::format(FMT_COMPILE("{:f}"), dec);
}

/// @brief Parses a `Decimal` from the `istream`
///
/// Acts like the `Decimal(str)` constructor, except that it allows junk that
/// immediately follows the number. Sets the stream's fail bit on failure.
///
/// Usage example:
///
///     if (os >> dec) {
///       // success
///     } else {
///       // failure
///     }
///
/// @see Decimal::Decimal(std::string_view)
template <typename CharT, typename Traits, int Prec, typename RoundPolicy>
std::basic_istream<CharT, Traits>& operator>>(
    std::basic_istream<CharT, Traits>& is, Decimal<Prec, RoundPolicy>& d) {
  if (is.flags() & std::ios_base::skipws) {
    std::ws(is);
  }
  const auto result = impl::Parse<Prec, RoundPolicy>(
      impl::StreamCharSequence(is), {impl::ParseOptions::kAllowTrailingJunk});

  if (result.error) {
    is.setstate(std::ios_base::failbit);
  } else {
    d = result.decimal;
  }
  return is;
}

/// @brief Writes the `Decimal` to the `ostream`
/// @see ToString
template <typename CharT, typename Traits, int Prec, typename RoundPolicy>
std::basic_ostream<CharT, Traits>& operator<<(
    std::basic_ostream<CharT, Traits>& os,
    const Decimal<Prec, RoundPolicy>& d) {
  os << ToString(d);
  return os;
}

/// @brief Writes the `Decimal` to the logger
/// @see ToString
template <int Prec, typename RoundPolicy>
logging::LogHelper& operator<<(logging::LogHelper& lh,
                               const Decimal<Prec, RoundPolicy>& d) {
  lh << ToString(d);
  return lh;
}

/// @brief Parses the `Decimal` from the string
/// @see Decimal::Decimal(std::string_view)
template <int Prec, typename RoundPolicy, typename Value>
std::enable_if_t<formats::common::kIsFormatValue<Value>,
                 Decimal<Prec, RoundPolicy>>
Parse(const Value& value, formats::parse::To<Decimal<Prec, RoundPolicy>>) {
  const std::string input = value.template As<std::string>();

  const auto result = impl::Parse<Prec, RoundPolicy>(
      impl::StringCharSequence(std::string_view{input}),
      impl::ParseOptions::kNone);

  if (result.error) {
    throw ParseError(impl::GetErrorMessage(
        input, value.GetPath(), result.error_position, *result.error));
  }
  return result.decimal;
}

/// @brief Serializes the `Decimal` to string
/// @see ToString
template <int Prec, typename RoundPolicy, typename TargetType>
TargetType Serialize(const Decimal<Prec, RoundPolicy>& object,
                     formats::serialize::To<TargetType>) {
  return typename TargetType::Builder(ToString(object)).ExtractValue();
}

/// @brief Writes the `Decimal` to stream
/// @see ToString
template <int Prec, typename RoundPolicy, typename StringBuilder>
void WriteToStream(const Decimal<Prec, RoundPolicy>& object,
                   StringBuilder& sw) {
  WriteToStream(ToString(object), sw);
}

}  // namespace decimal64

/// std::hash support
template <int Prec, typename RoundPolicy>
struct std::hash<decimal64::Decimal<Prec, RoundPolicy>> {
  std::size_t operator()(const decimal64::Decimal<Prec, RoundPolicy>& v) const
      noexcept {
    return std::hash<int64_t>{}(v.AsUnbiased());
  }
};

/// @brief fmt support
///
/// Spec format:
/// - {} trims any trailing zeros;
/// - {:f} writes exactly `Prec` decimal digits, including trailing zeros
///   if needed.
template <int Prec, typename RoundPolicy, typename Char>
class fmt::formatter<decimal64::Decimal<Prec, RoundPolicy>, Char> {
  // TODO TAXICOMMON-2916 Add support for formatting Decimal with custom
  //  precision

 public:
  constexpr auto parse(fmt::format_parse_context& ctx) {
    auto it = ctx.begin();
    const auto end = ctx.end();

    if (it != end && *it == 'f') {
      remove_trailing_zeros_ = false;
      ++it;
    }

    if (it != end && *it != '}') {
      throw format_error("invalid format");
    }

    return it;
  }

  template <typename FormatContext>
  auto format(const decimal64::Decimal<Prec, RoundPolicy>& dec,
              FormatContext& ctx) {
    auto [before, after] = decimal64::impl::AsUnpacked(dec);
    int after_digits = Prec;

    if (remove_trailing_zeros_) {
      after_digits -= decimal64::impl::TrimTrailingZeros<Prec>(after);
    }

    if (after_digits > 0) {
      if (dec.Sign() == -1) {
        return fmt::format_to(ctx.out(), FMT_STRING("-{}.{:0{}}"), -before,
                              -after, after_digits);
      } else {
        return fmt::format_to(ctx.out(), FMT_STRING("{}.{:0{}}"), before, after,
                              after_digits);
      }
    } else {
      return fmt::format_to(ctx.out(), FMT_COMPILE("{}"), before);
    }
  }

 private:
  bool remove_trailing_zeros_ = true;
};