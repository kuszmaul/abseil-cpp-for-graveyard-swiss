// Copyright 2018 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// An open-addressed hash table with implicit graveyard hashing.
//
// This is a low level hashtable on top of which different interfaces can be
// implemented, like flat_hash_set, node_hash_set, string_hash_set, etc.
//
// The table interface is similar to that of std::unordered_set. Notable
// differences are that most member functions support heterogeneous keys when
// BOTH the hash and eq functions are marked as transparent. They do so by
// providing a typedef called `is_transparent`.
//
// When heterogeneous lookup is enabled, functions that take key_type act as if
// they have an overload set like:
//
//   iterator find(const key_type& key);
//   template <class K>
//   iterator find(const K& key);
//
//   size_type erase(const key_type& key);
//   template <class K>
//   size_type erase(const K& key);
//
//   std::pair<iterator, iterator> equal_range(const key_type& key);
//   template <class K>
//   std::pair<iterator, iterator> equal_range(const K& key);
//
// When heterogeneous lookup is disabled, only the explicit `key_type` overloads
// exist.
//
// find() also supports passing the hash explicitly:
//
//   iterator find(const key_type& key, size_t hash);
//   template <class U>
//   iterator find(const U& key, size_t hash);
//
// In addition the pointer to element and iterator stability guarantees are
// weaker: all iterators and pointers are invalidated after a new element is
// inserted.
//
// IMPLEMENTATION DETAILS
//
// # Table Layout
//
// A graveyard_raw_hash_set's backing storage is an array of buckets.  Each
// bucket is a pseudo-struct:
//
//   struct Bucket {
//     // Usually SlotsPerBucket==14 .
//     ctrl_t ctrl[SlotsPerBucket];
//     uint16_t last_bucket : 1;
//     uint16_t search_distance : 15;
//     // slots may or may not contain objects.
//     slot_type slots[SlotsPerBucket];
//   };
//
// For very small tables the Bucket may be truncated (e.g., if capacity()==1,
// there is space for only one slot, although there are still 14 ctrl bytes.
// The unused ctrl bytes will always indicate "empty".
//
// The length of this array is computed by `AllocSize()` below.
//
// Control bytes (`ctrl_t`) are bytes that define the state of the corresponding
// slot in the slot array. Group manipulation is tightly optimized to be as
// efficient as possible: SSE and friends on x86, clever bit operations on other
// arches.
//
// Each control byte is either a special value for empty slots, (sometimes
// called *tombstones*), or else a value for full slots.  There is one value
// used for empty slots, 254 values used for full slots, and one unused values.
// Of the 254 values, 1 bit is used to indicate that the occupied slot may be
// out of order.
//
// We maintain occupied slots in hash order, as much as possible.  Given pointers
// to slots, `a` and `b`, in the same table (possibly in different buckets) with
// `a < b`, we say that the slots are properly ordered `hash(*a) <= hash(*b)`.
// After rehashing, all pairs of slots are properly ordered.  Newly inserted
// slots are not properly ordered.  The `ctrl` byte keeps track of which slots
// might be out of order.
//
// It turns out that since we wrap around at the end of the table, the ordering
// property is a little bit more complex.  The first few buckets may contain
// values that have very large hashes.  If a slot contains a value whose `H1` is
// greater than the bucket number, then we know that the value was wrapped
// around.
//
// There are no explicit tombstones, just empty slots.
//
// # Hashing
//
// We compute two separate hashes, `H1` and `H2`, from the hash of an object.
// `H1(hash(x))` is a bucket number, and essentially the starting point for the
// probe sequence. `H2(hash(x))` is a 7-bit value used to filter out objects
// that cannot possibly be the one we are looking for.
//
// We compute `H1` from the high order bits of `hash(x)` (see
// https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/).
//
// We compute `H2` (a value in the range `[0, 127)`) simply by writing `hash(v)
// % 127` and relying on the compiler to do a good job of modulo by a
// non-power-of-two constant.
//
// # Table operations.
//
// The key operations are `insert`, `find`, and `erase`.
//
// Since `insert` and `erase` are implemented in terms of `find`, we describe
// `find` first. To `find` a value `x`, we compute `hash(x)`. We start looking
// in bucket `H1(hash(x))`, doing linear probing, looking at subsequent buckets.
// We know, due to the graveyard hashing theory, that linear probing works well
// (assuming that the hash function is good).  Many implementors had assumed
// that linear probing works poorly because of Knuth's original result.  It's
// actually not so bad, and when combined with graveyard tombstones, linear
// probing is actually very good.
//
// We now walk through the buckets (wrapping around at the end).  At each index,
// we read all 14 bytes in the bucket and extract potential candidates: occupied
// slots with a H2 value equal to `H2(hash(x))`.  Each candidate slot `y` is
// compared with `x`; if `x == y`, we are done and return `&y`; otherwise we
// continue to the next probe index.
//
// The `H2` bits ensure when we compare a slot to an object with `==`, we are
// likely to have actually found the object.  That is, the chance is low that
// `==` is called and returns `false`.  Thus, when we search for an object, we
// are unlikely to call `==` many times.  This likelyhood can be analyzed as
// follows (assuming that H2 is a random enough hash function).
//
// Let's assume that there are `k` "wrong" objects that must be examined in a
// probe sequence.  For example, when doing a `find` on an object that is in the
// table, `k` is the number of objects between the start of the probe sequence
// and the final found object (not including the final found object).  The
// expected number of objects with an H2 match is then `k/127`.  Measurements
// and analysis indicate that even at high load factors, `k` is less than 32,
// meaning that the number of "false positive" comparisons we must perform is
// less than 1/8 per `find`.
//
// `insert` is implemented in terms of `unchecked_insert`, which inserts a value
// presumed to not be in the table (violating this requirement will cause the
// table to behave erratically). Given `x` and its hash `hash(x)`, to insert it,
// we construct a `probe_seq` once again, and use it to find the first group
// with an unoccupied slot. We place `x` into the first such slot in the group
// and mark it as full with `x`'s H2.
//
// To `insert`, we compose `unchecked_insert` with `find`. We compute `h(x)` and
// perform a `find` to see if it's already present; if it is, we're done. If
// it's not, we may decide the table is getting overcrowded (i.e. the load
// factor is greater than some constant (perhaps 7/8) for big tables;
// `is_small()` tables use a max load factor of 1); in this case, we allocate a
// bigger array, and move the values into the new array.  We do this move so
// that after the rehash, all the values in the table will be in hash order.
//
// We do this by scanning through the table, merging sorted values with the
// unsorted values.  When we encounter an unsorted value, we remember it in a
// heap data structure.  When we encounter a sorted value, if it's less than the
// smallest item in the heap, we place it in the destination table, otherwise we
// move the unsorted item to the destination.  Whenever we encounter a new
// bucket in the source table, we scan forward to find all the unsorted values
// (using the search_distance to limit how fare we have to look into the future.
//
// For example, for a table that is rehashed when it's 90% full, and we rehash
// it to be 80% full, only 10% of the items are out of order, and on average we
// need to examine just over one bucket, so the number of items in the heap is
// small (one or two).  For a table that's rehashed down to 50% full, it will
// turn out there are many unordered values.  It's probably better to simply run
// at a relatively high load factor, and rehash the table more often.
//
// The rehash also takes care to limit the high-water mark for memory: Since we
// are scanning the old table from left to right, and inserting into the new
// table from left to right, we don't actually need both tables to fully occupy
// RAM at the same time.  We can use `madvise(DONT_NEED)` to cause parts of the
// old table to have their physical memory deallocated as we fill in the new
// table.
//
// After moving everything from the old to the new backing storage, we can
// discard the old strorage.  At this point, we may `unchecked_insert` the value
// `x`.
//
// Below, `unchecked_insert` is partly implemented by `prepare_insert`, which
// presents a viable, initialized slot pointee to the caller.
//
// `erase` is implemented in terms of `erase_at`, which takes an index to a
// slot. Given an offset, we simply mark it empty and destroy its contents.  We
// don't bother to try to update the search distance, since that will be fixed
// up on the next rehash anyway.
//
// `erase` is `erase_at` composed with `find`: if we have a value `x`, we can
// perform a `find`, and then `erase_at` the resulting slot.
//
// To iterate, we simply traverse the buckets array, skipping empty slots and
// stopping when we have searched `search_distance` buckets.
//
// If we discover that the hash function is bad (because we are getting long
// probe sequences), which switch to simply using `std::unordered_set`.

#ifndef ABSL_CONTAINER_INTERNAL_GRAVEYARD_RAW_HASH_SET_H_
#define ABSL_CONTAINER_INTERNAL_GRAVEYARD_RAW_HASH_SET_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#include "absl/base/config.h"
#include "absl/base/internal/endian.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/optimization.h"
#include "absl/base/port.h"
#include "absl/base/prefetch.h"
#include "absl/container/internal/bit_mask.h"
#include "absl/container/internal/common.h"
#include "absl/container/internal/compressed_tuple.h"
#include "absl/container/internal/container_memory.h"
#include "absl/container/internal/hash_policy_traits.h"
#include "absl/container/internal/hashtable_debug_hooks.h"
#include "absl/container/internal/hashtablez_sampler.h"
#include "absl/container/internal/raw_traits.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/numeric/bits.h"
#include "absl/utility/utility.h"

#ifdef ABSL_INTERNAL_HAVE_SSE2
#include <emmintrin.h>
#endif

#ifdef ABSL_INTERNAL_HAVE_SSSE3
#include <tmmintrin.h>
#endif

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef ABSL_INTERNAL_HAVE_ARM_NEON
#include <arm_neon.h>
#endif

namespace absl {
ABSL_NAMESPACE_BEGIN

using absl::countl_zero;
using absl::countr_zero;
using absl::bit_width;
namespace little_endian {
using absl::little_endian::Load64;
using absl::little_endian::Store64;
}
namespace graveyard_container_internal {
using absl::container_internal::BitMask;
using absl::container_internal::Allocate;
using absl::container_internal::CommonAccess;
using absl::container_internal::Deallocate;
using absl::container_internal::IsDecomposable;
using absl::container_internal::IsNoThrowSwappable;
using absl::container_internal::IsTransparent;
using absl::container_internal::KeyArg;
using absl::container_internal::NonIterableBitMask;
using absl::container_internal::Sample;
using absl::container_internal::hash_policy_traits;
using absl::container_internal::HashtablezInfoHandle;
using absl::container_internal::InsertReturnType;
using absl::container_internal::node_handle;
using absl::container_internal::SanitizerPoisonMemoryRegion;
using absl::container_internal::SanitizerUnpoisonMemoryRegion;
using absl::container_internal::TrailingZeros;

#ifdef ABSL_SWISSTABLE_ENABLE_GENERATIONS
#error ABSL_SWISSTABLE_ENABLE_GENERATIONS cannot be directly set
#elif defined(ABSL_HAVE_ADDRESS_SANITIZER) || \
    defined(ABSL_HAVE_MEMORY_SANITIZER)
// When compiled in sanitizer mode, we add generation integers to the backing
// array and iterators. In the backing array, we store the generation between
// the control bytes and the slots. When iterators are dereferenced, we assert
// that the container has not been mutated in a way that could cause iterator
// invalidation since the iterator was initialized.
#define ABSL_SWISSTABLE_ENABLE_GENERATIONS
#endif

// We use uint8_t so we don't need to worry about padding.
using GenerationType = uint8_t;

// A sentinel value for empty generations. Using 0 makes it easy to constexpr
// initialize an array of this value.
constexpr GenerationType SentinelEmptyGeneration() { return 0; }

constexpr GenerationType NextGeneration(GenerationType generation) {
  return ++generation == SentinelEmptyGeneration() ? ++generation : generation;
}

#ifdef ABSL_SWISSTABLE_ENABLE_GENERATIONS
constexpr bool SwisstableGenerationsEnabled() { return true; }
constexpr size_t NumGenerationBytes() { return sizeof(GenerationType); }
#else
constexpr bool SwisstableGenerationsEnabled() { return false; }
constexpr size_t NumGenerationBytes() { return 0; }
#endif

template <typename AllocType>
void SwapAlloc(AllocType& lhs, AllocType& rhs,
               std::true_type /* propagate_on_container_swap */) {
  using std::swap;
  swap(lhs, rhs);
}
template <typename AllocType>
void SwapAlloc(AllocType& /*lhs*/, AllocType& /*rhs*/,
               std::false_type /* propagate_on_container_swap */) {}

// The state for a probe sequence.
//
// The sequence is simply a linear progression
//
//   p(i) := i mod bucket_count;
class probe_seq {
 public:
  // Creates a new probe sequence using `hash` as the initial value of the
  // sequence and `mask` (usually the capacity of the table) as the mask to
  // apply to each value in the progression.
  probe_seq(size_t h1, size_t bucket_count) :index_(h1), bucket_count_(bucket_count) {
  }

  void next() {
    ++index_;
    if (index_ == bucket_count_) {
      index_ = 0;
    }
  }
  // 0-based probe index, a multiple of `Width`.
  size_t index() const { return index_; }

 private:
  size_t index_ = 0;
  size_t bucket_count_;
};

using h2_t = uint8_t;

// The values here are selected for maximum performance. See the static asserts
// below for details.

// A `ctrl_t` is a single control byte, which can have one of four
// states: empty, deleted, full (which has an associated seven-bit h2_t value)
// and the sentinel. They have the following bit patterns:
//
//      empty: 1 0 0 0 0 0 0 0
//       full: 0 u h h h h h h  // h represents the hash bits.
//                              // u represented unordered
//
// These values are specifically tuned for SSE-flavored SIMD.
// The static_asserts below detail the source of these choices.
class ctrl_t {
 public:
  static constexpr uint8_t kEmpty = 0x80;

