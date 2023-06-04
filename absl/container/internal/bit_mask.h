#ifndef ABSL_CONTAINER_INTERNAL_BIT_MASK_H_
#define ABSL_CONTAINER_INTERNAL_BIT_MASK_H_

#include <cstdint>
#include <type_traits>

#include "absl/base/config.h"
#include "absl/base/optimization.h"
#include "absl/numeric/bits.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

template <typename T>
uint32_t TrailingZeros(T x) {
  ABSL_ASSUME(x != 0);
  return static_cast<uint32_t>(countr_zero(x));
}

// An abstract bitmask, such as that emitted by a SIMD instruction.
//
// Specifically, this type implements a simple bitset whose representation is
// controlled by `SignificantBits` and `Shift`. `SignificantBits` is the number
// of abstract bits in the bitset, while `Shift` is the log-base-two of the
// width of an abstract bit in the representation.
// This mask provides operations for any number of real bits set in an abstract
// bit. To add iteration on top of that, implementation must guarantee no more
// than one real bit is set in an abstract bit.
template <class T, int SignificantBits, int Shift = 0>
class NonIterableBitMask {
 public:
  explicit NonIterableBitMask(T mask) : mask_(mask) {}

  explicit operator bool() const { return this->mask_ != 0; }

  // Returns the index of the lowest *abstract* bit set in `self`.
  uint32_t LowestBitSet() const {
    return container_internal::TrailingZeros(mask_) >> Shift;
  }

  // Returns the index of the highest *abstract* bit set in `self`.
  uint32_t HighestBitSet() const {
    return static_cast<uint32_t>((bit_width(mask_) - 1) >> Shift);
  }

  // Return the number of trailing zero *abstract* bits.
  uint32_t TrailingZeros() const {
    return container_internal::TrailingZeros(mask_) >> Shift;
  }

  // Return the number of leading zero *abstract* bits.
  uint32_t LeadingZeros() const {
    constexpr int total_significant_bits = SignificantBits << Shift;
    constexpr int extra_bits = sizeof(T) * 8 - total_significant_bits;
    return static_cast<uint32_t>(countl_zero(mask_ << extra_bits)) >> Shift;
  }

  T mask_;
};

// Mask that can be iterable
//
// For example, when `SignificantBits` is 16 and `Shift` is zero, this is just
// an ordinary 16-bit bitset occupying the low 16 bits of `mask`. When
// `SignificantBits` is 8 and `Shift` is 3, abstract bits are represented as
// the bytes `0x00` and `0x80`, and it occupies all 64 bits of the bitmask.
//
// For example:
//   for (int i : BitMask<uint32_t, 16>(0b101)) -> yields 0, 2
//   for (int i : BitMask<uint64_t, 8, 3>(0x0000000080800000)) -> yields 2, 3
template <class T, int SignificantBits, int Shift = 0>
class BitMask : public NonIterableBitMask<T, SignificantBits, Shift> {
  using Base = NonIterableBitMask<T, SignificantBits, Shift>;
  static_assert(std::is_unsigned<T>::value, "");
  static_assert(Shift == 0 || Shift == 3, "");

 public:
  explicit BitMask(T mask) : Base(mask) {}
  // BitMask is an iterator over the indices of its abstract bits.
  using value_type = int;
  using iterator = BitMask;
  using const_iterator = BitMask;

  BitMask& operator++() {
    this->mask_ &= (this->mask_ - 1);
    return *this;
  }

  uint32_t operator*() const { return Base::LowestBitSet(); }

  BitMask begin() const { return *this; }
  BitMask end() const { return BitMask(0); }

 private:
  friend bool operator==(const BitMask& a, const BitMask& b) {
    return a.mask_ == b.mask_;
  }
  friend bool operator!=(const BitMask& a, const BitMask& b) {
    return a.mask_ != b.mask_;
  }
};
}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_CONTAINER_INTERNAL_BITMASK_H_