  ctrl_t(char value) :value_(value) {}

  bool IsEmpty() const { return value_ == kEmpty; }
  bool IsFull() const { return !IsEmpty(); }

 private:
  uint8_t value_;
};

#if 0
enum class ctrl_t : int8_t {
  kEmpty = -128,   // 0b10000000
  kDeleted = -2,   // 0b11111110
  kSentinel = -1,  // 0b11111111
};
static_assert(
    (static_cast<int8_t>(ctrl_t::kEmpty) &
     static_cast<int8_t>(ctrl_t::kDeleted) &
     static_cast<int8_t>(ctrl_t::kSentinel) & 0x80) != 0,
    "Special markers need to have the MSB to make checking for them efficient");
static_assert(
    ctrl_t::kEmpty < ctrl_t::kSentinel && ctrl_t::kDeleted < ctrl_t::kSentinel,
    "ctrl_t::kEmpty and ctrl_t::kDeleted must be smaller than "
    "ctrl_t::kSentinel to make the SIMD test of IsEmptyOrDeleted() efficient");
static_assert(
    ctrl_t::kSentinel == static_cast<ctrl_t>(-1),
    "ctrl_t::kSentinel must be -1 to elide loading it from memory into SIMD "
    "registers (pcmpeqd xmm, xmm)");
static_assert(ctrl_t::kEmpty == static_cast<ctrl_t>(-128),
              "ctrl_t::kEmpty must be -128 to make the SIMD check for its "
              "existence efficient (psignb xmm, xmm)");
static_assert(
    (~static_cast<int8_t>(ctrl_t::kEmpty) &
     ~static_cast<int8_t>(ctrl_t::kDeleted) &
     static_cast<int8_t>(ctrl_t::kSentinel) & 0x7F) != 0,
    "ctrl_t::kEmpty and ctrl_t::kDeleted must share an unset bit that is not "
    "shared by ctrl_t::kSentinel to make the scalar test for "
    "MaskEmptyOrDeleted() efficient");
static_assert(ctrl_t::kDeleted == static_cast<ctrl_t>(-2),
              "ctrl_t::kDeleted must be -2 to make the implementation of "
              "ConvertSpecialToEmptyAndFullToDeleted efficient");
#endif

static const size_t max_slots_per_bucket = 14;

// Returns a pointer to data that can be used by empty tables.
ABSL_DLL extern const char kEmptyData[max_slots_per_bucket + 2];

inline char* EmptyData() {
  // Const must be cast away here; no uses of this function will actually write
  // to it, because it is only used for empty tables.
  return const_cast<char*>(kEmptyData);
}

// Returns a pointer to a generation to use for an empty hashtable.
GenerationType* EmptyGeneration();

// Returns whether `generation` is a generation for an empty hashtable that
// could be returned by EmptyGeneration().
inline bool IsEmptyGeneration(const GenerationType* generation) {
  return *generation == SentinelEmptyGeneration();
}

// Mixes a randomly generated per-process seed with `hash` and `ctrl` to
// randomize insertion order within groups.
bool ShouldInsertBackwards(size_t hash, const ctrl_t* ctrl);

// Returns a per-table, hash salt, which changes on resize. This gets mixed into
// H1 to randomize iteration order per-table.
//
// The seed consists of the ctrl_ pointer, which adds enough entropy to ensure
// non-determinism of iteration order in most cases.
inline size_t PerTableSalt(const ctrl_t* ctrl) {
  // The low bits of the pointer have little or no entropy because of
  // alignment. We shift the pointer to try to use higher entropy bits. A
  // good number seems to be 12 bits, because that aligns with page size.
  return reinterpret_cast<uintptr_t>(ctrl) >> 12;
}
// Extracts the H1 portion of a hash: 57 bits mixed with a per-table salt.
inline size_t H1(size_t hash, const ctrl_t* ctrl) {
  return (hash >> 7) ^ PerTableSalt(ctrl);
}

// Extracts the H2 portion of a hash: the 7 bits not used for H1.
//
// These are used as an occupied control byte.
inline h2_t H2(size_t hash) { return hash & 0x7F; }

// TODO: Maybe the template parameters can be removed on this code so that there
// is less code bloat.
template<size_t slots_per_bucket, class SlotType>
class BucketPointer {
  static constexpr uint16_t kSearchDistanceMask = (1u<<15) - 1u;
 public:
  BucketPointer() :bucket_start_(EmptyData()) {}
  SlotType& GetSlot(size_t offset) {
    assert(offset < slots_per_bucket);
    return *reinterpret_cast<SlotType*>(bucket_start_ + slots_per_bucket + 2 + offset * sizeof(SlotType));
  }
  bool SlotIsFull(size_t offset) const {
    return GetCtrl(offset).IsFull();
  }
  ctrl_t GetCtrl(size_t offset) const {
    assert(offset < slots_per_bucket);
    return ctrl_t(bucket_start_[2 + offset]);
  }
  size_t SearchDistance() const {
    return (*reinterpret_cast<const uint16_t*>(bucket_start_)) & kSearchDistanceMask;
  }
  void SetSearchDistance(size_t distance) {
    assert(distance <= kSearchDistanceMask);
    uint16_t *p = reinterpret_cast<const uint16_t*>(bucket_start_);
    *p = (*p & (~kSearchDistanceMask)) | distance;
  }
  void SetNotLastAndSearchDistanceToZero() {
    uint16_t *p = reinterpret_cast<const uint16_t*>(bucket_start_);
    *p = 0;
  }
  bool IsLast() const {
    uint16_t *p = reinterpret_cast<const uint16_t*>(bucket_start_);
    return *p >> 15;
  }
  BucketPointer& operator++() {
    if (IsLast()) {
      bucket_start_ = nullptr;
    } else {
      bucket_start_ += slots_per_bucket + 2 + slots_per_bucket * sizeof(SlotType);
    }
    return *this;
  }
  BucketPointer operator+(size_t bucket_count) {
    BucketPointer result(bucket_start_);
    result.bucket_start += bucket_count * (slots_per_bucket + 2 + slots_per_bucket * sizeof(SlotType));
    return result;
  }
 private:
  explicit BucketPointer(char *bucket_start) :bucket_start_(bucket_start) {
    assert(bucket_start != nullptr);
  }
  char *bucket_start_ = EmptyData();
};

#ifdef ABSL_INTERNAL_HAVE_SSE2
// Quick reference guide for intrinsics used below:
//
// * __m128i: An XMM (128-bit) word.
//
// * _mm_setzero_si128: Returns a zero vector.
// * _mm_set1_epi8:     Returns a vector with the same i8 in each lane.
//
// * _mm_subs_epi8:    Saturating-subtracts two i8 vectors.
// * _mm_and_si128:    Ands two i128s together.
// * _mm_or_si128:     Ors two i128s together.
// * _mm_andnot_si128: And-nots two i128s together.
//
// * _mm_cmpeq_epi8: Component-wise compares two i8 vectors for equality,
//                   filling each lane with 0x00 or 0xff.
// * _mm_cmpgt_epi8: Same as above, but using > rather than ==.
//
// * _mm_loadu_si128:  Performs an unaligned load of an i128.
// * _mm_storeu_si128: Performs an unaligned store of an i128.
//
// * _mm_sign_epi8:     Retains, negates, or zeroes each i8 lane of the first
//                      argument if the corresponding lane of the second
//                      argument is positive, negative, or zero, respectively.
// * _mm_movemask_epi8: Selects the sign bit out of each i8 lane and produces a
//                      bitmask consisting of those bits.
// * _mm_shuffle_epi8:  Selects i8s from the first argument, using the low
//                      four bits of each i8 lane in the second argument as
//                      indices.

// https://github.com/abseil/abseil-cpp/issues/209
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=87853
// _mm_cmpgt_epi8 is broken under GCC with -funsigned-char
// Work around this by using the portable implementation of Group
// when using -funsigned-char under GCC.
inline __m128i _mm_cmpgt_epi8_fixed(__m128i a, __m128i b) {
#if defined(__GNUC__) && !defined(__clang__)
  if (std::is_unsigned<char>::value) {
    const __m128i mask = _mm_set1_epi8(0x80);
    const __m128i diff = _mm_subs_epi8(b, a);
    return _mm_cmpeq_epi8(_mm_and_si128(diff, mask), mask);
  }
#endif
  return _mm_cmpgt_epi8(a, b);
}

struct GroupSse2Impl {
  static constexpr size_t kWidth = 16;  // the number of slots per group

  explicit GroupSse2Impl(const ctrl_t* pos) {
    ctrl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pos));
  }

  // Returns a bitmask representing the positions of slots that match hash.
  BitMask<uint32_t, kWidth> Match(h2_t hash) const {
    auto match = _mm_set1_epi8(static_cast<char>(hash));
    return BitMask<uint32_t, kWidth>(
        static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, ctrl))));
  }

  // Returns a bitmask representing the positions of empty slots.
  NonIterableBitMask<uint32_t, kWidth> MaskEmpty() const {
    return NonIterableBitMask<uint32_t, kWidth>(MaskEmptyInt());
  }

  uint32_t MaskEmptyInt() const {
#ifdef ABSL_INTERNAL_HAVE_SSSE3
    // This works because empty values have the sign bit set.
    return static_cast<uint32_t>(_mm_movemask_epi8(ctrl));
#else
    auto match = _mm_set1_epi8(static_cast<char>(ctrl_t::kEmpty));
    return static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, ctrl)));
#endif
  }

  // Returns the number of trailing empty elements in the group.
  uint32_t CountLeadingEmpty() const {
    return TrailingZeros(static_cast<uint32_t>(MaskEmptyInt()));
  }

  __m128i ctrl;
};
#endif  // ABSL_INTERNAL_RAW_HASH_SET_HAVE_SSE2

#if defined(ABSL_INTERNAL_HAVE_ARM_NEON) && defined(ABSL_IS_LITTLE_ENDIAN)
struct GroupAArch64Impl {
  static constexpr size_t kWidth = 8;

  explicit GroupAArch64Impl(const ctrl_t* pos) {
    ctrl = vld1_u8(reinterpret_cast<const uint8_t*>(pos));
  }

  BitMask<uint64_t, kWidth, 3> Match(h2_t hash) const {
    uint8x8_t dup = vdup_n_u8(hash);
    auto mask = vceq_u8(ctrl, dup);
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    return BitMask<uint64_t, kWidth, 3>(
        vget_lane_u64(vreinterpret_u64_u8(mask), 0) & msbs);
  }

  NonIterableBitMask<uint64_t, kWidth, 3> MaskEmpty() const {
    uint64_t mask =
        vget_lane_u64(vreinterpret_u64_u8(vceq_s8(
                          vdup_n_s8(static_cast<int8_t>(ctrl_t::kEmpty)),
                          vreinterpret_s8_u8(ctrl))),
                      0);
    return NonIterableBitMask<uint64_t, kWidth, 3>(mask);
  }

  NonIterableBitMask<uint64_t, kWidth, 3> MaskEmptyOrDeleted() const {
    uint64_t mask =
        vget_lane_u64(vreinterpret_u64_u8(vcgt_s8(
                          vdup_n_s8(static_cast<int8_t>(ctrl_t::kSentinel)),
                          vreinterpret_s8_u8(ctrl))),
                      0);
    return NonIterableBitMask<uint64_t, kWidth, 3>(mask);
  }

  uint32_t CountLeadingEmptyOrDeleted() const {
    uint64_t mask =
        vget_lane_u64(vreinterpret_u64_u8(vcle_s8(
                          vdup_n_s8(static_cast<int8_t>(ctrl_t::kSentinel)),
                          vreinterpret_s8_u8(ctrl))),
                      0);
    // Similar to MaskEmptyorDeleted() but we invert the logic to invert the
    // produced bitfield. We then count number of trailing zeros.
    // Clang and GCC optimize countr_zero to rbit+clz without any check for 0,
    // so we should be fine.
    return static_cast<uint32_t>(countr_zero(mask)) >> 3;
  }

  void ConvertSpecialToEmptyAndFullToDeleted(ctrl_t* dst) const {
    uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(ctrl), 0);
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    constexpr uint64_t slsbs = 0x0202020202020202ULL;
    constexpr uint64_t midbs = 0x7e7e7e7e7e7e7e7eULL;
    auto x = slsbs & (mask >> 6);
    auto res = (x + midbs) | msbs;
    little_endian::Store64(dst, res);
  }

  uint8x8_t ctrl;
};
#endif  // ABSL_INTERNAL_HAVE_ARM_NEON && ABSL_IS_LITTLE_ENDIAN

struct GroupPortableImpl {
  static constexpr size_t kWidth = 8;

  explicit GroupPortableImpl(const ctrl_t* pos)
      : ctrl(little_endian::Load64(pos)) {}

  BitMask<uint64_t, kWidth, 3> Match(h2_t hash) const {
    // For the technique, see:
    // http://graphics.stanford.edu/~seander/bithacks.html##ValueInWord
    // (Determine if a word has a byte equal to n).
    //
    // Caveat: there are false positives but:
    // - they only occur if there is a real match
    // - they never occur on ctrl_t::kEmpty, ctrl_t::kDeleted, ctrl_t::kSentinel
    // - they will be handled gracefully by subsequent checks in code
    //
    // Example:
    //   v = 0x1716151413121110
    //   hash = 0x12
    //   retval = (v - lsbs) & ~v & msbs = 0x0000000080800000
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    constexpr uint64_t lsbs = 0x0101010101010101ULL;
    auto x = ctrl ^ (lsbs * hash);
    return BitMask<uint64_t, kWidth, 3>((x - lsbs) & ~x & msbs);
  }

  NonIterableBitMask<uint64_t, kWidth, 3> MaskEmpty() const {
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    return NonIterableBitMask<uint64_t, kWidth, 3>((ctrl & (~ctrl << 6)) &
                                                   msbs);
  }

  NonIterableBitMask<uint64_t, kWidth, 3> MaskEmptyOrDeleted() const {
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    return NonIterableBitMask<uint64_t, kWidth, 3>((ctrl & (~ctrl << 7)) &
                                                   msbs);
  }

  uint32_t CountLeadingEmptyOrDeleted() const {
    // ctrl | ~(ctrl >> 7) will have the lowest bit set to zero for kEmpty and
    // kDeleted. We lower all other bits and count number of trailing zeros.
    constexpr uint64_t bits = 0x0101010101010101ULL;
    return static_cast<uint32_t>(countr_zero((ctrl | ~(ctrl >> 7)) & bits) >>
                                 3);
  }

  void ConvertSpecialToEmptyAndFullToDeleted(ctrl_t* dst) const {
    constexpr uint64_t msbs = 0x8080808080808080ULL;
    constexpr uint64_t lsbs = 0x0101010101010101ULL;
    auto x = ctrl & msbs;
    auto res = (~x + (x >> 7)) & ~lsbs;
    little_endian::Store64(dst, res);
  }

  uint64_t ctrl;
};

#ifdef ABSL_INTERNAL_HAVE_SSE2
using Group = GroupSse2Impl;
#elif defined(ABSL_INTERNAL_HAVE_ARM_NEON) && defined(ABSL_IS_LITTLE_ENDIAN)
using Group = GroupAArch64Impl;
#else
using Group = GroupPortableImpl;
#endif

// When there is an insertion with no reserved growth, we rehash with
// probability `min(1, RehashProbabilityConstant() / capacity())`. Using a
// constant divided by capacity ensures that inserting N elements is still O(N)
// in the average case. Using the constant 16 means that we expect to rehash ~8
// times more often than when generations are disabled. We are adding expected
// rehash_probability * #insertions/capacity_growth = 16/capacity * ((7/8 -
// 7/16) * capacity)/capacity_growth = ~7 extra rehashes per capacity growth.
inline size_t RehashProbabilityConstant() { return 16; }

class CommonFieldsGenerationInfoEnabled {
  // A sentinel value for reserved_growth_ indicating that we just ran out of
  // reserved growth on the last insertion. When reserve is called and then
  // insertions take place, reserved_growth_'s state machine is N, ..., 1,
  // kReservedGrowthJustRanOut, 0.
  static constexpr size_t kReservedGrowthJustRanOut =
      (std::numeric_limits<size_t>::max)();

 public:
  CommonFieldsGenerationInfoEnabled() = default;
  CommonFieldsGenerationInfoEnabled(CommonFieldsGenerationInfoEnabled&& that)
      : reserved_growth_(that.reserved_growth_), generation_(that.generation_) {
    that.reserved_growth_ = 0;
    that.generation_ = EmptyGeneration();
  }
  CommonFieldsGenerationInfoEnabled& operator=(
      CommonFieldsGenerationInfoEnabled&&) = default;

  // We rehash on the first insertion after after reserved_growth_ reaches 0
  // after a call to reserve.  In order to avoid having to also do a rehash with
  // low probability whenever reserved_growth_ is zero, we just set
  // reserved_growth_ to a lower value when we have GenerationInfo enabled.
  void maybe_increment_generation_on_insert() {
    if (reserved_growth_ == kReservedGrowthJustRanOut) reserved_growth_ = 0;

    if (reserved_growth_ > 0) {
      if (--reserved_growth_ == 0) reserved_growth_ = kReservedGrowthJustRanOut;
    } else {
      *generation_ = NextGeneration(*generation_);
    }
  }
  void reset_reserved_growth(size_t reservation, size_t size) {
    reserved_growth_ = reservation - size;
  }
  size_t reserved_growth() const { return reserved_growth_; }
  void set_reserved_growth(size_t r) { reserved_growth_ = r; }
  GenerationType generation() const { return *generation_; }
  void set_generation(GenerationType g) { *generation_ = g; }
  GenerationType* generation_ptr() const { return generation_; }
  void set_generation_ptr(GenerationType* g) { generation_ = g; }

 private:
  // The number of insertions remaining that are guaranteed to not rehash due to
  // a prior call to reserve. Note: we store reserved growth rather than
  // reservation size because calls to erase() decrease size_ but don't decrease
  // reserved growth.
  size_t reserved_growth_ = 0;
  // Pointer to the generation counter, which is used to validate iterators and
  // is stored in the backing array between the control bytes and the slots.
  // Note that we can't store the generation inside the container itself and
  // keep a pointer to the container in the iterators because iterators must
  // remain valid when the container is moved.
  // Note: we could derive this pointer from the control pointer, but it makes
  // the code more complicated, and there's a benefit in having the sizes of
  // graveyard_raw_hash_set in sanitizer mode and non-sanitizer mode a bit more different,
  // which is that tests are less likely to rely on the size remaining the same.
  GenerationType* generation_ = EmptyGeneration();
};

class CommonFieldsGenerationInfoDisabled {
 public:
  CommonFieldsGenerationInfoDisabled() = default;
  CommonFieldsGenerationInfoDisabled(CommonFieldsGenerationInfoDisabled&&) =
      default;
  CommonFieldsGenerationInfoDisabled& operator=(
      CommonFieldsGenerationInfoDisabled&&) = default;

  void maybe_increment_generation_on_insert() {}
  void reset_reserved_growth(size_t, size_t) {}
  size_t reserved_growth() const { return 0; }
  void set_reserved_growth(size_t) {}
  GenerationType generation() const { return 0; }
  void set_generation(GenerationType) {}
  GenerationType* generation_ptr() const { return nullptr; }
  void set_generation_ptr(GenerationType*) {}
};

class HashSetIteratorGenerationInfoEnabled {
 public:
  HashSetIteratorGenerationInfoEnabled() = default;
  explicit HashSetIteratorGenerationInfoEnabled(
      const GenerationType* generation_ptr)
      : generation_ptr_(generation_ptr), generation_(*generation_ptr) {}

  GenerationType generation() const { return generation_; }
  void reset_generation() { generation_ = *generation_ptr_; }
  const GenerationType* generation_ptr() const { return generation_ptr_; }
  void set_generation_ptr(const GenerationType* ptr) { generation_ptr_ = ptr; }

 private:
  const GenerationType* generation_ptr_ = EmptyGeneration();
  GenerationType generation_ = *generation_ptr_;
};

class HashSetIteratorGenerationInfoDisabled {
 public:
  HashSetIteratorGenerationInfoDisabled() = default;
  explicit HashSetIteratorGenerationInfoDisabled(const GenerationType*) {}

  GenerationType generation() const { return 0; }
  void reset_generation() {}
  const GenerationType* generation_ptr() const { return nullptr; }
  void set_generation_ptr(const GenerationType*) {}
};

#ifdef ABSL_SWISSTABLE_ENABLE_GENERATIONS
using CommonFieldsGenerationInfo = CommonFieldsGenerationInfoEnabled;
using HashSetIteratorGenerationInfo = HashSetIteratorGenerationInfoEnabled;
#else
using CommonFieldsGenerationInfo = CommonFieldsGenerationInfoDisabled;
using HashSetIteratorGenerationInfo = HashSetIteratorGenerationInfoDisabled;
#endif

// CommonFields hold the fields in graveyard_raw_hash_set that do not depend
// on template parameters. This allows us to conveniently pass all
// of this state to helper functions as a single argument.
template <size_t slots_per_bucket, class slot_type>
class CommonFields : public CommonFieldsGenerationInfo {
 public:
  CommonFields() = default;

  // Not copyable
  CommonFields(const CommonFields&) = delete;
  CommonFields& operator=(const CommonFields&) = delete;

  // Movable
  CommonFields(CommonFields&& that)
      : CommonFieldsGenerationInfo(
            std::move(static_cast<CommonFieldsGenerationInfo&&>(that))),
        // Explicitly copying fields into "this" and then resetting "that"
        // fields generates less code then calling absl::exchange per field.
        buckets_(that.buckets_),
        size_(that.size_),
        capacity_(that.capacity_),
        compressed_tuple_(that.growth_left(), std::move(that.infoz())) {
    that.buckets__ = BucketPointer<slots_per_bucket, slot_type>();
    that.size_ = 0;
    that.capacity_ = 0;
    that.growth_left() = 0;
  }
  CommonFields& operator=(CommonFields&&) = default;

  // The number of slots we can still fill without needing to rehash.
  size_t& growth_left() { return compressed_tuple_.template get<0>(); }

  HashtablezInfoHandle& infoz() { return compressed_tuple_.template get<1>(); }
  const HashtablezInfoHandle& infoz() const {
    return compressed_tuple_.template get<1>();
  }

  void reset_reserved_growth(size_t reservation) {
    CommonFieldsGenerationInfo::reset_reserved_growth(reservation, size_);
  }

  // We need to know the number of buckets, but sometimes we need the capacity
  // (when deciding to insert backwards).  We don't want to constantly have to
  // divide by 14.
  class Capacity {
    static constexpr ptrdiff_t ToEncodedCapacity(size_t capacity) {
      assert(capacity <= std::numeric_limits<ptrdiff_t>::max());
      if (capacity < slots_per_bucket) {
        return -capacity;
      } else {
        assert(capacity % slots_per_bucket == 0);
        return capacity / slots_per_bucket;
      }
    }
    static constexpr size_t FromEncodedCapacity(ptrdiff_t encoded) {
      if (encoded <= 0) {
        return -encoded;
      } else {
        return encoded * slots_per_bucket;
      }
    }
   public:
    explicit Capacity(size_t capacity) :encoded_capacity_(ToEncodedCapacity(capacity)) {}
    size_t capacity() const { return FromEncodedCapacity(encoded_capacity_); };
    // Returns the number of buckets (0 if the capacity is 0).
    size_t bucket_count() const {
      if (encoded_capacity_ < 0) return 1;
      else return encoded_capacity_;
    }
   private:
    ptrdiff_t encoded_capacity_;
  };

  BucketPointer<slots_per_bucket, slot_type> buckets_;

  // The number of filled slots.
  size_t size_ = 0;

  // The total number of available slots.
  Capacity capacity_;

  // Bundle together growth_left and HashtablezInfoHandle to ensure EBO for
  // HashtablezInfoHandle when sampling is turned off.
  absl::container_internal::CompressedTuple<size_t, HashtablezInfoHandle>
      compressed_tuple_{0u, HashtablezInfoHandle{}};
};

// Returns the number of "cloned control bytes".
//
// This is the number of control bytes that are present both at the beginning
// of the control byte array and at the end, such that we can create a
// `Group::kWidth`-width probe window starting from any control byte.
constexpr size_t NumClonedBytes() { return Group::kWidth - 1; }

template <class Policy, class Hash, class Eq, class Alloc>
class graveyard_raw_hash_set;

// Returns whether `n` is a valid capacity (i.e., number of slots).
//
// A valid capacity is a non-zero integer `2^m - 1`.
inline bool IsValidCapacity(size_t n) { return ((n + 1) & n) == 0 && n > 0; }

// Returns the next valid capacity after `n`.
inline size_t NextCapacity(size_t n) {
  assert(IsValidCapacity(n) || n == 0);
  return n * 2 + 1;
}

// Applies the following mapping to every byte in the control array:
//   * kDeleted -> kEmpty
//   * kEmpty -> kEmpty
//   * _ -> kDeleted
// PRECONDITION:
//   IsValidCapacity(capacity)
//   ctrl[capacity] == ctrl_t::kSentinel
//   ctrl[i] != ctrl_t::kSentinel for all i < capacity
void ConvertDeletedToEmptyAndFullToDeleted(ctrl_t* ctrl, size_t capacity);

// Converts `n` into the next valid capacity, per `IsValidCapacity`.
inline size_t NormalizeCapacity(size_t n) {
  return n ? ~size_t{} >> countl_zero(n) : 1;
}

// General notes on capacity/growth methods below:
// - We use 7/8th as maximum load factor. For 16-wide groups, that gives an
//   average of two empty slots per group.
// - For (capacity+1) >= Group::kWidth, growth is 7/8*capacity.
// - For (capacity+1) < Group::kWidth, growth == capacity. In this case, we
//   never need to probe (the whole table fits in one group) so we don't need a
//   load factor less than 1.

// Given `capacity`, applies the load factor; i.e., it returns the maximum
// number of values we should put into the table before a resizing rehash.
inline size_t CapacityToGrowth(size_t capacity) {
  assert(IsValidCapacity(capacity));
  // `capacity*7/8`
  if (Group::kWidth == 8 && capacity == 7) {
    // x-x/8 does not work when x==7.
    return 6;
  }
  return capacity - capacity / 8;
}

// Given `growth`, "unapplies" the load factor to find how large the capacity
// should be to stay within the load factor.
//
// This might not be a valid capacity and `NormalizeCapacity()` should be
// called on this.
inline size_t GrowthToLowerboundCapacity(size_t growth) {
  // `growth*8/7`
  if (Group::kWidth == 8 && growth == 7) {
    // x+(x-1)/7 does not work when x==7.
    return 8;
  }
  return growth + static_cast<size_t>((static_cast<int64_t>(growth) - 1) / 7);
}

template <class InputIter>
size_t SelectBucketCountForIterRange(InputIter first, InputIter last,
                                     size_t bucket_count) {
  if (bucket_count != 0) {
    return bucket_count;
  }
  using InputIterCategory =
      typename std::iterator_traits<InputIter>::iterator_category;
  if (std::is_base_of<std::random_access_iterator_tag,
                      InputIterCategory>::value) {
    return GrowthToLowerboundCapacity(
        static_cast<size_t>(std::distance(first, last)));
  }
  return 0;
}

constexpr bool SwisstableDebugEnabled() {
#if defined(ABSL_SWISSTABLE_ENABLE_GENERATIONS) || \
    ABSL_OPTION_HARDENED == 1 || !defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

struct FindInfo {
  size_t offset;
  size_t probe_length;
};

// Whether a table is "small". A small table fits entirely into a probing
// group, i.e., has a capacity < `Group::kWidth`.
//
// In small mode we are able to use the whole capacity. The extra control
// bytes give us at least one "empty" control byte to stop the iteration.
// This is important to make 1 a valid capacity.
//
// In small mode only the first `capacity` control bytes after the sentinel
// are valid. The rest contain dummy ctrl_t::kEmpty values that do not
// represent a real slot. This is important to take into account on
// `find_first_non_full()`, where we never try
// `ShouldInsertBackwards()` for small tables.
inline bool is_small(size_t capacity) { return capacity < Group::kWidth - 1; }

#if 0
// Probes an array of control bits using a probe sequence derived from `hash`,
// and returns the offset corresponding to the first deleted or empty slot.
//
// Behavior when the entire table is full is undefined.
template <size_t slots_per_bucket, class slot_type>
inline FindInfo find_first_non_full(const CommonFields<slots_per_bucket, slot_type>& common, size_t hash) {
  auto seq = probe_seq{hash, common, hash};
  const ctrl_t* ctrl = common.control_;
  while (true) {
    Group g{ctrl + seq.offset()};
    auto mask = g.MaskEmptyOrDeleted();
    if (mask) {
#if !defined(NDEBUG)
      // We want to add entropy even when ASLR is not enabled.
      // In debug build we will randomly insert in either the front or back of
      // the group.
      // TODO(kfm,sbenza): revisit after we do unconditional mixing
      if (!is_small(common.capacity_) && ShouldInsertBackwards(hash, ctrl)) {
        return {seq.offset(mask.HighestBitSet()), seq.index()};
      }
#endif
      return {seq.offset(mask.LowestBitSet()), seq.index()};
    }
    seq.next();
    assert(seq.index() <= common.capacity_ && "full table!");
  }
}

// Extern template for inline function keep possibility of inlining.
// When compiler decided to not inline, no symbols will be added to the
// corresponding translation unit.
extern template FindInfo find_first_non_full(const CommonFields&, size_t);

// Non-inlined version of find_first_non_full for use in less
// performance critical routines.
FindInfo find_first_non_full_outofline(const CommonFields&, size_t);

inline void ResetGrowthLeft(CommonFields& common) {
  common.growth_left() = CapacityToGrowth(common.capacity_) - common.size_;
}

// Sets `ctrl` to `{kEmpty, kSentinel, ..., kEmpty}`, marking the entire
// array as marked as empty.
inline void ResetCtrl(CommonFields& common, size_t slot_size) {
  const size_t capacity = common.capacity_;
  ctrl_t* ctrl = common.control_;
  std::memset(ctrl, static_cast<int8_t>(ctrl_t::kEmpty),
              capacity + 1 + NumClonedBytes());
  ctrl[capacity] = ctrl_t::kSentinel;
  SanitizerPoisonMemoryRegion(common.slots_, slot_size * capacity);
  ResetGrowthLeft(common);
}

// Sets `ctrl[i]` to `h`.
//
// Unlike setting it directly, this function will perform bounds checks and
// mirror the value to the cloned tail if necessary.
inline void SetCtrl(const CommonFields& common, size_t i, ctrl_t h,
                    size_t slot_size) {
  const size_t capacity = common.capacity_;
  assert(i < capacity);

  auto* slot_i = static_cast<const char*>(common.slots_) + i * slot_size;
  if (IsFull(h)) {
    SanitizerUnpoisonMemoryRegion(slot_i, slot_size);
  } else {
    SanitizerPoisonMemoryRegion(slot_i, slot_size);
  }

  ctrl_t* ctrl = common.control_;
  ctrl[i] = h;
  ctrl[((i - NumClonedBytes()) & capacity) + (NumClonedBytes() & capacity)] = h;
}

// Overload for setting to an occupied `h2_t` rather than a special `ctrl_t`.
inline void SetCtrl(const CommonFields& common, size_t i, h2_t h,
                    size_t slot_size) {
  SetCtrl(common, i, static_cast<ctrl_t>(h), slot_size);
}
#endif

// Given the capacity of a table, computes the offset (from the start of the
// backing allocation) of the generation counter (if it exists).
inline size_t GenerationOffset(size_t capacity) {
  assert(IsValidCapacity(capacity));
  const size_t num_control_bytes = capacity + 1 + NumClonedBytes();
  return num_control_bytes;
}

// Given the capacity of a table, computes the offset (from the start of the
// backing allocation) at which the slots begin.
inline size_t SlotOffset(size_t capacity, size_t slot_align) {
  assert(IsValidCapacity(capacity));
  const size_t num_control_bytes = capacity + 1 + NumClonedBytes();
  return (num_control_bytes + NumGenerationBytes() + slot_align - 1) &
         (~slot_align + 1);
}

// Given the capacity of a table, computes the total size of the backing
// array.
inline size_t AllocSize(size_t capacity, size_t slot_size, size_t slot_align) {
  return SlotOffset(capacity, slot_align) + capacity * slot_size;
}

#if 0
template <typename Alloc, size_t SizeOfSlot, size_t AlignOfSlot>
ABSL_ATTRIBUTE_NOINLINE void InitializeSlots(CommonFields& c, Alloc alloc) {
  assert(c.capacity_);
  // Folks with custom allocators often make unwarranted assumptions about the
  // behavior of their classes vis-a-vis trivial destructability and what
  // calls they will or won't make.  Avoid sampling for people with custom
  // allocators to get us out of this mess.  This is not a hard guarantee but
  // a workaround while we plan the exact guarantee we want to provide.
  const size_t sample_size =
      (std::is_same<Alloc, std::allocator<char>>::value && c.slots_ == nullptr)
          ? SizeOfSlot
          : 0;

  const size_t cap = c.capacity_;
  char* mem = static_cast<char*>(
      Allocate<AlignOfSlot>(&alloc, AllocSize(cap, SizeOfSlot, AlignOfSlot)));
  const GenerationType old_generation = c.generation();
  c.set_generation_ptr(
      reinterpret_cast<GenerationType*>(mem + GenerationOffset(cap)));
  c.set_generation(NextGeneration(old_generation));
  c.control_ = reinterpret_cast<ctrl_t*>(mem);
  c.slots_ = mem + SlotOffset(cap, AlignOfSlot);
  ResetCtrl(c, SizeOfSlot);
  if (sample_size) {
    c.infoz() = Sample(sample_size);
  }
  c.infoz().RecordStorageChanged(c.size_, cap);
}
#endif

// PolicyFunctions bundles together some information for a particular
// graveyard_raw_hash_set<T, ...> instantiation. This information is passed to
// type-erased functions that want to do small amounts of type-specific
// work.
struct PolicyFunctions {
  size_t slot_size;

  // Return the hash of the pointed-to slot.
  size_t (*hash_slot)(void* set, void* slot);

  // Transfer the contents of src_slot to dst_slot.
  void (*transfer)(void* set, void* dst_slot, void* src_slot);

  // Deallocate the specified backing store which is sized for n slots.
  void (*dealloc)(void* set, const PolicyFunctions& policy, ctrl_t* ctrl,
                  void* slot_array, size_t n);
};

#if 0
// ClearBackingArray clears the backing array, either modifying it in place,
// or creating a new one based on the value of "reuse".
// REQUIRES: c.capacity > 0
void ClearBackingArray(CommonFields& c, const PolicyFunctions& policy,
                       bool reuse);

// Type-erased version of graveyard_raw_hash_set::erase_meta_only.
void EraseMetaOnly(CommonFields& c, ctrl_t* it, size_t slot_size);

// Function to place in PolicyFunctions::dealloc for graveyard_raw_hash_sets
// that are using std::allocator. This allows us to share the same
// function body for graveyard_raw_hash_set instantiations that have the
// same slot alignment.
template <size_t AlignOfSlot>
ABSL_ATTRIBUTE_NOINLINE void DeallocateStandard(void*,
                                                const PolicyFunctions& policy,
                                                ctrl_t* ctrl, void* slot_array,
                                                size_t n) {
  // Unpoison before returning the memory to the allocator.
  SanitizerUnpoisonMemoryRegion(slot_array, policy.slot_size * n);

  std::allocator<char> alloc;
  Deallocate<AlignOfSlot>(&alloc, ctrl,
                          AllocSize(n, policy.slot_size, AlignOfSlot));
}
#endif

// For trivially relocatable types we use memcpy directly. This allows us to
// share the same function body for graveyard_raw_hash_set instantiations that have the
// same slot size as long as they are relocatable.
template <size_t SizeOfSlot>
ABSL_ATTRIBUTE_NOINLINE void TransferRelocatable(void*, void* dst, void* src) {
  memcpy(dst, src, SizeOfSlot);
}

#if 0
// Type-erased version of graveyard_raw_hash_set::drop_deletes_without_resize.
void DropDeletesWithoutResize(CommonFields& common,
                              const PolicyFunctions& policy, void* tmp_space);
#endif

// A SwissTable.
//
// Policy: a policy defines how to perform different operations on
// the slots of the hashtable (see hash_policy_traits.h for the full interface
// of policy).
//
// Hash: a (possibly polymorphic) functor that hashes keys of the hashtable. The
// functor should accept a key and return size_t as hash. For best performance
// it is important that the hash function provides high entropy across all bits
// of the hash.
//
// Eq: a (possibly polymorphic) functor that compares two keys for equality. It
// should accept two (of possibly different type) keys and return a bool: true
// if they are equal, false if they are not. If two keys compare equal, then
// their hash values as defined by Hash MUST be equal.
//
// Allocator: an Allocator
// [https://en.cppreference.com/w/cpp/named_req/Allocator] with which
// the storage of the hashtable will be allocated and the elements will be
// constructed and destroyed.
template <class Policy, class Hash, class Eq, class Alloc>
class graveyard_raw_hash_set {
  using PolicyTraits = hash_policy_traits<Policy>;
  using KeyArgImpl =
      KeyArg<IsTransparent<Eq>::value && IsTransparent<Hash>::value>;

 public:
  using init_type = typename PolicyTraits::init_type;
  using key_type = typename PolicyTraits::key_type;
  // TODO(sbenza): Hide slot_type as it is an implementation detail. Needs user
  // code fixes!
  using slot_type = typename PolicyTraits::slot_type;
  using allocator_type = Alloc;
  using size_type = size_t;
  using difference_type = ptrdiff_t;
  using hasher = Hash;
  using key_equal = Eq;
  using policy_type = Policy;
  using value_type = typename PolicyTraits::value_type;
  using reference = value_type&;
  using const_reference = const value_type&;
  using pointer = typename absl::allocator_traits<
      allocator_type>::template rebind_traits<value_type>::pointer;
  using const_pointer = typename absl::allocator_traits<
      allocator_type>::template rebind_traits<value_type>::const_pointer;

  // Alias used for heterogeneous lookup functions.
  // `key_arg<K>` evaluates to `K` when the functors are transparent and to
  // `key_type` otherwise. It permits template argument deduction on `K` for the
  // transparent case.
  template <class K>
  using key_arg = typename KeyArgImpl::template type<K, key_type>;

 private:
  // TODO: slots_per_bucket should depend on slot_type.  (See F14, for example).
  static constexpr size_t slots_per_bucket = 14;

  using common_fields = CommonFields<slots_per_bucket, slot_type>;

  // Give an early error when key_type is not hashable/eq.
  auto KeyTypeCanBeHashed(const Hash& h, const key_type& k) -> decltype(h(k));
  auto KeyTypeCanBeEq(const Eq& eq, const key_type& k) -> decltype(eq(k, k));

  using AllocTraits = absl::allocator_traits<allocator_type>;
  using SlotAlloc = typename absl::allocator_traits<
      allocator_type>::template rebind_alloc<slot_type>;
  using SlotAllocTraits = typename absl::allocator_traits<
      allocator_type>::template rebind_traits<slot_type>;

  static_assert(std::is_lvalue_reference<reference>::value,
                "Policy::element() must return a reference");

  template <typename T>
  struct SameAsElementReference
      : std::is_same<typename std::remove_cv<
                         typename std::remove_reference<reference>::type>::type,
                     typename std::remove_cv<
                         typename std::remove_reference<T>::type>::type> {};

  // An enabler for insert(T&&): T must be convertible to init_type or be the
  // same as [cv] value_type [ref].
  // Note: we separate SameAsElementReference into its own type to avoid using
  // reference unless we need to. MSVC doesn't seem to like it in some
  // cases.
  template <class T>
  using RequiresInsertable = typename std::enable_if<
      absl::disjunction<std::is_convertible<T, init_type>,
                        SameAsElementReference<T>>::value,
      int>::type;

  // RequiresNotInit is a workaround for gcc prior to 7.1.
  // See https://godbolt.org/g/Y4xsUh.
  template <class T>
  using RequiresNotInit =
      typename std::enable_if<!std::is_same<T, init_type>::value, int>::type;

  template <class... Ts>
  using IsDecomposable = IsDecomposable<void, PolicyTraits, Hash, Eq, Ts...>;

  using BucketPtr = BucketPointer<slots_per_bucket, slot_type>;

 public:
  static_assert(std::is_same<pointer, value_type*>::value,
                "Allocators with custom pointer types are not supported");
  static_assert(std::is_same<const_pointer, const value_type*>::value,
                "Allocators with custom pointer types are not supported");

  class iterator : private HashSetIteratorGenerationInfo {
    friend class graveyard_raw_hash_set;

   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = typename graveyard_raw_hash_set::value_type;
    using reference =
        absl::conditional_t<PolicyTraits::constant_iterators::value,
                            const value_type&, value_type&>;
    using pointer = absl::remove_reference_t<reference>*;
    using difference_type = typename graveyard_raw_hash_set::difference_type;

    iterator() {}

    // PRECONDITION: not an end() iterator.
    reference operator*() const {
      AssertIsFull("operator*()");
      return PolicyTraits::element(bucket_.GetSlot(slot_in_bucket_));
    }

    // PRECONDITION: not an end() iterator.
    pointer operator->() const {
      AssertIsFull("operator->");
      return &operator*();
    }

    // PRECONDITION: not an end() iterator.
    iterator& operator++() {
      AssertIsFull("operator++");
      AdvanceByOne();
      skip_empty_or_deleted();
      return *this;
    }
    // PRECONDITION: not an end() iterator.
    iterator operator++(int) {
      auto tmp = *this;
      ++*this;
      return tmp;
    }

    friend bool operator==(const iterator& a, const iterator& b) {
      AssertIsValidForComparison(a.ctrl_, a.generation(), a.generation_ptr());
      AssertIsValidForComparison(b.ctrl_, b.generation(), b.generation_ptr());
      AssertSameContainer(a.ctrl_, b.ctrl_, a.slot_, b.slot_,
                          a.generation_ptr(), b.generation_ptr());
      return a.bucket_ == b.bucket_ && a.slot_in_bucket_ == b.slot_in_bucket_;
    }
    friend bool operator!=(const iterator& a, const iterator& b) {
      return !(a == b);
    }

   private:
    iterator(BucketPtr bucket_pointer,
             size_t slot_in_bucket,
             const GenerationType* generation_ptr)
        : HashSetIteratorGenerationInfo(generation_ptr),
          bucket_(bucket_pointer),
          slot_in_bucket_(slot_in_bucket) {
    }

    void AdvanceByOne() {
      ++slot_in_bucket_;
      if (slot_in_bucket_ == slots_per_bucket) {
        slot_in_bucket_ = 0;
        ++bucket_;
      }
    }

    // Fixes up the iterator to point to a full by advancing it and `slot_` until
    // they reach one.
    //
    // If the end reached, we turn it into an end iterator.
    void skip_empty_or_deleted() {
      // TODO(bradley): Vectorize the finding of the next full slot.
      while (!bucket_.IsEnd() && bucket_.IsEmpty(slot_in_bucket_)) {
        AdvanceByOne();
      }
    }

    bool IsEnd() const {
      return slot_in_bucket_ == slots_per_bucket;
    }

    static constexpr size_t kDefaultConstructedSlot =
        std::numeric_limits<size_t>::min();

    bool IsDefault() const {
      return slot_in_bucket_ == kDefaultConstructedSlot;
    }

    // We could probably reduce code bloat if we made these assertions not be
    // templated on the slot_type, but this is for debug-mode, so it probably
    // doesn't matter.
    void AssertIsFull(const char* operation) const {
      if (!SwisstableDebugEnabled()) return;
      if (IsEnd()) {
        ABSL_INTERNAL_LOG(FATAL,
                          std::string(operation) + " called on end() iterator.");
      }
      if (IsDefault()) {
        ABSL_INTERNAL_LOG(FATAL, std::string(operation) +
                          " called on default-constructed iterator.");
      }
      if (SwisstableGenerationsEnabled()) {
        if (generation() != *generation_ptr()) {
          ABSL_INTERNAL_LOG(FATAL,
                            std::string(operation) +
                            " called on invalid iterator. The table could have "
                            "rehashed since this iterator was initialized.");
        }
        if (!bucket_.IsFull(slot_in_bucket_)) {
          ABSL_INTERNAL_LOG(
              FATAL,
              std::string(operation) +
              " called on invalid iterator. The element was likely erased.");
        }
      } else {
        if (!bucket_.IsFull(slot_in_bucket_)) {
          ABSL_INTERNAL_LOG(
              FATAL,
              std::string(operation) +
              " called on invalid iterator. The element might have been erased "
              "or the table might have rehashed. Consider running with "
              "--config=asan to diagnose rehashing issues.");
        }
      }
    }

    // Note that for comparisons, null/end iterators are valid.
    void AssertIsValidForComparison() const {
      if (!SwisstableDebugEnabled()) return;
      if (IsEnd()) return;
      if (SwisstableGenerationsEnabled()) {
        if (generation() != *generation_ptr()) {
          ABSL_INTERNAL_LOG(FATAL,
                            "Invalid iterator comparison. The table could have "
                            "rehashed since this iterator was initialized.");
        }
        if (!bucket_.IsFull()) {
          ABSL_INTERNAL_LOG(
              FATAL, "Invalid iterator comparison. The element was likely erased.");
        }
      } else {
        ABSL_HARDENING_ASSERT(
            bucket_.IsFull() &&
            "Invalid iterator comparison. The element might have been erased or "
            "the table might have rehashed. Consider running with --config=asan to "
            "diagnose rehashing issues.");
      }
    }

    // Asserts that two iterators come from the same container.
    // Note: we take slots by reference so that it's not UB if they're uninitialized
    // as long as we don't read them (when ctrl is null).
    static void AssertSameContainer(iterator a, iterator b) {
      if (!SwisstableDebugEnabled()) return;
      if (a.IsDefault() != b.IsDefault()) {
        ABSL_INTERNAL_LOG(
            FATAL,
            "Invalid iterator comparison. Comparing default-constructed iterator "
            "with non-default-constructed iterator.");
      }
      if (a.IsDefault() && b.IsDefault()) return;

      if (SwisstableGenerationsEnabled()) {
        if (a.generation_ptr() == b.generation_ptr()) return;
        const bool a_is_empty = IsEmptyGeneration(a.generation_ptr());
        const bool b_is_empty = IsEmptyGeneration(b.generation_ptr());
        if (a_is_empty != b_is_empty) {
          ABSL_INTERNAL_LOG(FATAL,
                            "Invalid iterator comparison. Comparing iterator from "
                            "a non-empty hashtable with an iterator from an empty "
                            "hashtable.");
        }
        if (a_is_empty && b_is_empty) {
          ABSL_INTERNAL_LOG(FATAL,
                            "Invalid iterator comparison. Comparing iterators from "
                            "different empty hashtables.");
        }
        if (a.IsEnd() || b.IsEnd()) {
          ABSL_INTERNAL_LOG(FATAL,
                            "Invalid iterator comparison. Comparing iterator with "
                            "an end() iterator from a different hashtable.");
        }
        ABSL_INTERNAL_LOG(FATAL,
                          "Invalid iterator comparison. Comparing non-end() "
                          "iterators from different hashtables.");
      } else {
        // We cannot easily check that iterators are from the same container
        // with this representation.
        return;
      }
    }


    // End iterators are represented by `slot_in_bucket_ == slots_per_bucket`.
    //
    // Default-constructed iterators are represented by `slot_in_bucket == -1`.
    BucketPtr bucket_;
    size_t slot_in_bucket_ = kDefaultConstructedSlot;
  };

  class const_iterator {
    friend class graveyard_raw_hash_set;

   public:
    using iterator_category = typename iterator::iterator_category;
    using value_type = typename graveyard_raw_hash_set::value_type;
    using reference = typename graveyard_raw_hash_set::const_reference;
    using pointer = typename graveyard_raw_hash_set::const_pointer;
    using difference_type = typename graveyard_raw_hash_set::difference_type;

    const_iterator() = default;
    // Implicit construction from iterator.
    const_iterator(iterator i) : inner_(std::move(i)) {}  // NOLINT

    reference operator*() const { return *inner_; }
    pointer operator->() const { return inner_.operator->(); }

    const_iterator& operator++() {
      ++inner_;
      return *this;
    }
    const_iterator operator++(int) { return inner_++; }

    friend bool operator==(const const_iterator& a, const const_iterator& b) {
      return a.inner_ == b.inner_;
    }
    friend bool operator!=(const const_iterator& a, const const_iterator& b) {
      return !(a == b);
    }

   private:
    const_iterator(const ctrl_t* ctrl, const slot_type* slot,
                   const GenerationType* gen)
        : inner_(const_cast<ctrl_t*>(ctrl), const_cast<slot_type*>(slot), gen) {
    }

    iterator inner_;
  };

  using node_type = node_handle<Policy, hash_policy_traits<Policy>, Alloc>;
  using insert_return_type = InsertReturnType<iterator, node_type>;

  // Note: can't use `= default` due to non-default noexcept (causes
  // problems for some compilers). NOLINTNEXTLINE
  graveyard_raw_hash_set() noexcept(
      std::is_nothrow_default_constructible<hasher>::value &&
      std::is_nothrow_default_constructible<key_equal>::value &&
      std::is_nothrow_default_constructible<allocator_type>::value) {}

  ABSL_ATTRIBUTE_NOINLINE explicit graveyard_raw_hash_set(
      size_t bucket_count, const hasher& hash = hasher(),
      const key_equal& eq = key_equal(),
      const allocator_type& alloc = allocator_type())
      : settings_(common_fields{}, hash, eq, alloc) {
    if (bucket_count) {
      allocate_slots();
      auto bucket_pointer = common().buckets_;
      for (size_t i = 0; i < bucket_count; ++i, ++bucket_pointer) {
        bucket_pointer.SetNotLastAndSearchDistanceToZero();
      }
      common().buckets_[bucket_count - 1].SetLast();
    }
  }

  graveyard_raw_hash_set(size_t bucket_count, const hasher& hash,
               const allocator_type& alloc)
      : graveyard_raw_hash_set(bucket_count, hash, key_equal(), alloc) {}

  graveyard_raw_hash_set(size_t bucket_count, const allocator_type& alloc)
      : graveyard_raw_hash_set(bucket_count, hasher(), key_equal(), alloc) {}

  explicit graveyard_raw_hash_set(const allocator_type& alloc)
      : graveyard_raw_hash_set(0, hasher(), key_equal(), alloc) {}

  template <class InputIter>
  graveyard_raw_hash_set(InputIter first, InputIter last, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : graveyard_raw_hash_set(SelectBucketCountForIterRange(first, last, bucket_count),
                     hash, eq, alloc) {
    insert(first, last);
  }

  template <class InputIter>
  graveyard_raw_hash_set(InputIter first, InputIter last, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : graveyard_raw_hash_set(first, last, bucket_count, hash, key_equal(), alloc) {}

  template <class InputIter>
  graveyard_raw_hash_set(InputIter first, InputIter last, size_t bucket_count,
               const allocator_type& alloc)
      : graveyard_raw_hash_set(first, last, bucket_count, hasher(), key_equal(), alloc) {}

  template <class InputIter>
  graveyard_raw_hash_set(InputIter first, InputIter last, const allocator_type& alloc)
      : graveyard_raw_hash_set(first, last, 0, hasher(), key_equal(), alloc) {}

  // Instead of accepting std::initializer_list<value_type> as the first
  // argument like std::unordered_set<value_type> does, we have two overloads
  // that accept std::initializer_list<T> and std::initializer_list<init_type>.
  // This is advantageous for performance.
  //
  //   // Turns {"abc", "def"} into std::initializer_list<std::string>, then
  //   // copies the strings into the set.
  //   std::unordered_set<std::string> s = {"abc", "def"};
  //
  //   // Turns {"abc", "def"} into std::initializer_list<const char*>, then
  //   // copies the strings into the set.
  //   absl::flat_hash_set<std::string> s = {"abc", "def"};
  //
  // The same trick is used in insert().
  //
  // The enabler is necessary to prevent this constructor from triggering where
  // the copy constructor is meant to be called.
  //
  //   absl::flat_hash_set<int> a, b{a};
  //
  // RequiresNotInit<T> is a workaround for gcc prior to 7.1.
  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  graveyard_raw_hash_set(std::initializer_list<T> init, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : graveyard_raw_hash_set(init.begin(), init.end(), bucket_count, hash, eq, alloc) {}

  graveyard_raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : graveyard_raw_hash_set(init.begin(), init.end(), bucket_count, hash, eq, alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  graveyard_raw_hash_set(std::initializer_list<T> init, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : graveyard_raw_hash_set(init, bucket_count, hash, key_equal(), alloc) {}

  graveyard_raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : graveyard_raw_hash_set(init, bucket_count, hash, key_equal(), alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  graveyard_raw_hash_set(std::initializer_list<T> init, size_t bucket_count,
               const allocator_type& alloc)
      : graveyard_raw_hash_set(init, bucket_count, hasher(), key_equal(), alloc) {}

  graveyard_raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count,
               const allocator_type& alloc)
      : graveyard_raw_hash_set(init, bucket_count, hasher(), key_equal(), alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  graveyard_raw_hash_set(std::initializer_list<T> init, const allocator_type& alloc)
      : graveyard_raw_hash_set(init, 0, hasher(), key_equal(), alloc) {}

  graveyard_raw_hash_set(std::initializer_list<init_type> init,
               const allocator_type& alloc)
      : graveyard_raw_hash_set(init, 0, hasher(), key_equal(), alloc) {}

  graveyard_raw_hash_set(const graveyard_raw_hash_set& that)
      : graveyard_raw_hash_set(that, AllocTraits::select_on_container_copy_construction(
                               that.alloc_ref())) {}

  graveyard_raw_hash_set(const graveyard_raw_hash_set& that, const allocator_type& a)
      : graveyard_raw_hash_set(0, that.hash_ref(), that.eq_ref(), a) {
    reserve(that.size());
    // Because the table is guaranteed to be empty, we can do something faster
    // than a full `insert`.
    for (const auto& v : that) {
      const size_t hash = PolicyTraits::apply(HashElement{hash_ref()}, v);
      auto target = find_first_non_full_outofline(common(), hash);
      SetCtrl(common(), target.offset, H2(hash), sizeof(slot_type));
      emplace_at(target.offset, v);
      common().maybe_increment_generation_on_insert();
      infoz().RecordInsert(hash, target.probe_length);
    }
    common().size_ = that.size();
    growth_left() -= that.size();
  }

  ABSL_ATTRIBUTE_NOINLINE graveyard_raw_hash_set(graveyard_raw_hash_set&& that) noexcept(
      std::is_nothrow_copy_constructible<hasher>::value &&
      std::is_nothrow_copy_constructible<key_equal>::value &&
      std::is_nothrow_copy_constructible<allocator_type>::value)
      :  // Hash, equality and allocator are copied instead of moved because
         // `that` must be left valid. If Hash is std::function<Key>, moving it
         // would create a nullptr functor that cannot be called.
        settings_(absl::exchange(that.common(), common_fields{}),
                  that.hash_ref(), that.eq_ref(), that.alloc_ref()) {}

  graveyard_raw_hash_set(graveyard_raw_hash_set&& that, const allocator_type& a)
      : settings_(common_fields{}, that.hash_ref(), that.eq_ref(), a) {
    if (a == that.alloc_ref()) {
      std::swap(common(), that.common());
    } else {
      reserve(that.size());
      // Note: this will copy elements of dense_set and unordered_set instead of
      // moving them. This can be fixed if it ever becomes an issue.
      for (auto& elem : that) insert(std::move(elem));
    }
  }

  graveyard_raw_hash_set& operator=(const graveyard_raw_hash_set& that) {
    graveyard_raw_hash_set tmp(that,
                     AllocTraits::propagate_on_container_copy_assignment::value
                         ? that.alloc_ref()
                         : alloc_ref());
    swap(tmp);
    return *this;
  }

  graveyard_raw_hash_set& operator=(graveyard_raw_hash_set&& that) noexcept(
      absl::allocator_traits<allocator_type>::is_always_equal::value &&
      std::is_nothrow_move_assignable<hasher>::value &&
      std::is_nothrow_move_assignable<key_equal>::value) {
    // TODO(sbenza): We should only use the operations from the noexcept clause
    // to make sure we actually adhere to that contract.
    // NOLINTNEXTLINE: not returning *this for performance.
    return move_assign(
        std::move(that),
        typename AllocTraits::propagate_on_container_move_assignment());
  }

  ~graveyard_raw_hash_set() {
    const size_t cap = capacity();
    if (!cap) return;
    destroy_slots();

    // Unpoison before returning the memory to the allocator.
    SanitizerUnpoisonMemoryRegion(slot_array(), sizeof(slot_type) * cap);
    Deallocate<alignof(slot_type)>(
        &alloc_ref(), control(),
        AllocSize(cap, sizeof(slot_type), alignof(slot_type)));

    infoz().Unregister();
  }

  iterator begin() ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto it = iterator_at(0);
    it.skip_empty_or_deleted();
    return it;
  }
  iterator end() ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return iterator(common().generation_ptr());
  }

  const_iterator begin() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return const_cast<graveyard_raw_hash_set*>(this)->begin();
  }
  const_iterator end() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return iterator(common().generation_ptr());
  }
  const_iterator cbegin() const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return begin();
  }
  const_iterator cend() const ABSL_ATTRIBUTE_LIFETIME_BOUND { return end(); }

  bool empty() const { return !size(); }
  size_t size() const { return common().size_; }
  size_t capacity() const { return common().capacity_; }
  size_t max_size() const { return (std::numeric_limits<size_t>::max)(); }

  ABSL_ATTRIBUTE_REINITIALIZES void clear() {
    // Iterating over this container is O(bucket_count()). When bucket_count()
    // is much greater than size(), iteration becomes prohibitively expensive.
    // For clear() it is more important to reuse the allocated array when the
    // container is small because allocation takes comparatively long time
    // compared to destruction of the elements of the container. So we pick the
    // largest bucket_count() threshold for which iteration is still fast and
    // past that we simply deallocate the array.
    const size_t cap = capacity();
    if (cap == 0) {
      // Already guaranteed to be empty; so nothing to do.
    } else {
      destroy_slots();
      ClearBackingArray(common(), GetPolicyFunctions(),
                        /*reuse=*/cap < 128);
    }
    common().set_reserved_growth(0);
  }

  inline void destroy_slots() {
    for (BucketPointer bp = common().buckets_; true; ++bp) {
      for (size_t i = 0; i < slots_per_bucket; ++i) {
        if (bp.SlotIsFull(i)) {
          PolicyTraits::destroy(&alloc_ref(), bp.GetSlot(i));
        }
      }
      if (bp.IsLast()) {
        break;
      }
    }
  }

  // This overload kicks in when the argument is an rvalue of insertable and
  // decomposable type other than init_type.
  //
  //   flat_hash_map<std::string, int> m;
  //   m.insert(std::make_pair("abc", 42));
  // TODO(cheshire): A type alias T2 is introduced as a workaround for the nvcc
  // bug.
  template <class T, RequiresInsertable<T> = 0, class T2 = T,
            typename std::enable_if<IsDecomposable<T2>::value, int>::type = 0,
            T* = nullptr>
  std::pair<iterator, bool> insert(T&& value) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return emplace(std::forward<T>(value));
  }

  // This overload kicks in when the argument is a bitfield or an lvalue of
  // insertable and decomposable type.
  //
  //   union { int n : 1; };
  //   flat_hash_set<int> s;
  //   s.insert(n);
  //
  //   flat_hash_set<std::string> s;
  //   const char* p = "hello";
  //   s.insert(p);
  //
  template <
      class T, RequiresInsertable<const T&> = 0,
      typename std::enable_if<IsDecomposable<const T&>::value, int>::type = 0>
  std::pair<iterator, bool> insert(const T& value)
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return emplace(value);
  }

  // This overload kicks in when the argument is an rvalue of init_type. Its
  // purpose is to handle brace-init-list arguments.
  //
  //   flat_hash_map<std::string, int> s;
  //   s.insert({"abc", 42});
  std::pair<iterator, bool> insert(init_type&& value)
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return emplace(std::move(value));
  }

  // TODO(cheshire): A type alias T2 is introduced as a workaround for the nvcc
  // bug.
  template <class T, RequiresInsertable<T> = 0, class T2 = T,
            typename std::enable_if<IsDecomposable<T2>::value, int>::type = 0,
            T* = nullptr>
  iterator insert(const_iterator, T&& value) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return insert(std::forward<T>(value)).first;
  }

  template <
      class T, RequiresInsertable<const T&> = 0,
      typename std::enable_if<IsDecomposable<const T&>::value, int>::type = 0>
  iterator insert(const_iterator,
                  const T& value) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return insert(value).first;
  }

  iterator insert(const_iterator,
                  init_type&& value) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return insert(std::move(value)).first;
  }

  template <class InputIt>
  void insert(InputIt first, InputIt last) {
    for (; first != last; ++first) emplace(*first);
  }

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<const T&> = 0>
  void insert(std::initializer_list<T> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  void insert(std::initializer_list<init_type> ilist) {
    insert(ilist.begin(), ilist.end());
  }

  insert_return_type insert(node_type&& node) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    if (!node) return {end(), false, node_type()};
    const auto& elem = PolicyTraits::element(CommonAccess::GetSlot(node));
    auto res = PolicyTraits::apply(
        InsertSlot<false>{*this, std::move(*CommonAccess::GetSlot(node))},
        elem);
    if (res.second) {
      CommonAccess::Reset(&node);
      return {res.first, true, node_type()};
    } else {
      return {res.first, false, std::move(node)};
    }
  }

  iterator insert(const_iterator,
                  node_type&& node) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto res = insert(std::move(node));
    node = std::move(res.node);
    return res.position;
  }

  // This overload kicks in if we can deduce the key from args. This enables us
  // to avoid constructing value_type if an entry with the same key already
  // exists.
  //
  // For example:
  //
  //   flat_hash_map<std::string, std::string> m = {{"abc", "def"}};
  //   // Creates no std::string copies and makes no heap allocations.
  //   m.emplace("abc", "xyz");
  template <class... Args, typename std::enable_if<
                               IsDecomposable<Args...>::value, int>::type = 0>
  std::pair<iterator, bool> emplace(Args&&... args)
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return PolicyTraits::apply(EmplaceDecomposable{*this},
                               std::forward<Args>(args)...);
  }

  // This overload kicks in if we cannot deduce the key from args. It constructs
  // value_type unconditionally and then either moves it into the table or
  // destroys.
  template <class... Args, typename std::enable_if<
                               !IsDecomposable<Args...>::value, int>::type = 0>
  std::pair<iterator, bool> emplace(Args&&... args)
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    alignas(slot_type) unsigned char raw[sizeof(slot_type)];
    slot_type* slot = reinterpret_cast<slot_type*>(&raw);

    PolicyTraits::construct(&alloc_ref(), slot, std::forward<Args>(args)...);
    const auto& elem = PolicyTraits::element(slot);
    return PolicyTraits::apply(InsertSlot<true>{*this, std::move(*slot)}, elem);
  }

  template <class... Args>
  iterator emplace_hint(const_iterator,
                        Args&&... args) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return emplace(std::forward<Args>(args)...).first;
  }

  // Extension API: support for lazy emplace.
  //
  // Looks up key in the table. If found, returns the iterator to the element.
  // Otherwise calls `f` with one argument of type `graveyard_raw_hash_set::constructor`.
  //
  // `f` must abide by several restrictions:
  //  - it MUST call `graveyard_raw_hash_set::constructor` with arguments as if a
  //    `graveyard_raw_hash_set::value_type` is constructed,
  //  - it MUST NOT access the container before the call to
  //    `graveyard_raw_hash_set::constructor`, and
  //  - it MUST NOT erase the lazily emplaced element.
  // Doing any of these is undefined behavior.
  //
  // For example:
  //
  //   std::unordered_set<ArenaString> s;
  //   // Makes ArenaStr even if "abc" is in the map.
  //   s.insert(ArenaString(&arena, "abc"));
  //
  //   flat_hash_set<ArenaStr> s;
  //   // Makes ArenaStr only if "abc" is not in the map.
  //   s.lazy_emplace("abc", [&](const constructor& ctor) {
  //     ctor(&arena, "abc");
  //   });
  //
  // WARNING: This API is currently experimental. If there is a way to implement
  // the same thing with the rest of the API, prefer that.
  class constructor {
    friend class graveyard_raw_hash_set;

   public:
    template <class... Args>
    void operator()(Args&&... args) const {
      assert(*slot_);
      PolicyTraits::construct(alloc_, *slot_, std::forward<Args>(args)...);
      *slot_ = nullptr;
    }

   private:
    constructor(allocator_type* a, slot_type** slot) : alloc_(a), slot_(slot) {}

    allocator_type* alloc_;
    slot_type** slot_;
  };

  template <class K = key_type, class F>
  iterator lazy_emplace(const key_arg<K>& key,
                        F&& f) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto res = find_or_prepare_insert(key);
    if (res.second) {
      slot_type* slot = slot_array() + res.first;
      std::forward<F>(f)(constructor(&alloc_ref(), &slot));
      assert(!slot);
    }
    return iterator_at(res.first);
  }

  // Extension API: support for heterogeneous keys.
  //
  //   std::unordered_set<std::string> s;
  //   // Turns "abc" into std::string.
  //   s.erase("abc");
  //
  //   flat_hash_set<std::string> s;
  //   // Uses "abc" directly without copying it into std::string.
  //   s.erase("abc");
  template <class K = key_type>
  size_type erase(const key_arg<K>& key) {
    auto it = find(key);
    if (it == end()) return 0;
    erase(it);
    return 1;
  }

  // Erases the element pointed to by `it`.  Unlike `std::unordered_set::erase`,
  // this method returns void to reduce algorithmic complexity to O(1).  The
  // iterator is invalidated, so any increment should be done before calling
  // erase.  In order to erase while iterating across a map, use the following
  // idiom (which also works for standard containers):
  //
  // for (auto it = m.begin(), end = m.end(); it != end;) {
  //   // `erase()` will invalidate `it`, so advance `it` first.
  //   auto copy_it = it++;
  //   if (<pred>) {
  //     m.erase(copy_it);
  //   }
  // }
  void erase(const_iterator cit) { erase(cit.inner_); }

  // This overload is necessary because otherwise erase<K>(const K&) would be
  // a better match if non-const iterator is passed as an argument.
  void erase(iterator it) {
    AssertIsFull(it.ctrl_, it.generation(), it.generation_ptr(), "erase()");
    PolicyTraits::destroy(&alloc_ref(), it.slot_);
    erase_meta_only(it);
  }

  iterator erase(const_iterator first,
                 const_iterator last) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    while (first != last) {
      erase(first++);
    }
    return last.inner_;
  }

  // Moves elements from `src` into `this`.
  // If the element already exists in `this`, it is left unmodified in `src`.
  template <typename H, typename E>
  void merge(graveyard_raw_hash_set<Policy, H, E, Alloc>& src) {  // NOLINT
    assert(this != &src);
    for (auto it = src.begin(), e = src.end(); it != e;) {
      auto next = std::next(it);
      if (PolicyTraits::apply(InsertSlot<false>{*this, std::move(*it.slot_)},
                              PolicyTraits::element(it.slot_))
              .second) {
        src.erase_meta_only(it);
      }
      it = next;
    }
  }

  template <typename H, typename E>
  void merge(graveyard_raw_hash_set<Policy, H, E, Alloc>&& src) {
    merge(src);
  }

  node_type extract(const_iterator position) {
    AssertIsFull(position.inner_.ctrl_, position.inner_.generation(),
                 position.inner_.generation_ptr(), "extract()");
    auto node =
        CommonAccess::Transfer<node_type>(alloc_ref(), position.inner_.slot_);
    erase_meta_only(position);
    return node;
  }

  template <
      class K = key_type,
      typename std::enable_if<!std::is_same<K, iterator>::value, int>::type = 0>
  node_type extract(const key_arg<K>& key) {
    auto it = find(key);
    return it == end() ? node_type() : extract(const_iterator{it});
  }

  void swap(graveyard_raw_hash_set& that) noexcept(
      IsNoThrowSwappable<hasher>() && IsNoThrowSwappable<key_equal>() &&
      IsNoThrowSwappable<allocator_type>(
          typename AllocTraits::propagate_on_container_swap{})) {
    using std::swap;
    swap(common(), that.common());
    swap(hash_ref(), that.hash_ref());
    swap(eq_ref(), that.eq_ref());
    SwapAlloc(alloc_ref(), that.alloc_ref(),
              typename AllocTraits::propagate_on_container_swap{});
  }

  void rehash(size_t n) {
    if (n == 0 && capacity() == 0) return;
    if (n == 0 && size() == 0) {
      ClearBackingArray(common(), GetPolicyFunctions(),
                        /*reuse=*/false);
      return;
    }

    // bitor is a faster way of doing `max` here. We will round up to the next
    // power-of-2-minus-1, so bitor is good enough.
    auto m = NormalizeCapacity(n | GrowthToLowerboundCapacity(size()));
    // n == 0 unconditionally rehashes as per the standard.
    if (n == 0 || m > capacity()) {
      resize(m);

      // This is after resize, to ensure that we have completed the allocation
      // and have potentially sampled the hashtable.
      infoz().RecordReservation(n);
    }
  }

  void reserve(size_t n) {
    if (n > size() + growth_left()) {
      size_t m = GrowthToLowerboundCapacity(n);
      resize(NormalizeCapacity(m));

      // This is after resize, to ensure that we have completed the allocation
      // and have potentially sampled the hashtable.
      infoz().RecordReservation(n);
    }
    common().reset_reserved_growth(n);
  }

  // Extension API: support for heterogeneous keys.
  //
  //   std::unordered_set<std::string> s;
  //   // Turns "abc" into std::string.
  //   s.count("abc");
  //
  //   ch_set<std::string> s;
  //   // Uses "abc" directly without copying it into std::string.
  //   s.count("abc");
  template <class K = key_type>
  size_t count(const key_arg<K>& key) const {
    return find(key) == end() ? 0 : 1;
  }

  // Issues CPU prefetch instructions for the memory needed to find or insert
  // a key.  Like all lookup functions, this support heterogeneous keys.
  //
  // NOTE: This is a very low level operation and should not be used without
  // specific benchmarks indicating its importance.
  template <class K = key_type>
  void prefetch(const key_arg<K>& key) const {
    (void)key;
    // Avoid probing if we won't be able to prefetch the addresses received.
#ifdef ABSL_HAVE_PREFETCH
    prefetch_heap_block();
    auto seq = probe(common(), hash_ref()(key));
    PrefetchToLocalCache(control() + seq.offset());
    PrefetchToLocalCache(slot_array() + seq.offset());
#endif  // ABSL_HAVE_PREFETCH
  }

  // The API of find() has two extensions.
  //
  // 1. The hash can be passed by the user. It must be equal to the hash of the
  // key.
  //
  // 2. The type of the key argument doesn't have to be key_type. This is so
  // called heterogeneous key support.
  template <class K = key_type>
  iterator find(const key_arg<K>& key,
                size_t hash) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto seq = probe(common(), hash);
    slot_type* slot_ptr = slot_array();
    const ctrl_t* ctrl = control();
    while (true) {
      Group g{ctrl + seq.offset()};
      for (uint32_t i : g.Match(H2(hash))) {
        if (ABSL_PREDICT_TRUE(PolicyTraits::apply(
                EqualElement<K>{key, eq_ref()},
                PolicyTraits::element(slot_ptr + seq.offset(i)))))
          return iterator_at(seq.offset(i));
      }
      if (ABSL_PREDICT_TRUE(g.MaskEmpty())) return end();
      seq.next();
      assert(seq.index() <= capacity() && "full table!");
    }
  }
  template <class K = key_type>
  iterator find(const key_arg<K>& key) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    prefetch_heap_block();
    return find(key, hash_ref()(key));
  }

  template <class K = key_type>
  const_iterator find(const key_arg<K>& key,
                      size_t hash) const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return const_cast<graveyard_raw_hash_set*>(this)->find(key, hash);
  }
  template <class K = key_type>
  const_iterator find(const key_arg<K>& key) const
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    prefetch_heap_block();
    return find(key, hash_ref()(key));
  }

  template <class K = key_type>
  bool contains(const key_arg<K>& key) const {
    return find(key) != end();
  }

  template <class K = key_type>
  std::pair<iterator, iterator> equal_range(const key_arg<K>& key)
      ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto it = find(key);
    if (it != end()) return {it, std::next(it)};
    return {it, it};
  }
  template <class K = key_type>
  std::pair<const_iterator, const_iterator> equal_range(
      const key_arg<K>& key) const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    auto it = find(key);
    if (it != end()) return {it, std::next(it)};
    return {it, it};
  }

  size_t bucket_count() const { return capacity(); }
  float load_factor() const {
    return capacity() ? static_cast<double>(size()) / capacity() : 0.0;
  }
  float max_load_factor() const { return 1.0f; }
  void max_load_factor(float) {
    // Does nothing.
  }

  hasher hash_function() const { return hash_ref(); }
  key_equal key_eq() const { return eq_ref(); }
  allocator_type get_allocator() const { return alloc_ref(); }

  friend bool operator==(const graveyard_raw_hash_set& a, const graveyard_raw_hash_set& b) {
    if (a.size() != b.size()) return false;
    const graveyard_raw_hash_set* outer = &a;
    const graveyard_raw_hash_set* inner = &b;
    if (outer->capacity() > inner->capacity()) std::swap(outer, inner);
    for (const value_type& elem : *outer)
      if (!inner->has_element(elem)) return false;
    return true;
  }

  friend bool operator!=(const graveyard_raw_hash_set& a, const graveyard_raw_hash_set& b) {
    return !(a == b);
  }

  template <typename H>
  friend typename std::enable_if<H::template is_hashable<value_type>::value,
                                 H>::type
  AbslHashValue(H h, const graveyard_raw_hash_set& s) {
    return H::combine(H::combine_unordered(std::move(h), s.begin(), s.end()),
                      s.size());
  }

  friend void swap(graveyard_raw_hash_set& a,
                   graveyard_raw_hash_set& b) noexcept(noexcept(a.swap(b))) {
    a.swap(b);
  }

 private:
  template <class Container, typename Enabler>
  friend struct absl::container_internal::hashtable_debug_internal::
      HashtableDebugAccess;

  struct FindElement {
    template <class K, class... Args>
    const_iterator operator()(const K& key, Args&&...) const {
      return s.find(key);
    }
    const graveyard_raw_hash_set& s;
  };

  struct HashElement {
    template <class K, class... Args>
    size_t operator()(const K& key, Args&&...) const {
      return h(key);
    }
    const hasher& h;
  };

  template <class K1>
  struct EqualElement {
    template <class K2, class... Args>
    bool operator()(const K2& lhs, Args&&...) const {
      return eq(lhs, rhs);
    }
    const K1& rhs;
    const key_equal& eq;
  };

  struct EmplaceDecomposable {
    template <class K, class... Args>
    std::pair<iterator, bool> operator()(const K& key, Args&&... args) const {
      auto res = s.find_or_prepare_insert(key);
      if (res.second) {
        s.emplace_at(res.first, std::forward<Args>(args)...);
      }
      return {s.iterator_at(res.first), res.second};
    }
    graveyard_raw_hash_set& s;
  };

  template <bool do_destroy>
  struct InsertSlot {
    template <class K, class... Args>
    std::pair<iterator, bool> operator()(const K& key, Args&&...) && {
      auto res = s.find_or_prepare_insert(key);
      if (res.second) {
        PolicyTraits::transfer(&s.alloc_ref(), s.slot_array() + res.first,
                               &slot);
      } else if (do_destroy) {
        PolicyTraits::destroy(&s.alloc_ref(), &slot);
      }
      return {s.iterator_at(res.first), res.second};
    }
    graveyard_raw_hash_set& s;
    // Constructed slot. Either moved into place or destroyed.
    slot_type&& slot;
  };

  // Erases, but does not destroy, the value pointed to by `it`.
  //
  // This merely updates the pertinent control byte. This can be used in
  // conjunction with Policy::transfer to move the object to another place.
  void erase_meta_only(const_iterator it) {
    EraseMetaOnly(common(), it.inner_.ctrl_, sizeof(slot_type));
  }

  // Allocates a backing array for `self` and initializes its control bytes.
  // This reads `capacity` and updates all other fields based on the result of
  // the allocation.
  //
  // This does not free the currently held array; `capacity` must be nonzero.
  inline void allocate_slots() {
    // People are often sloppy with the exact type of their allocator (sometimes
    // it has an extra const or is missing the pair, but rebinds made it work
    // anyway).
    using CharAlloc =
        typename absl::allocator_traits<Alloc>::template rebind_alloc<char>;

    assert(common().capacity_);
    // Folks with custom allocators often make unwarranted assumptions about the
    // behavior of their classes vis-a-vis trivial destructability and what
    // calls they will or won't make.  Avoid sampling for people with custom
    // allocators to get us out of this mess.  This is not a hard guarantee but
    // a workaround while we plan the exact guarantee we want to provide.
    const size_t sample_size =
        (std::is_same<Alloc, std::allocator<char>>::value)
          ? sizeof(slot_type)
          : 0;

    const size_t cap = common().capacity_;
    char* mem = static_cast<char*>(
        Allocate<alignof(slot_type)>(&CharAlloc(alloc_ref()), AllocSize(cap, sizeof(slot_type), alignof(slot_type))));

    const GenerationType old_generation = common().generation();
    common().set_generation_ptr(
        reinterpret_cast<GenerationType*>(mem + GenerationOffset(cap)));
    common().set_generation(NextGeneration(old_generation));

    common().buckets_ = BucketPtr(mem);
    if (sample_size) {
      common().infoz() = Sample(sample_size);
    }
    common().infoz().RecordStorageChanged(common().size_, cap);
  }

  ABSL_ATTRIBUTE_NOINLINE void resize(size_t new_capacity) {
    assert(IsValidCapacity(new_capacity));
    auto* old_ctrl = control();
    auto* old_slots = slot_array();
    const size_t old_capacity = common().capacity_;
    common().capacity_ = new_capacity;
    initialize_slots();

    auto* new_slots = slot_array();
    size_t total_probe_length = 0;
    for (size_t i = 0; i != old_capacity; ++i) {
      if (IsFull(old_ctrl[i])) {
        size_t hash = PolicyTraits::apply(HashElement{hash_ref()},
                                          PolicyTraits::element(old_slots + i));
        auto target = find_first_non_full(common(), hash);
        size_t new_i = target.offset;
        total_probe_length += target.probe_length;
        SetCtrl(common(), new_i, H2(hash), sizeof(slot_type));
        PolicyTraits::transfer(&alloc_ref(), new_slots + new_i, old_slots + i);
      }
    }
    if (old_capacity) {
      SanitizerUnpoisonMemoryRegion(old_slots,
                                    sizeof(slot_type) * old_capacity);
      Deallocate<alignof(slot_type)>(
          &alloc_ref(), old_ctrl,
          AllocSize(old_capacity, sizeof(slot_type), alignof(slot_type)));
    }
    infoz().RecordRehash(total_probe_length);
  }

  // Prunes control bytes to remove as many tombstones as possible.
  //
  // See the comment on `rehash_and_grow_if_necessary()`.
  inline void drop_deletes_without_resize() {
    // Stack-allocate space for swapping elements.
    alignas(slot_type) unsigned char tmp[sizeof(slot_type)];
    DropDeletesWithoutResize(common(), GetPolicyFunctions(), tmp);
  }

  // Called whenever the table *might* need to conditionally grow.
  //
  // This function is an optimization opportunity to perform a rehash even when
  // growth is unnecessary, because vacating tombstones is beneficial for
  // performance in the long-run.
  void rehash_and_grow_if_necessary() {
    const size_t cap = capacity();
    if (cap > Group::kWidth &&
        // Do these calculations in 64-bit to avoid overflow.
        size() * uint64_t{32} <= cap * uint64_t{25}) {
      // Squash DELETED without growing if there is enough capacity.
      //
      // Rehash in place if the current size is <= 25/32 of capacity.
      // Rationale for such a high factor: 1) drop_deletes_without_resize() is
      // faster than resize, and 2) it takes quite a bit of work to add
      // tombstones.  In the worst case, seems to take approximately 4
      // insert/erase pairs to create a single tombstone and so if we are
      // rehashing because of tombstones, we can afford to rehash-in-place as
      // long as we are reclaiming at least 1/8 the capacity without doing more
      // than 2X the work.  (Where "work" is defined to be size() for rehashing
      // or rehashing in place, and 1 for an insert or erase.)  But rehashing in
      // place is faster per operation than inserting or even doubling the size
      // of the table, so we actually afford to reclaim even less space from a
      // resize-in-place.  The decision is to rehash in place if we can reclaim
      // at about 1/8th of the usable capacity (specifically 3/28 of the
      // capacity) which means that the total cost of rehashing will be a small
      // fraction of the total work.
      //
      // Here is output of an experiment using the BM_CacheInSteadyState
      // benchmark running the old case (where we rehash-in-place only if we can
      // reclaim at least 7/16*capacity) vs. this code (which rehashes in place
      // if we can recover 3/32*capacity).
      //
      // Note that although in the worst-case number of rehashes jumped up from
      // 15 to 190, but the number of operations per second is almost the same.
      //
      // Abridged output of running BM_CacheInSteadyState benchmark from
      // graveyard_raw_hash_set_benchmark.   N is the number of insert/erase operations.
      //
      //      | OLD (recover >= 7/16        | NEW (recover >= 3/32)
      // size |    N/s LoadFactor NRehashes |    N/s LoadFactor NRehashes
      //  448 | 145284       0.44        18 | 140118       0.44        19
      //  493 | 152546       0.24        11 | 151417       0.48        28
      //  538 | 151439       0.26        11 | 151152       0.53        38
      //  583 | 151765       0.28        11 | 150572       0.57        50
      //  628 | 150241       0.31        11 | 150853       0.61        66
      //  672 | 149602       0.33        12 | 150110       0.66        90
      //  717 | 149998       0.35        12 | 149531       0.70       129
      //  762 | 149836       0.37        13 | 148559       0.74       190
      //  807 | 149736       0.39        14 | 151107       0.39        14
      //  852 | 150204       0.42        15 | 151019       0.42        15
      drop_deletes_without_resize();
    } else {
      // Otherwise grow the container.
      resize(NextCapacity(cap));
    }
  }

  bool has_element(const value_type& elem) const {
    size_t hash = PolicyTraits::apply(HashElement{hash_ref()}, elem);
    auto seq = probe(common(), hash);
    const ctrl_t* ctrl = control();
    while (true) {
      Group g{ctrl + seq.offset()};
      for (uint32_t i : g.Match(H2(hash))) {
        if (ABSL_PREDICT_TRUE(
                PolicyTraits::element(slot_array() + seq.offset(i)) == elem))
          return true;
      }
      if (ABSL_PREDICT_TRUE(g.MaskEmpty())) return false;
      seq.next();
      assert(seq.index() <= capacity() && "full table!");
    }
    return false;
  }

  // TODO(alkis): Optimize this assuming *this and that don't overlap.
  graveyard_raw_hash_set& move_assign(graveyard_raw_hash_set&& that, std::true_type) {
    graveyard_raw_hash_set tmp(std::move(that));
    swap(tmp);
    return *this;
  }
  graveyard_raw_hash_set& move_assign(graveyard_raw_hash_set&& that, std::false_type) {
    graveyard_raw_hash_set tmp(std::move(that), alloc_ref());
    swap(tmp);
    return *this;
  }

 protected:
  // Attempts to find `key` in the table; if it isn't found, returns a slot that
  // the value can be inserted into, with the control byte already set to
  // `key`'s H2.
  template <class K>
  std::pair<size_t, bool> find_or_prepare_insert(const K& key) {
    prefetch_heap_block();
    auto hash = hash_ref()(key);
    auto seq = probe(common(), hash);
    const ctrl_t* ctrl = control();
    while (true) {
      Group g{ctrl + seq.offset()};
      for (uint32_t i : g.Match(H2(hash))) {
        if (ABSL_PREDICT_TRUE(PolicyTraits::apply(
                EqualElement<K>{key, eq_ref()},
                PolicyTraits::element(slot_array() + seq.offset(i)))))
          return {seq.offset(i), false};
      }
      if (ABSL_PREDICT_TRUE(g.MaskEmpty())) break;
      seq.next();
      assert(seq.index() <= capacity() && "full table!");
    }
    return {prepare_insert(hash), true};
  }

  // Given the hash of a value not currently in the table, finds the next
  // viable slot index to insert it at.
  //
  // REQUIRES: At least one non-full slot available.
  size_t prepare_insert(size_t hash) ABSL_ATTRIBUTE_NOINLINE {
    const bool rehash_for_bug_detection =
        common().should_rehash_for_bug_detection_on_insert();
    if (rehash_for_bug_detection) {
      // Move to a different heap allocation in order to detect bugs.
      const size_t cap = capacity();
      resize(growth_left() > 0 ? cap : NextCapacity(cap));
    }
    auto target = find_first_non_full(common(), hash);
    if (!rehash_for_bug_detection &&
        ABSL_PREDICT_FALSE(growth_left() == 0 &&
                           !IsDeleted(control()[target.offset]))) {
      rehash_and_grow_if_necessary();
      target = find_first_non_full(common(), hash);
    }
    ++common().size_;
    growth_left() -= IsEmpty(control()[target.offset]);
    SetCtrl(common(), target.offset, H2(hash), sizeof(slot_type));
    common().maybe_increment_generation_on_insert();
    infoz().RecordInsert(hash, target.probe_length);
    return target.offset;
  }

  // Constructs the value in the space pointed by the iterator. This only works
  // after an unsuccessful find_or_prepare_insert() and before any other
  // modifications happen in the graveyard_raw_hash_set.
  //
  // PRECONDITION: i is an index returned from find_or_prepare_insert(k), where
  // k is the key decomposed from `forward<Args>(args)...`, and the bool
  // returned by find_or_prepare_insert(k) was true.
  // POSTCONDITION: *m.iterator_at(i) == value_type(forward<Args>(args)...).
  template <class... Args>
  void emplace_at(size_t i, Args&&... args) {
    PolicyTraits::construct(&alloc_ref(), slot_array() + i,
                            std::forward<Args>(args)...);

    assert(PolicyTraits::apply(FindElement{*this}, *iterator_at(i)) ==
               iterator_at(i) &&
           "constructed value does not match the lookup key");
  }

  iterator iterator_at(size_t i) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return {control() + i, slot_array() + i, common().generation_ptr()};
  }
  const_iterator iterator_at(size_t i) const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return {control() + i, slot_array() + i, common().generation_ptr()};
  }

 private:
  friend struct RawHashSetTestOnlyAccess;

  // The number of slots we can still fill without needing to rehash.
  //
  // This is stored separately due to tombstones: we do not include tombstones
  // in the growth capacity, because we'd like to rehash when the table is
  // otherwise filled with tombstones: otherwise, probe sequences might get
  // unacceptably long without triggering a rehash. Callers can also force a
  // rehash via the standard `rehash(0)`, which will recompute this value as a
  // side-effect.
  //
  // See `CapacityToGrowth()`.
  size_t& growth_left() { return common().growth_left(); }

  // Prefetch the heap-allocated memory region to resolve potential TLB and
  // cache misses. This is intended to overlap with execution of calculating the
  // hash for a key.
  void prefetch_heap_block() const {
#if ABSL_HAVE_BUILTIN(__builtin_prefetch) || defined(__GNUC__)
    __builtin_prefetch(control(), 0, 1);
#endif
  }

  common_fields& common() { return settings_.template get<0>(); }
  const common_fields& common() const { return settings_.template get<0>(); }

  ctrl_t* control() const { return common().control_; }
  slot_type* slot_array() const {
    return static_cast<slot_type*>(common().slots_);
  }
  HashtablezInfoHandle& infoz() { return common().infoz(); }

  hasher& hash_ref() { return settings_.template get<1>(); }
  const hasher& hash_ref() const { return settings_.template get<1>(); }
  key_equal& eq_ref() { return settings_.template get<2>(); }
  const key_equal& eq_ref() const { return settings_.template get<2>(); }
  allocator_type& alloc_ref() { return settings_.template get<3>(); }
  const allocator_type& alloc_ref() const {
    return settings_.template get<3>();
  }

  // Make type-specific functions for this type's PolicyFunctions struct.
  static size_t hash_slot_fn(void* set, void* slot) {
    auto* h = static_cast<graveyard_raw_hash_set*>(set);
    return PolicyTraits::apply(
        HashElement{h->hash_ref()},
        PolicyTraits::element(static_cast<slot_type*>(slot)));
  }
  static void transfer_slot_fn(void* set, void* dst, void* src) {
    auto* h = static_cast<graveyard_raw_hash_set*>(set);
    PolicyTraits::transfer(&h->alloc_ref(), static_cast<slot_type*>(dst),
                           static_cast<slot_type*>(src));
  }
  // Note: dealloc_fn will only be used if we have a non-standard allocator.
  static void dealloc_fn(void* set, const PolicyFunctions&, ctrl_t* ctrl,
                         void* slot_mem, size_t n) {
    auto* h = static_cast<graveyard_raw_hash_set*>(set);

    // Unpoison before returning the memory to the allocator.
    SanitizerUnpoisonMemoryRegion(slot_mem, sizeof(slot_type) * n);

    Deallocate<alignof(slot_type)>(
        &h->alloc_ref(), ctrl,
        AllocSize(n, sizeof(slot_type), alignof(slot_type)));
  }

  static const PolicyFunctions& GetPolicyFunctions() {
    static constexpr PolicyFunctions value = {
        sizeof(slot_type),
        &graveyard_raw_hash_set::hash_slot_fn,
        PolicyTraits::transfer_uses_memcpy()
            ? TransferRelocatable<sizeof(slot_type)>
            : &graveyard_raw_hash_set::transfer_slot_fn,
        (std::is_same<SlotAlloc, std::allocator<slot_type>>::value
             ? &DeallocateStandard<alignof(slot_type)>
             : &graveyard_raw_hash_set::dealloc_fn),
    };
    return value;
  }

  // Bundle together CommonFields plus other objects which might be empty.
  // CompressedTuple will ensure that sizeof is not affected by any of the empty
  // fields that occur after CommonFields.
  absl::container_internal::CompressedTuple<common_fields, hasher, key_equal,
                                            allocator_type>
      settings_{common_fields{}, hasher{}, key_equal{}, allocator_type{}};
};

// Erases all elements that satisfy the predicate `pred` from the container `c`.
template <typename P, typename H, typename E, typename A, typename Predicate>
typename graveyard_raw_hash_set<P, H, E, A>::size_type EraseIf(
    Predicate& pred, graveyard_raw_hash_set<P, H, E, A>* c) {
  const auto initial_size = c->size();
  for (auto it = c->begin(), last = c->end(); it != last;) {
    if (pred(*it)) {
      c->erase(it++);
    } else {
      ++it;
    }
  }
  return initial_size - c->size();
}

}  // namespace graveyard_container_internal

namespace container_internal::hashtable_debug_internal {
template <typename Set>
struct HashtableDebugAccess<Set, absl::void_t<typename Set::graveyard_raw_hash_set>> {
  using ctrl_t = graveyard_container_internal::ctrl_t;
  using Group = graveyard_container_internal::Group;
  using Traits = typename Set::PolicyTraits;
  using Slot = typename Traits::slot_type;

  static size_t GetNumProbes(const Set& set,
                             const typename Set::key_type& key) {
    size_t num_probes = 0;
    size_t hash = set.hash_ref()(key);
    auto seq = probe(set.common(), hash);
    const ctrl_t* ctrl = set.control();
    while (true) {
      Group g{ctrl + seq.offset()};
      for (uint32_t i : g.Match(graveyard_container_internal::H2(hash))) {
        if (Traits::apply(
                typename Set::template EqualElement<typename Set::key_type>{
                    key, set.eq_ref()},
                Traits::element(set.slot_array() + seq.offset(i))))
          return num_probes;
        ++num_probes;
      }
      if (g.MaskEmpty()) return num_probes;
      seq.next();
      ++num_probes;
    }
  }

  static size_t AllocatedByteSize(const Set& c) {
    size_t capacity = c.capacity();
    if (capacity == 0) return 0;
    size_t m = graveyard_container_internal::AllocSize(capacity, sizeof(Slot), alignof(Slot));

    size_t per_slot = Traits::space_used(static_cast<const Slot*>(nullptr));
    if (per_slot != ~size_t{}) {
      m += per_slot * c.size();
    } else {
      const ctrl_t* ctrl = c.control();
      for (size_t i = 0; i != capacity; ++i) {
        if (graveyard_container_internal::IsFull(ctrl[i])) {
          m += Traits::space_used(c.slot_array() + i);
        }
      }
    }
    return m;
  }

  static size_t LowerBoundAllocatedByteSize(size_t size) {
    size_t capacity = graveyard_container_internal::GrowthToLowerboundCapacity(size);
    if (capacity == 0) return 0;
    size_t m =
        graveyard_container_internal::AllocSize(graveyard_container_internal::NormalizeCapacity(capacity), sizeof(Slot), alignof(Slot));
    size_t per_slot = Traits::space_used(static_cast<const Slot*>(nullptr));
    if (per_slot != ~size_t{}) {
      m += per_slot * size;
    }
    return m;
  }
};

}  // namespace hashtable_debug_internal
ABSL_NAMESPACE_END
}  // namespace absl

#undef ABSL_SWISSTABLE_ENABLE_GENERATIONS

#endif  // ABSL_CONTAINER_INTERNAL_GRAVEYARD_RAW_HASH_SET_H
