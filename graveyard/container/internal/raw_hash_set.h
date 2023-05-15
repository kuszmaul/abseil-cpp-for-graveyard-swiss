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
// An open-addressing hashtable with linear probing and graveyard hashing.
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
// A raw_hash_set's backing array comprises *bins*.  Each *bin* comprises
// *control bytes*, a *search distance*, and *end sentinal*, and slots.  The
// *control bytes* are an array (typically of length 14) bytes.  The *slots* are
// an array that can hold values.  In most cases there are the same number of
// slots as control bytes (but for very small tables, there are fewer slots than
// control bytes).
//
// Each control byte contains information about whether there is a value, and if
// there is a value, whether that value is *disordered*, as well as a hash
// called the H2 hash.
//
// We compute two separate hashes, `H1` and `H2`, from the hash of an object.
// `H1(hash(x))` is a bin number, and essentially the starting point for the
// probe sequence. `H2(hash(x))` is a number in [0, 127] used to filter out
// objects that cannot possibly be the one we are looking for.
//
// The encoding is
//   bit 7:  the value, if present, is disordered.  Always 0 for empty slots.
//   bits 6:0:
//    contains 127 if there is no corresponding value, and
//    otherwise cantains H2, a number in [0, 127).
//
// Thus a bin is the following pseudo-struct:
//   struct Bin {
//     // The capacity is typically 14, but is sometimes other values to achieve
//     // better cache alignment.
//     static size_t BinCapacity = 14;
//     ctrl_t ctrl[BinCapacity];
//     uint16_t is_last_bin : 1;
//     uint16_t search_distance : 7;
//     // lots may actually be shorter than `BinCapacity` for very small tables.
//     slot_type slots[BinCapacity];
//   };
//   struct ctrl_t {
//     uint8_t is_disordered : 1;
//     uint8_t h2 : 7;  // 127 means empty
//   };
//
//  The backing array is an array of `Bin`s.  The number of bins is `
//  The backing array is thus the following pseudo-struct:
//    struct {
//      // H1 is always in [0, logical_bin_count).
//      size_t logical_bin_count;
//      // `physical_bin_count` can be slightly larger than `logical_bin_count`.
//      size_t physical_bin_count;
//      Bin[physical_bin_count];
//    };
//
// # Table operations.
//
// The key operations are `insert`, `find`, and `erase`.
//
// Since `insert` and `erase` are implemented in terms of `find`, we describe
// `find` first. To `find` a value `x`, we compute `hash(x)`. From `H1(hash(x))`
// we identify the first bin we will look in.  We also identify the
// search_distance for that bin.  We will visit up to `search_distance` bins
// starting at the first bin.
//
// For each Bin we examine the `ctrl` bytes looking for occupied slots with a h2
// control byte equal to `H2(hash(x))`.  Each candidate slot `y` is compared
// with `x`; if `x == y`, we are done and return `&y`; otherwise we continue to
// the next probe index.  (Note that unlike swiss raw_hash_map, the is no
// separate tombstone: there are only empty slos.)
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
// expected number of objects with an H2 match is then `k/127`.  If the load
// factor is `1-1/x` then we end up looking at about x slots (rounding up to
// 14).  Simulations indicate for a load factor of 75%, the number of slots we
// need to look as is ??? per `find`.  TODO: Get this data.
//
// If the probe sequence would go off the end of the physical_bin_count, we wrap
// around to bin zero.
//
// `insert` is implemented in terms of `unchecked_insert`, which inserts a value
// presumed to not be in the table (violating this requirement will cause the
// table to behave erratically). Given `x` and its hash `hash(x)`, to insert it,
// we construct a probe sequence again, starting at the preferred bin of `x`,
// and searching arbitrarily far to find the first bin with with an unoccupied
// slot. We place `x` into the first such slot in the group and mark it as full
// with `x`'s H2.  We set the search distance of the preferred bin to the
// maximum of its old value and the distance to the found bin.  Any newly
// inserted value is marked as disordered.
//
// To `insert`, we compose `unchecked_insert` with `find`. We compute `h(x)` and
// perform a `find` to see if it's already present; if it is, we're done. If
// it's not, we may decide the table is getting overcrowded; in this case, we
// allocate a bigger array, move each element of the table into the new array,
// and discard the old backing array. At this point, we may `unchecked_insert`
// the value `x`.
//
// Whenever we move elements from one Bin array to another, we take advantage of
// the fact that the elements are mostly sorted by H1.  In particular we
// maintain the following invariant:
//
//   If `x` and `y` are both in the hash table and the position of `x` is before
//   `y` and neither `x` or `y` is marked as `disordered` in its control byte,
//   then `H1(hash(x)) <= H2(hash(y))`
//
// We say that `x` is *disordered* if its control byte is marked as disordered,
// otherwise `x` is *ordered*.
//
// As we move items from the old array to the new array, we do a merge of the
// ordered values with the disordered values.  To do this we maintain two
// iterators: one scans through the ordered values, and the other scan through
// the disordered values, putting them into a heap.  One easy way to do this is
// to take all the disordered values and put them into the heap.  But we can be
// more careful, and need only put some of the disordered values into the heap,
// since the `search_distance` bounds how far we would have to look to find a
// value that could possibly have a hash smaller than the current ordered value.
// (Also, any value that is wrapped around is marked as disordered.  We keep the
// number of disordered values small by having the physical_bin_count be a
// little larger than the logical bin count, and use the wraparound only in the
// unusual case where the extra physical bins didn't turn out to be big enough.)
//
// Below, `unchecked_insert` is partly implemented by `prepare_insert`, which
// presents a viable, initialized slot pointee to the caller.
//
// `erase` is implemented in terms of `erase_at`, which takes an index to a
// slot. Given an offset, we simply mark the slot empty and destroy its
// contents.
//
// `erase` is `erase_at` composed with `find`: if we have a value `x`, we can
// perform a `find`, and then `erase_at` the resulting slot.
//
// To iterate, we simply traverse the array, skipping empty slots and stopping
// at the bin that has `is_last_bin` set.
//
// We need to rehash if the table becomes too full, or there have been too many
// insertions since the last rehash (and hence the number of disordered elements
// has gotten large and we may be running out of graveyard tombstones.)  We
// maintain a counter, growth_left, that says how many more insertions we may do
// before rehashing.  (Insertions decrement the counter, but deletions do not
// increment it.)

#ifndef GRAVEYARD_CONTAINER_INTERNAL_RAW_HASH_SET_H_
#define GRAVEYARD_CONTAINER_INTERNAL_RAW_HASH_SET_H_

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
#include "absl/container/internal/common.h"
#include "absl/container/internal/compressed_tuple.h"
#include "absl/container/internal/container_memory.h"
#include "absl/container/internal/hash_policy_traits.h"
#include "absl/container/internal/hashtablez_sampler.h"
#include "absl/numeric/int128.h"
#include "absl/memory/memory.h"
#include "absl/meta/type_traits.h"
#include "absl/numeric/bits.h"
#include "absl/utility/utility.h"
#include "graveyard/container/internal/hashtable_debug_hooks.h"

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

namespace graveyard {
ABSL_NAMESPACE_BEGIN

using absl::countr_zero;
using absl::countl_zero;

namespace container_internal {

using absl::container_internal::Allocate;
using absl::container_internal::CommonAccess;
using absl::container_internal::Deallocate;
using absl::container_internal::HashtablezInfoHandle;
using absl::container_internal::InsertReturnType;
using absl::container_internal::IsTransparent;
using absl::container_internal::KeyArg;
using absl::container_internal::node_handle;
using absl::container_internal::SanitizerPoisonMemoryRegion;
using absl::container_internal::SanitizerUnpoisonMemoryRegion;
using absl::container_internal::Sample;
using absl::container_internal::hash_policy_traits;

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

// The ordinary swiss table uses a per-table hash salt (which changes on resize)
// so that the iterator order will change.  We may not be able to support that
// easily for graveyard hashing, since we are relying on the ordered elements
// being in order of increasing H1.  Also the swiss tables employ a per-process
// ShouldInsertBackwards flag.  TODO: what kind of order fuzzing can we
// implement for graveyard hashing?
//
// Extracts the H1 portion of a hash (the bin number) given the number of
// logical bins.  We use the high order bits.  See
// https://lemire.me/blog/2016/06/27/a-fast-alternative-to-the-modulo-reduction/
static inline size_t H1(size_t hash, size_t logical_bin_count) {
  static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8);
  if constexpr (sizeof(size_t) == 8) {
    return size_t((absl::uint128(hash) * absl::uint128(logical_bin_count)) >> 64);
  } else {
    return (uint64_t(hash) * uint64_t(logical_bin_count)) >> 32;
  }
}

static constexpr size_t H2(size_t hash) {
  return hash % 127;
}

static constexpr size_t AlignAs(size_t size, size_t align) {
  return (size + align - 1) & (~align + 1);
}

// A `ctrl_t` is a single control byte, which can either be
//  empty ({.is_disordered = 0, .h2 = kEmpty})
//  full and ordered ({.is_disordered = 0, .h2 != kEmpty})
//  full and disordered ({.is_disordered = 1, .h2 != kEmpty})
// It cannot be disordered and empty.
class ctrl_t {
 public:
  static constexpr uint8_t kEmpty = 127;
  constexpr ctrl_t() :is_disordered_(0), h2_(kEmpty) {}
  static constexpr ctrl_t MakeDisordered(uint8_t h2) {
    return ctrl_t(true, h2);
  }
  static constexpr ctrl_t MakeOrdered(uint8_t h2) {
    return ctrl_t(false, h2);
  }
  constexpr ctrl_t(bool is_disordered, uint8_t h2) :is_disordered_(is_disordered), h2_(h2) {
    assert(h2 < kEmpty);
  }
  constexpr bool IsEmpty() const { return h2_ == kEmpty; }
  constexpr bool IsFull() const { return h2_ != kEmpty; }
  constexpr bool H2() const { return h2_; }
 private:
  uint8_t is_disordered_ : 1;
  uint8_t h2_ : 7;
};

class search_distance_t {
 public:
  constexpr explicit search_distance_t(bool is_end) :is_end_(is_end), search_distance_(0) {}
  constexpr bool GetIsEnd() const { return is_end_; }
  constexpr size_t GetSearchDistance() const { return search_distance_; }
  void SetIsEnd(bool is_end) { is_end_ = is_end; }
  void SetSearchDistance(size_t search_distance) {
    search_distance_ = search_distance;
    assert(search_distance_ == search_distance);
  }
 private:
  uint16_t is_end_ : 1;
  uint16_t search_distance_ : 15;
};

template <class T>
struct GraveyardCommonPolicy {
  static constexpr size_t kCacheLineSize = ABSL_CACHELINE_SIZE;
  // TODO: Consider doing F14 does, which produces slightly different
  // `kSlotsPerBin` depending on `T`.
  static constexpr size_t kSlotsPerBin = 14;
};

template <class T>
struct GraveyardLightlyLoadedPolicy : public GraveyardCommonPolicy<T> {
  static constexpr size_t kFullUtilizationNumerator = 7;
  static constexpr size_t kFullUtilizationDenominator = 8;
  static constexpr size_t kRehashedUtilizationNumerator = 7;
  static constexpr size_t kRehashedUtilizationDenominator = 16;
};

// Implement the layout
template <class Policy>
class HashTableMemory {
  using slot_type = typename Policy::slot_type;

  static constexpr size_t kCacheLineSize = Policy::cache_line_size;

  static constexpr size_t kSlotsPerBin = Policy::kSlotsPerBin;

  static constexpr size_t kCtrlSize = sizeof(ctrl_t);
  static constexpr size_t kCtrlAlign = alignof(ctrl_t);

  static constexpr size_t kSearchDistanceSize = sizeof(search_distance_t);
  static constexpr size_t kSearchDistanceAlign = alignof(search_distance_t);

  static constexpr size_t kSlotSize = sizeof(slot_type);
  static constexpr size_t kSlotAlign = alignof(slot_type);


  static constexpr size_t kCtrlStart = 0;
  static constexpr size_t kSearchDistanceStart = AlignAs(kCtrlStart + kSlotsPerBin * kCtrlSize,
                                                         kSearchDistanceAlign);
  static constexpr size_t kValueStart = AlignAs(kSearchDistanceStart + kSearchDistanceSize,
                                                kSlotAlign);

  static constexpr size_t kBinSize = AlignAs(kValueStart + kSlotsPerBin * kSlotSize, kCtrlAlign);

  static constexpr size_t kAlignment = std::max(kCtrlAlign, std::max(kSearchDistanceAlign, kSlotAlign));

 public:
  HashTableMemory() = default;
  ~HashTableMemory() {
    assert(memory_ == nullptr);
  }
  // No copy constructors or assignments
  HashTableMemory(const HashTableMemory&) = delete;
  HashTableMemory& operator=(const HashTableMemory&) = delete;
  // Move constructor
  HashTableMemory(HashTableMemory&& other) :physical_bin_count_(other.physical_bin_count_), memory_(other.memory_) {
    other.physical_bin_count_ = 0;
    other.memory_ = nullptr;
  }
  // Move assignment operator
  HashTableMemory &operator=(HashTableMemory&& other) {
    if (this != &other) {
      if (memory_) {
        // TODO: Use Deallocate
        free(memory_);
      }
      physical_bin_count_ = other.physical_bin_count_;
      memory_ = other.memory_;
      other.physical_bin_count_ = 0;
      other.memory_ = nullptr;
    }
    return *this;
  }

  // Don't bother with cache alignment unless the table has several bins in it.
  explicit HashTableMemory(size_t physical_bin_count) {
    AllocateMemory(physical_bin_count);
  }
  void AllocateMemory(size_t physical_bin_count) {
    physical_bin_count_ = physical_bin_count;
    assert(memory_ == nullptr);
    // TODO: Use Allocate
    memory_ = static_cast<char*>(std::aligned_alloc(physical_bin_count > 4 ? std::max(kCacheLineSize, kAlignment) : kAlignment,
                                                    SizeOf()));
  }
  ctrl_t* ControlOf(size_t bin_number) {
    return reinterpret_cast<ctrl_t*>(Bin(bin_number) + kCtrlStart);
  }
  const ctrl_t* ControlOf(size_t bin_number) const {
    return reinterpret_cast<ctrl_t*>(Bin(bin_number) + kCtrlStart);
  }
  search_distance_t* SearchDistanceOf(size_t bin_number) {
    return reinterpret_cast<search_distance_t*>(Bin(bin_number) + kSearchDistanceStart);
  }
  const search_distance_t* SearchDistanceOf(size_t bin_number) const {
    return reinterpret_cast<search_distance_t*>(Bin(bin_number) + kSearchDistanceStart);
  }
  slot_type* SlotOf(size_t bin_number, size_t slot_in_bin) {
    return reinterpret_cast<slot_type*>(Bin(bin_number) + kValueStart + slot_in_bin * kSlotSize);
  }
  const slot_type* SlotOf(size_t bin_number, size_t slot_in_bin) const {
    return reinterpret_cast<slot_type*>(Bin(bin_number) + kValueStart + slot_in_bin * kSlotSize);
  }
  size_t SizeOf() const {
    return physical_bin_count_ * kBinSize;
  }
  size_t GetPhysicalBinCount() const {
    return physical_bin_count_;
  }
  size_t GetLogicalBinCount() const {
    return logical_bin_count_;
  }
  size_t GetH1(size_t hash) const {
    return H1(hash, logical_bin_count_);
  }
  const char* RawMemory() const { return memory_; }
  void Deallocate() {
    // Todo: Use Deallocate()
    if (memory_) free(memory_);
    memory_ = nullptr;
  }
  class BinPointer {
   public:
    const ctrl_t* get_control() const {
      return reinterpret_cast<ctrl_t*>(bin_);
    }
    ctrl_t* get_control() {
      return reinterpret_cast<ctrl_t*>(bin_);
    }
    size_t get_search_distance() const {
      return reinterpret_cast<search_distance_t*>(bin_ + kSearchDistanceStart)->GetSearchDistance();
    }
    // TODO: Be consistent.  Is it `GetIsEnd` or `get_is_end`?
    //
    // TODO: And why isn't `search_distance_t` spelled `SearchDistance`.
    //
    // TODO: And why isn't `search_distance_t` named something that indicates it
    // holds both `search_distance` and `is_end`?
    bool get_is_end() const {
      return reinterpret_cast<search_distance_t*>(bin_ + kSearchDistanceStart)->GetIsEnd();
    }
    const slot_type* get_slot(size_t slot_in_bin) const {
      return bin_ + kValueStart + slot_in_bin * kSlotSize;
    }
    slot_type* get_slot(size_t slot_in_bin) {
      return reinterpret_cast<slot_type*>(bin_ + kValueStart + slot_in_bin * kSlotSize);
    }
    BinPointer& operator++() {
      bin_ += kBinSize;
      return *this;
    }
    bool is_default_constructed() const {
      return bin_ == nullptr;
    }
   private:
    friend HashTableMemory;
    explicit BinPointer(char *bin) :bin_(bin) {}
    // The default-constructed bin-pointer is nullptr.
    char *bin_ = nullptr;
  };
  BinPointer MakeBinPointer(size_t bin_number) {
    return BinPointer(Bin(bin_number));
  }
  struct FindInfo {
    BinPointer bin_pointer;
    size_t slot_in_bin;
    size_t probe_length;
  };

  // Probes an array of ctrl_t's using a probe sequence derived from `hash`, and
  // returns the offset corresponding to the empty slot.
  //
  // Behavior when the entire table is full is undefined.
  //
  // This code works even for a foreshortened bin used for very small table
  // capacities.  TODO: Define "foreshortened".
  FindInfo find_first_empty(size_t hash) {
    size_t h1 = H1(hash, logical_bin_count_);
    // TODO: Do we really need probe_length?
    size_t probe_length = 0;
    for (auto bin_pointer = MakeBinPointer(h1);
         true;
         ++bin_pointer, ++probe_length) {
      const ctrl_t *ctrl = bin_pointer.get_control();
      // TODO: Add a little entropy even when ASLR is not enabled by inserting
      // backwards rather than forwards in ShouldInsertBackwards.
      //
      // TODO: Vectorize this
      for (size_t slot_in_bin = 0; slot_in_bin < Policy::kSlotsPerBin; ++slot_in_bin) {
        if (ctrl[slot_in_bin].IsEmpty()) {
          return {.bin_pointer = bin_pointer, .slot_in_bin = slot_in_bin, .probe_length = probe_length};
        }
      }
      // TODO: Make falling off the end more bulletproof.
      assert(!bin_pointer.GetIsEnd());
    }
  }

 private:
  char *Bin(size_t bin_number) {
    assert(bin_number < physical_bin_count_);
    return memory_ + bin_number * kBinSize;
  }
  const char *Bin(size_t bin_number) const {
    assert(bin_number < physical_bin_count_);
    return memory_ + bin_number * kBinSize;
  }
  size_t logical_bin_count_ = 0;
  // TODO: physical_bin_count_ should be derived from logical_bin_count_.
  size_t physical_bin_count_ = 0;
  char *memory_ = nullptr;
};

template <class ContainerKey, class Hash, class Eq>
struct RequireUsableKey {
  template <class PassedKey, class... Args>
  std::pair<
      decltype(std::declval<const Hash&>()(std::declval<const PassedKey&>())),
      decltype(std::declval<const Eq&>()(std::declval<const ContainerKey&>(),
                                         std::declval<const PassedKey&>()))>*
  operator()(const PassedKey&, const Args&...) const;
};

template <class E, class Policy, class Hash, class Eq, class... Ts>
struct IsDecomposable : std::false_type {};

template <class Policy, class Hash, class Eq, class... Ts>
struct IsDecomposable<
    absl::void_t<decltype(Policy::apply(
        RequireUsableKey<typename Policy::key_type, Hash, Eq>(),
        std::declval<Ts>()...))>,
    Policy, Hash, Eq, Ts...> : std::true_type {};

// TODO(alkis): Switch to std::is_nothrow_swappable when gcc/clang supports it.
template <class T>
constexpr bool IsNoThrowSwappable(std::true_type = {} /* is_swappable */) {
  using std::swap;
  return noexcept(swap(std::declval<T&>(), std::declval<T&>()));
}
template <class T>
constexpr bool IsNoThrowSwappable(std::false_type /* is_swappable */) {
  return false;
}

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

using h2_t = uint8_t;

ABSL_DLL extern const ctrl_t kEmptyGroup[16];

// Returns a pointer to a control byte group that can be used by empty tables.
inline ctrl_t* EmptyGroup() {
  // Const must be cast away here; no uses of this function will actually write
  // to it, because it is only used for empty tables.
  return const_cast<ctrl_t*>(kEmptyGroup);
}

// Returns a pointer to a generation to use for an empty hashtable.
GenerationType* EmptyGeneration();

// Returns whether `generation` is a generation for an empty hashtable that
// could be returned by EmptyGeneration().
inline bool IsEmptyGeneration(const GenerationType* generation) {
  return *generation == SentinelEmptyGeneration();
}

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

// TODO: Kill this
static constexpr uint8_t kSentinel = 42;

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
#ifdef ABSL_INTERNAL_HAVE_SSSE3
    // This only works because ctrl_t::kEmpty is -128.
    return NonIterableBitMask<uint32_t, kWidth>(
        static_cast<uint32_t>(_mm_movemask_epi8(_mm_sign_epi8(ctrl, ctrl))));
#else
    auto match = _mm_set1_epi8(static_cast<char>(ctrl_t::kEmpty));
    return NonIterableBitMask<uint32_t, kWidth>(
        static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(match, ctrl))));
#endif
  }

  // Returns a bitmask representing the positions of empty or deleted slots.
  NonIterableBitMask<uint32_t, kWidth> MaskEmptyOrDeleted() const {
    auto special = _mm_set1_epi8(static_cast<char>(kSentinel));
    return NonIterableBitMask<uint32_t, kWidth>(static_cast<uint32_t>(
        _mm_movemask_epi8(_mm_cmpgt_epi8_fixed(special, ctrl))));
  }

  // Returns the number of trailing empty or deleted elements in the group.
  uint32_t CountLeadingEmptyOrDeleted() const {
    auto special = _mm_set1_epi8(static_cast<char>(kSentinel));
    return TrailingZeros(static_cast<uint32_t>(
        _mm_movemask_epi8(_mm_cmpgt_epi8_fixed(special, ctrl)) + 1));
  }

  void ConvertSpecialToEmptyAndFullToDeleted(ctrl_t* dst) const {
    auto msbs = _mm_set1_epi8(static_cast<char>(-128));
    auto x126 = _mm_set1_epi8(126);
#ifdef ABSL_INTERNAL_HAVE_SSSE3
    auto res = _mm_or_si128(_mm_shuffle_epi8(x126, ctrl), msbs);
#else
    auto zero = _mm_setzero_si128();
    auto special_mask = _mm_cmpgt_epi8_fixed(zero, ctrl);
    auto res = _mm_or_si128(msbs, _mm_andnot_si128(special_mask, x126));
#endif
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), res);
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
      : ctrl(absl::little_endian::Load64(pos)) {}

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
    absl::little_endian::Store64(dst, res);
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

  // Whether we should rehash on insert in order to detect bugs of using invalid
  // references. We rehash on the first insertion after reserved_growth_ reaches
  // 0 after a call to reserve. We also do a rehash with low probability
  // whenever reserved_growth_ is zero.
  bool should_rehash_for_bug_detection_on_insert(const ctrl_t* ctrl,
                                                 size_t capacity) const;
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
  // raw_hash_set in sanitizer mode and non-sanitizer mode a bit more different,
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

  bool should_rehash_for_bug_detection_on_insert(const ctrl_t*, size_t) const {
    return false;
  }
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

#if 0
template<size_t BinSize, class value_
class Bin {
  ctrl_t ctrl_[BinSize];
  uint16_t is_last_ : 1;
  uint16_t search_distance_ : 15;
  Item slots_[BinSize];
};

#endif


//ABSL_DLL extern const BinMetadata<14> kEmptyBinMetadata;

// CommonFields hold the fields in raw_hash_set that do not depend
// on template parameters. This allows us to conveniently pass all
// of this state to helper functions as a single argument.
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
        control_(that.control_),
        slots_(that.slots_),
        size_(that.size_),
        capacity_(that.capacity_),
        compressed_tuple_(that.growth_left(), std::move(that.infoz())) {
#if 0
    that.control_ = EmptyGroup();
#endif
    that.slots_ = nullptr;
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

  bool should_rehash_for_bug_detection_on_insert() const {
    return CommonFieldsGenerationInfo::
        should_rehash_for_bug_detection_on_insert(control_, capacity_);
  }
  void reset_reserved_growth(size_t reservation) {
    CommonFieldsGenerationInfo::reset_reserved_growth(reservation, size_);
  }

  // TODO: Investigate removing some of these fields:
  //   size can be moved into the mallocated memory (with `bins_`)

  // Always contains at least one bin (possibly EmptyBin)
  void *bins_;

  ctrl_t *control_ = 0;
  void *slots_ = 0;

  // The number of filled slots.
  size_t size_ = 0;

  // The total number of available slots.
  size_t logical_bin_count_ = 0;

  size_t capacity_;
  // Bundle together growth_left and HashtablezInfoHandle to ensure EBO for
  // HashtablezInfoHandle when sampling is turned off.
  absl::container_internal::CompressedTuple<size_t, absl::container_internal::HashtablezInfoHandle>
      compressed_tuple_{0u, absl::container_internal::HashtablezInfoHandle{}};
};

template <class Policy, class Hash, class Eq, class Alloc>
class raw_hash_set;

// General notes on capacity/growth methods below:
// - We keep the load factor below `Policy::max_utilization_numerator /
//   Policy::max_utilization_denominator`.  (Except for small capacity).
// - The capacity is <= Policy::SlotsPerBin or a multiple of
//   Policy::SlotsPerBin.  If the `capacity <= Policy::SlotsPerBin ` then we
//   never need to probe (the whole table fits in one bin) so we can allow the
//   load factor to be as high as 1.

// Return the ceiling of a/b.
constexpr size_t Ceil(size_t a, size_t b) {
  return (a + b - 1) / b;
}

// Return the number of logical bins needed for a particular size, to run at a
// load factor.  The load factor is expressed as a ratio
// kNumerator/kDenominator.
template <size_t slots_per_bin, size_t kNumerator, size_t kDenominator>
static constexpr size_t BinCountForLoad(size_t size) {
  // If the size is zero, we have a special case array for that
  if (size == 0) return 0;
  // If the size fits into one bin, then just use one bin, since there is no probing.
  if (size <= slots_per_bin) return 1;
  // Otherwise we want `size <= number_of_slots * kNumerator / kDenominator`.
  //
  // But `number_of_slots == slots_per_bin * number_of_bins`, so we want
  //  `size <= number_of_bins * slots_per_bin * kNumerator / kDenominator`
  // so we want
  //  `size * kDenominator / (slots_per_bin * kNumerator) <= number_of_bins.
  return Ceil(size * kDenominator, slots_per_bin * kNumerator);
}


#if 0
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
#endif

constexpr bool SwisstableDebugEnabled() {
#if defined(ABSL_SWISSTABLE_ENABLE_GENERATIONS) || \
    ABSL_OPTION_HARDENED == 1 || !defined(NDEBUG)
  return true;
#else
  return false;
#endif
}

#if 0
inline void AssertIsFull(const const_iterator it,
                         const char* operation) {
}
#endif
#if 0
// Note that for comparisons, null/end iterators are valid.
inline void AssertIsValidForComparison(const ctrl_t* ctrl,
                                       GenerationType generation,
                                       const GenerationType* generation_ptr) {
  if (!SwisstableDebugEnabled()) return;
  const bool ctrl_is_valid_for_comparison =
      ctrl == nullptr || ctrl == EmptyGroup() || IsFull(*ctrl);
  if (SwisstableGenerationsEnabled()) {
    if (generation != *generation_ptr) {
      ABSL_INTERNAL_LOG(FATAL,
                        "Invalid iterator comparison. The table could have "
                        "rehashed since this iterator was initialized.");
    }
    if (!ctrl_is_valid_for_comparison) {
      ABSL_INTERNAL_LOG(
          FATAL, "Invalid iterator comparison. The element was likely erased.");
    }
  } else {
    ABSL_HARDENING_ASSERT(
        ctrl_is_valid_for_comparison &&
        "Invalid iterator comparison. The element might have been erased or "
        "the table might have rehashed. Consider running with --config=asan to "
        "diagnose rehashing issues.");
  }
}
#endif

// If the two iterators come from the same container, then their pointers will
// interleave such that ctrl_a <= ctrl_b < slot_a <= slot_b or vice/versa.
// Note: we take slots by reference so that it's not UB if they're uninitialized
// as long as we don't read them (when ctrl is null).
inline bool AreItersFromSameContainer(const ctrl_t* ctrl_a,
                                      const ctrl_t* ctrl_b,
                                      const void* const& slot_a,
                                      const void* const& slot_b) {
  // If either control byte is null, then we can't tell.
  if (ctrl_a == nullptr || ctrl_b == nullptr) return true;
  const void* low_slot = slot_a;
  const void* hi_slot = slot_b;
  if (ctrl_a > ctrl_b) {
    std::swap(ctrl_a, ctrl_b);
    std::swap(low_slot, hi_slot);
  }
  return ctrl_b < low_slot && low_slot <= hi_slot;
}

// Asserts that two iterators come from the same container.
// Note: we take slots by reference so that it's not UB if they're uninitialized
// as long as we don't read them (when ctrl is null).
inline void AssertSameContainer(const ctrl_t* ctrl_a, const ctrl_t* ctrl_b,
                                const void* const& slot_a,
                                const void* const& slot_b,
                                const GenerationType* generation_ptr_a,
                                const GenerationType* generation_ptr_b) {
  if (!SwisstableDebugEnabled()) return;
  const bool a_is_default = ctrl_a == EmptyGroup();
  const bool b_is_default = ctrl_b == EmptyGroup();
  if (a_is_default != b_is_default) {
    ABSL_INTERNAL_LOG(
        FATAL,
        "Invalid iterator comparison. Comparing default-constructed iterator "
        "with non-default-constructed iterator.");
  }
  if (a_is_default && b_is_default) return;

  if (SwisstableGenerationsEnabled()) {
    if (generation_ptr_a == generation_ptr_b) return;
    const bool a_is_empty = IsEmptyGeneration(generation_ptr_a);
    const bool b_is_empty = IsEmptyGeneration(generation_ptr_b);
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
    const bool a_is_end = ctrl_a == nullptr;
    const bool b_is_end = ctrl_b == nullptr;
    if (a_is_end || b_is_end) {
      ABSL_INTERNAL_LOG(FATAL,
                        "Invalid iterator comparison. Comparing iterator with "
                        "an end() iterator from a different hashtable.");
    }
    ABSL_INTERNAL_LOG(FATAL,
                      "Invalid iterator comparison. Comparing non-end() "
                      "iterators from different hashtables.");
  } else {
    ABSL_HARDENING_ASSERT(
        AreItersFromSameContainer(ctrl_a, ctrl_b, slot_a, slot_b) &&
        "Invalid iterator comparison. The iterators may be from different "
        "containers or the container might have rehashed. Consider running "
        "with --config=asan to diagnose rehashing issues.");
  }
}

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
// `find_first_empty()`, where we never try
// `ShouldInsertBackwards()` for small tables.
inline bool is_small(size_t capacity) { return capacity < Group::kWidth - 1; }

static inline size_t NumClonedBytes() { return 0; }

inline void ResetGrowthLeft(CommonFields& common) {
  common.growth_left() = /*CapacityToGrowth*/(common.capacity_) - common.size_;
}

// Sets `ctrl` to `{kEmpty, kSentinel, ..., kEmpty}`, marking the entire
// array as marked as empty.
inline void ResetCtrl(CommonFields& common, size_t slot_size) {
  const size_t capacity = common.capacity_;
  ctrl_t* ctrl = common.control_;
  *ctrl = ctrl_t();
#if 0
  std::memset(ctrl, static_cast<int8_t>(ctrl_t::kEmpty),
              capacity + 1 + NumClonedBytes());
#endif
  ctrl[capacity] = ctrl_t(); //ctrl_t::kSentinel;
  SanitizerPoisonMemoryRegion(common.slots_, slot_size * capacity);
  ResetGrowthLeft(common);
}

// TODO: Get rid of SetCtrl

// Sets `ctrl[i]` to `h`.
//
// Unlike setting it directly, this function will perform bounds checks and
// mirror the value to the cloned tail if necessary.
inline void SetCtrl(const CommonFields& common, size_t i, ctrl_t h,
                    size_t slot_size) {
  const size_t capacity = common.capacity_;
  assert(i < capacity);

#if 0
  auto* slot_i = static_cast<const char*>(common.slots_) + i * slot_size;
  if (IsFull(h)) {
    SanitizerUnpoisonMemoryRegion(slot_i, slot_size);
  } else {
    SanitizerPoisonMemoryRegion(slot_i, slot_size);
  }
#endif

  ctrl_t* ctrl = common.control_;
  ctrl[i] = h;
  ctrl[((i - NumClonedBytes()) & capacity) + (NumClonedBytes() & capacity)] = h;
}

// Overload for setting to an occupied `h2_t` rather than a special `ctrl_t`.
inline void SetCtrl(const CommonFields& common, size_t i, h2_t h,
                    size_t slot_size) {
#if 0
  SetCtrl(common, i, static_cast<ctrl_t>(h), slot_size);
#endif
}

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

// PolicyFunctions bundles together some information for a particular
// raw_hash_set<T, ...> instantiation. This information is passed to
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

// ClearBackingArray clears the backing array, either modifying it in place,
// or creating a new one based on the value of "reuse".
// REQUIRES: c.capacity > 0
void ClearBackingArray(CommonFields& c, const PolicyFunctions& policy,
                       bool reuse);

// Type-erased version of raw_hash_set::erase_meta_only.
void EraseMetaOnly(CommonFields& c, ctrl_t* it, size_t slot_size);

// Function to place in PolicyFunctions::dealloc for raw_hash_sets
// that are using std::allocator. This allows us to share the same
// function body for raw_hash_set instantiations that have the
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

// For trivially relocatable types we use memcpy directly. This allows us to
// share the same function body for raw_hash_set instantiations that have the
// same slot size as long as they are relocatable.
template <size_t SizeOfSlot>
ABSL_ATTRIBUTE_NOINLINE void TransferRelocatable(void*, void* dst, void* src) {
  memcpy(dst, src, SizeOfSlot);
}

// Type-erased version of raw_hash_set::drop_deletes_without_resize.
void DropDeletesWithoutResize(CommonFields& common,
                              const PolicyFunctions& policy, void* tmp_space);

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
class raw_hash_set {
  using PolicyTraits = absl::container_internal::hash_policy_traits<Policy>;
  using KeyArgImpl =
      KeyArg<IsTransparent<Eq>::value && IsTransparent<Hash>::value>;
  using Memory = HashTableMemory<Policy>;
  using BinPointer = typename Memory::BinPointer;

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

 public:
  static_assert(std::is_same<pointer, value_type*>::value,
                "Allocators with custom pointer types are not supported");
  static_assert(std::is_same<const_pointer, const value_type*>::value,
                "Allocators with custom pointer types are not supported");

  class iterator : private HashSetIteratorGenerationInfo {
    friend class raw_hash_set;

   public:
    using iterator_category = std::forward_iterator_tag;
    using slot_type  = typename raw_hash_set::slot_type;
    using value_type = typename raw_hash_set::value_type;
    using reference =
        absl::conditional_t<PolicyTraits::constant_iterators::value,
                            const value_type&, value_type&>;
    using pointer = absl::remove_reference_t<reference>*;
    using difference_type = typename raw_hash_set::difference_type;

    iterator() {}

    // PRECONDITION: not an end() iterator.
    reference operator*() const {
      AssertIsFull("operator*()");
      return PolicyTraits::element(bin_.get_slot(slot_in_bin_));
    }
    slot_type* get_slot() {
      bin_.get_slot(slot_in_bin_);
    }

#if 0
    // PRECONDITION: not an end() iterator.
    pointer operator->() const {
      AssertIsFull(ctrl_, generation(), generation_ptr(), "operator->");
      return &operator*();
    }
#endif

    // PRECONDITION: not an end() iterator.
    iterator& operator++() {
#if 0
      AssertIsFull(ctrl_, generation(), generation_ptr(), "operator++");
#endif
      ++slot_in_bin_;
      if (slot_in_bin_ == Policy::kSlotsPerBin && !bin_.get_is_end()) {
        ++bin_;
        slot_in_bin_ = 0;
      }
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
#if 0
      AssertIsValidForComparison(a.ctrl_, a.generation(), a.generation_ptr());
      AssertIsValidForComparison(b.ctrl_, b.generation(), b.generation_ptr());
      AssertSameContainer(a.ctrl_, b.ctrl_, a.slot_, b.slot_,
                          a.generation_ptr(), b.generation_ptr());
#endif
      return a.ctrl_ == b.ctrl_;
    }
    friend bool operator!=(const iterator& a, const iterator& b) {
      return !(a == b);
    }

    void AssertIsFull(const char *operation) const {
      if (!SwisstableDebugEnabled()) return;
      if (bin_.is_default_constructed()) {
        ABSL_INTERNAL_LOG(FATAL, std::string(operation) +
                          " called on default-constructed iterator.");
      }
      if (slot_in_bin_ == Policy::kSlotsPerBin && !bin_.get_is_end()) {
        ABSL_INTERNAL_LOG(FATAL,
                          std::string(operation) + " called on end() iterator.");
      }
      if (SwisstableGenerationsEnabled()) {
        if (generation() != *generation_ptr()) {
          ABSL_INTERNAL_LOG(FATAL,
                            std::string(operation) +
                            " called on invalid iterator. The table could have "
                            "rehashed since this iterator was initialized.");
        }
        if (bin_.get_control()[slot_in_bin_].IsEmpty()) {
          ABSL_INTERNAL_LOG(
              FATAL,
              std::string(operation) +
              " called on invalid iterator. The element was likely erased.");
        }
      } else {
        if (bin_.get_control()[slot_in_bin_].IsEmpty()) {
          ABSL_INTERNAL_LOG(
              FATAL,
              std::string(operation) +
              " called on invalid iterator. The element might have been erased "
              "or the table might have rehashed. Consider running with "
              "--config=asan to diagnose rehashing issues.");
        }
      }
    }

   private:
    iterator(BinPointer bin, size_t slot_in_bin,
             const GenerationType* generation_ptr)
        : HashSetIteratorGenerationInfo(generation_ptr),
          bin_(bin),
          slot_in_bin_(slot_in_bin) {
      // This assumption helps the compiler know that any non-end iterator is
      // not equal to any end iterator.
      ABSL_ASSUME(bin_.get_control() != nullptr);
    }
    // For end() iterators.
    explicit iterator(const GenerationType* generation_ptr)
        : HashSetIteratorGenerationInfo(generation_ptr) {}

    // Fixes up `ctrl_` to point to a full by advancing it and `slot_` until
    // they reach one.
    //
    // If a sentinel is reached, we null `ctrl_` out instead.
    void skip_empty_or_deleted() {
#if 0
      while (IsEmptyOrDeleted(*ctrl_)) {
        uint32_t shift = Group{ctrl_}.CountLeadingEmptyOrDeleted();
        ctrl_ += shift;
        slot_ += shift;
      }
      if (ABSL_PREDICT_FALSE(*ctrl_ == ctrl_t::kSentinel)) ctrl_ = nullptr;
#endif
    }

    // The end iterator has slot_in_bin_ == kSlotsPerBin and bin_ pointing to a
    // bin that has "is_last" set.
    //
    // The default-constructed iterator has a null bin_ (a default-constructed
    // BinPointer).
    BinPointer bin_;
    size_t slot_in_bin_;
  };

  class const_iterator {
    friend class raw_hash_set;

   public:
    using iterator_category = typename iterator::iterator_category;
    using value_type = typename raw_hash_set::value_type;
    using reference = typename raw_hash_set::const_reference;
    using pointer = typename raw_hash_set::const_pointer;
    using difference_type = typename raw_hash_set::difference_type;

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
  raw_hash_set() noexcept(
      std::is_nothrow_default_constructible<hasher>::value&&
          std::is_nothrow_default_constructible<key_equal>::value&&
              std::is_nothrow_default_constructible<allocator_type>::value) {}

  ABSL_ATTRIBUTE_NOINLINE explicit raw_hash_set(
      size_t bucket_count, const hasher& hash = hasher(),
      const key_equal& eq = key_equal(),
      const allocator_type& alloc = allocator_type())
      : settings_(CommonFields{}, hash, eq, alloc) {
    if (bucket_count) {
      common().capacity_ = /*NormalizeCapacity(bucket_count)*/ bucket_count;
      initialize_slots();
    }
  }

  raw_hash_set(size_t bucket_count, const hasher& hash,
               const allocator_type& alloc)
      : raw_hash_set(bucket_count, hash, key_equal(), alloc) {}

  raw_hash_set(size_t bucket_count, const allocator_type& alloc)
      : raw_hash_set(bucket_count, hasher(), key_equal(), alloc) {}

  explicit raw_hash_set(const allocator_type& alloc)
      : raw_hash_set(0, hasher(), key_equal(), alloc) {}

  template <class InputIter>
  raw_hash_set(InputIter first, InputIter last, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : raw_hash_set(SelectBucketCountForIterRange(first, last, bucket_count),
                     hash, eq, alloc) {
    insert(first, last);
  }

  template <class InputIter>
  raw_hash_set(InputIter first, InputIter last, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : raw_hash_set(first, last, bucket_count, hash, key_equal(), alloc) {}

  template <class InputIter>
  raw_hash_set(InputIter first, InputIter last, size_t bucket_count,
               const allocator_type& alloc)
      : raw_hash_set(first, last, bucket_count, hasher(), key_equal(), alloc) {}

  template <class InputIter>
  raw_hash_set(InputIter first, InputIter last, const allocator_type& alloc)
      : raw_hash_set(first, last, 0, hasher(), key_equal(), alloc) {}

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
  raw_hash_set(std::initializer_list<T> init, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : raw_hash_set(init.begin(), init.end(), bucket_count, hash, eq, alloc) {}

  raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count = 0,
               const hasher& hash = hasher(), const key_equal& eq = key_equal(),
               const allocator_type& alloc = allocator_type())
      : raw_hash_set(init.begin(), init.end(), bucket_count, hash, eq, alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  raw_hash_set(std::initializer_list<T> init, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : raw_hash_set(init, bucket_count, hash, key_equal(), alloc) {}

  raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count,
               const hasher& hash, const allocator_type& alloc)
      : raw_hash_set(init, bucket_count, hash, key_equal(), alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  raw_hash_set(std::initializer_list<T> init, size_t bucket_count,
               const allocator_type& alloc)
      : raw_hash_set(init, bucket_count, hasher(), key_equal(), alloc) {}

  raw_hash_set(std::initializer_list<init_type> init, size_t bucket_count,
               const allocator_type& alloc)
      : raw_hash_set(init, bucket_count, hasher(), key_equal(), alloc) {}

  template <class T, RequiresNotInit<T> = 0, RequiresInsertable<T> = 0>
  raw_hash_set(std::initializer_list<T> init, const allocator_type& alloc)
      : raw_hash_set(init, 0, hasher(), key_equal(), alloc) {}

  raw_hash_set(std::initializer_list<init_type> init,
               const allocator_type& alloc)
      : raw_hash_set(init, 0, hasher(), key_equal(), alloc) {}

  raw_hash_set(const raw_hash_set& that)
      : raw_hash_set(that, AllocTraits::select_on_container_copy_construction(
                               that.alloc_ref())) {}

  raw_hash_set(const raw_hash_set& that, const allocator_type& a)
      : raw_hash_set(0, that.hash_ref(), that.eq_ref(), a) {
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

  ABSL_ATTRIBUTE_NOINLINE raw_hash_set(raw_hash_set&& that) noexcept(
      std::is_nothrow_copy_constructible<hasher>::value&&
          std::is_nothrow_copy_constructible<key_equal>::value&&
              std::is_nothrow_copy_constructible<allocator_type>::value)
      :  // Hash, equality and allocator are copied instead of moved because
         // `that` must be left valid. If Hash is std::function<Key>, moving it
         // would create a nullptr functor that cannot be called.
        settings_(absl::exchange(that.common(), CommonFields{}),
                  that.hash_ref(), that.eq_ref(), that.alloc_ref()) {}

  raw_hash_set(raw_hash_set&& that, const allocator_type& a)
      : settings_(CommonFields{}, that.hash_ref(), that.eq_ref(), a) {
    if (a == that.alloc_ref()) {
      std::swap(common(), that.common());
    } else {
      reserve(that.size());
      // Note: this will copy elements of dense_set and unordered_set instead of
      // moving them. This can be fixed if it ever becomes an issue.
      for (auto& elem : that) insert(std::move(elem));
    }
  }

  raw_hash_set& operator=(const raw_hash_set& that) {
    raw_hash_set tmp(that,
                     AllocTraits::propagate_on_container_copy_assignment::value
                         ? that.alloc_ref()
                         : alloc_ref());
    swap(tmp);
    return *this;
  }

  raw_hash_set& operator=(raw_hash_set&& that) noexcept(
      absl::allocator_traits<allocator_type>::is_always_equal::value&&
          std::is_nothrow_move_assignable<hasher>::value&&
              std::is_nothrow_move_assignable<key_equal>::value) {
    // TODO(sbenza): We should only use the operations from the noexcept clause
    // to make sure we actually adhere to that contract.
    // NOLINTNEXTLINE: not returning *this for performance.
    return move_assign(
        std::move(that),
        typename AllocTraits::propagate_on_container_move_assignment());
  }

  ~raw_hash_set() {
    const size_t cap = capacity();
    if (!cap) return;
    destroy_slots();

    // Unpoison before returning the memory to the allocator.
    SanitizerUnpoisonMemoryRegion(slot_array(), sizeof(slot_type) * cap);
    hashtable_memory_.Deallocate();

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
    return const_cast<raw_hash_set*>(this)->begin();
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
    for (size_t i = 0; i < hashtable_memory_.GetPhysicalBinCount(); ++i) {
      ctrl_t* ctrl = hashtable_memory_.ControlOf(i);

      for (size_t slotoff = 0; slotoff < Policy::kSlotsPerBin; ++slotoff) {
        if (ctrl[slotoff].IsFull()) {
          PolicyTraits::destroy(&alloc_ref(), hashtable_memory_.SlotOf(i, slotoff));
        }
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
  // Otherwise calls `f` with one argument of type `raw_hash_set::constructor`.
  //
  // `f` must abide by several restrictions:
  //  - it MUST call `raw_hash_set::constructor` with arguments as if a
  //    `raw_hash_set::value_type` is constructed,
  //  - it MUST NOT access the container before the call to
  //    `raw_hash_set::constructor`, and
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
    friend class raw_hash_set;

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
  void merge(raw_hash_set<Policy, H, E, Alloc>& src) {  // NOLINT
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
  void merge(raw_hash_set<Policy, H, E, Alloc>&& src) {
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

  void swap(raw_hash_set& that) noexcept(
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
      size_t m = n/*GrowthToLowerboundCapacity(n)*/;
      resize(/*NormalizeCapacity*/(m));

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
    PrefetchToLocalCache(hashtable_memory_.ControlOf(seq.bin_number));
    PrefetchToLocalCache(hashtable_memory_.SlotOf(seq.bin_number, seq.slot_in_bin));
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
    size_t h1 = hashtable_memory_.GetH1(hash);
    size_t h2 = H2(hash);
    auto bin_pointer = hashtable_memory_.MakeBinPointer(h1);
    size_t search_distance = bin_pointer.get_search_distance();
    for (size_t bin_offset = 0; bin_offset < search_distance; ++bin_offset, ++bin_pointer) {
      const ctrl_t *ctrl = bin_pointer.get_control();
      // TODO: Vectorize this
      for (size_t slot_in_bin = 0; slot_in_bin < Policy::kSlotsPerBin; ++slot_in_bin) {
        const ctrl_t ctrl_of_slot = ctrl[slot_in_bin];
        if (ctrl_of_slot.IsFull() && ctrl_of_slot.H2() == h2 &&
            ABSL_PREDICT_TRUE(PolicyTraits::apply(
                EqualElement<K>{key, eq_ref()},
                PolicyTraits::element(bin_pointer.get_slot(slot_in_bin))))) {
          return iterator_at(bin_pointer, slot_in_bin);
        }
      }
    }
    return end();
  }

  template <class K = key_type>
  iterator find(const key_arg<K>& key) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    prefetch_heap_block();
    return find(key, hash_ref()(key));
  }

  template <class K = key_type>
  const_iterator find(const key_arg<K>& key,
                      size_t hash) const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return const_cast<raw_hash_set*>(this)->find(key, hash);
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

  friend bool operator==(const raw_hash_set& a, const raw_hash_set& b) {
    if (a.size() != b.size()) return false;
    const raw_hash_set* outer = &a;
    const raw_hash_set* inner = &b;
    if (outer->capacity() > inner->capacity()) std::swap(outer, inner);
    for (const value_type& elem : *outer)
      if (!inner->has_element(elem)) return false;
    return true;
  }

  friend bool operator!=(const raw_hash_set& a, const raw_hash_set& b) {
    return !(a == b);
  }

  template <typename H>
  friend typename std::enable_if<H::template is_hashable<value_type>::value,
                                 H>::type
  AbslHashValue(H h, const raw_hash_set& s) {
    return H::combine(H::combine_unordered(std::move(h), s.begin(), s.end()),
                      s.size());
  }

  friend void swap(raw_hash_set& a,
                   raw_hash_set& b) noexcept(noexcept(a.swap(b))) {
    a.swap(b);
  }

 private:
  template <class Container, typename Enabler>
  friend struct graveyard::container_internal::hashtable_debug_internal::
      HashtableDebugAccess;

  struct FindElement {
    template <class K, class... Args>
    const_iterator operator()(const K& key, Args&&...) const {
      return s.find(key);
    }
    const raw_hash_set& s;
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
      return res;
    }
    raw_hash_set& s;
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
    raw_hash_set& s;
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
  inline void initialize_slots() {
    // People are often sloppy with the exact type of their allocator (sometimes
    // it has an extra const or is missing the pair, but rebinds made it work
    // anyway).
    using CharAlloc =
        typename absl::allocator_traits<Alloc>::template rebind_alloc<char>;
    InitializeSlots<CharAlloc, sizeof(slot_type), alignof(slot_type)>(
        common(), CharAlloc(alloc_ref()));
  }

  private:
  void resize_to_size(size_t new_size) {
    size_t total_probe_length = 0;
    Memory old_memory = std::move(hashtable_memory_);
    // TODO: This isn't right:   This formula is for
    hashtable_memory_.AllocateMemory(BinCountForLoad<Policy::kSlotsPerBin, Policy::kFullUtilizationNumerator, Policy::kFullUtilizationDenominator>(new_size));
    common().capacity_ = hashtable_memory_.GetLogicalBinCount * Policy::kSlotsPerBin;
    initialize_slots();
    for (BinPointer bin_pointer = old_memory.MakeBinPointer(0); true; ++bin_pointer) {
      ctrl_t* ctrl = bin_pointer.get_control();
      for (size_t slot_in_bin = 0; slot_in_bin < Policy::kSlotsPerBin; ++slot_in_bin) {
        if (ctrl[slot_in_bin].IsFull()) {
          size_t hash = PolicyTraits::apply(HashElement{hash_ref()},
                                            PolicyTraits::element(bin_pointer.get_slot(slot_in_bin)));
          auto [dest_bin_pointer, dest_slot_in_bin, probe_length] = hashtable_memory_.find_first_empty(hash);
          total_probe_length += probe_length;
          dest_bin_pointer.get_control()[dest_slot_in_bin] = ctrl_t::MakeDisordered(H2(hash));
          PolicyTraits::transfer(&alloc_ref(), dest_bin_pointer.get_slot(dest_slot_in_bin), bin_pointer.get_slot(slot_in_bin));
          bin_pointer.get_control()[slot_in_bin] = ctrl_t{};
        }
      }
      if (bin_pointer.get_is_end()) {
        break;
      }
      // TODO:  The analogous thing to this:
      // SanitizerUnpoisonMemoryRegion(old_slots,
      //    sizeof(slot_type) * old_capacity);
      old_memory.Deallocate();
    }
    infoz().RecordRehash(total_probe_length);
  }

 public:
  ABSL_ATTRIBUTE_NOINLINE void resize(size_t new_capacity) {
    if (new_capacity == 0) {
      new_capacity = 1;
    }
    resize_to_size(Ceil(new_capacity * Policy::kFullUtilizationNumerator, Policy::kFullUtilizationDenominator));
  }

  bool has_element(const value_type& elem) const {
    size_t hash = PolicyTraits::apply(HashElement{hash_ref()}, elem);
    size_t h1 = hashtable_memory_.GetH1(hash);
    size_t h2 = H2(hash);
    auto bin_pointer = hashtable_memory_.MakeBinPointer(h1);
    size_t search_distance = bin_pointer.get_search_distance();
    for (size_t bin_offset = 0; bin_offset < search_distance; ++bin_offset, ++bin_pointer) {
      // TODO: Vectorize this
      const ctrl_t *ctrl = bin_pointer.get_control();
      for (size_t slot_in_bin = 0; slot_in_bin < Policy::kSlotsPerBin; ++slot_in_bin) {
        const ctrl_t ctrl_of_slot = ctrl[slot_in_bin];
        if (ctrl_of_slot.IsFull() && ctrl_of_slot.H2() == h2 &&
            ABSL_PREDICT_TRUE(eq_ref()(elem,
                                       PolicyTraits::element(bin_pointer.get_slot(slot_in_bin))))) {
          return true;
        }
      }
    }
    return false;
  }

  // TODO(alkis): Optimize this assuming *this and that don't overlap.
  raw_hash_set& move_assign(raw_hash_set&& that, std::true_type) {
    raw_hash_set tmp(std::move(that));
    swap(tmp);
    return *this;
  }
  raw_hash_set& move_assign(raw_hash_set&& that, std::false_type) {
    raw_hash_set tmp(std::move(that), alloc_ref());
    swap(tmp);
    return *this;
  }

 private:
  static constexpr size_t SizeAfterRehash(size_t size) {
    return Ceil(size * PolicyTraits::kRehashedUtilizationNumerator, PolicyTraits::kRehashedUtilizationDenominator);
  }

 protected:
  // Attempts to find `key` in the table; if it isn't found, returns a slot that
  // the value can be inserted into, with the control byte already set to
  // `key`'s H2.
  template <class K>
  std::pair<iterator, bool> find_or_prepare_insert(const K& key) {
    prefetch_heap_block();
    auto hash = hash_ref()(key);
    size_t h1 = hashtable_memory_.GetH1(hash);
    size_t h2 = H2(hash);
    auto bin_pointer = hashtable_memory_.MakeBinPointer(h1);
    size_t search_distance = bin_pointer.get_search_distance();
    for (size_t bin_offset = 0; bin_offset < search_distance; ++bin_offset, ++bin_pointer) {
      const ctrl_t *ctrl = bin_pointer.get_control();
      // TODO: Vectorize this
      for (size_t slot_in_bin = 0; slot_in_bin < Policy::kSlotsPerBin; ++slot_in_bin) {
        const ctrl_t ctrl_of_slot = ctrl[slot_in_bin];
        if (ctrl_of_slot.IsFull() && ctrl_of_slot.H2() == h2 &&
            ABSL_PREDICT_TRUE(PolicyTraits::apply(
                EqualElement<K>{key, eq_ref()},
                PolicyTraits::element(bin_pointer.get_slot(slot_in_bin))))) {
          return {iterator_at(bin_pointer, slot_in_bin), false};
        }
      }
    }
    return {prepare_insert(hash, h2), true};
  }

  // Given the hash of a value not currently in the table, finds the next viable
  // slot index to insert it at.  Updates the metadata to indicate that the slot
  // contains a value with `hash`.
  //
  // REQUIRES: At least one non-full slot available.
  iterator prepare_insert(size_t hash, size_t h2) ABSL_ATTRIBUTE_NOINLINE {
    // TODO: Should_rehash_for_bug_detection_on_insert() code should just be
    // removed, and we should simply set up growth_left() to be a smaller value
    // when we are doing this kind of fuzzing?
    const bool rehash_for_bug_detection =
        common().should_rehash_for_bug_detection_on_insert();
    if (rehash_for_bug_detection) {
      // Move to a different heap allocation in order to detect bugs.
      resize(growth_left() > 0 ? capacity() : SizeAfterRehash(common().size_));
    }
    auto target = find_first_non_full(common(), hash);
    if (!rehash_for_bug_detection &&
        ABSL_PREDICT_FALSE(growth_left() == 0)) {
      resize(SizeAfterRehash(common().size_));
      target = find_first_non_full(common(), hash);
    }
    ++common().size_;
    growth_left() -= IsEmpty(hashtable_memory_.ControlOf(target.bin_number)[target.slot_in_bin]);
    hashtable_memory_.ControlOf(target.bin_number)[target.slot_in_bin].MakeDisordered(H2(hash));
    common().maybe_increment_generation_on_insert();
    infoz().RecordInsert(hash, target.probe_length);
    return target.offset;
  }

  // Constructs the value in the space pointed by the iterator. This only works
  // after an unsuccessful find_or_prepare_insert() and before any other
  // modifications happen in the raw_hash_set.
  //
  // PRECONDITION: i is an index returned from find_or_prepare_insert(k), where
  // k is the key decomposed from `forward<Args>(args)...`, and the bool
  // returned by find_or_prepare_insert(k) was true.
  // POSTCONDITION: *m.iterator_at(i) == value_type(forward<Args>(args)...).
  template <class... Args>
  void emplace_at(iterator it, Args&&... args) {
    PolicyTraits::construct(&alloc_ref(), it.get_slot(),
                            std::forward<Args>(args)...);

    assert(PolicyTraits::apply(FindElement{*this}, *it) ==
              it &&
           "constructed value does not match the lookup key");
  }

  iterator iterator_at(BinPointer bp, size_t slot_in_bin) ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return {bp, slot_in_bin, common().generation_ptr()};
  }
  const_iterator iterator_at(BinPointer bp, size_t slot_in_bin) const ABSL_ATTRIBUTE_LIFETIME_BOUND {
    return {bp, slot_in_bin, common().generation_ptr()};
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
    __builtin_prefetch(hashtable_memory_.RawMemory(), 0, 1);
#endif
  }

  CommonFields& common() { return settings_.template get<0>(); }
  const CommonFields& common() const { return settings_.template get<0>(); }

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
    auto* h = static_cast<raw_hash_set*>(set);
    return PolicyTraits::apply(
        HashElement{h->hash_ref()},
        PolicyTraits::element(static_cast<slot_type*>(slot)));
  }
  static void transfer_slot_fn(void* set, void* dst, void* src) {
    auto* h = static_cast<raw_hash_set*>(set);
    PolicyTraits::transfer(&h->alloc_ref(), static_cast<slot_type*>(dst),
                           static_cast<slot_type*>(src));
  }
  // Note: dealloc_fn will only be used if we have a non-standard allocator.
  static void dealloc_fn(void* set, const PolicyFunctions&, ctrl_t* ctrl,
                         void* slot_mem, size_t n) {
    auto* h = static_cast<raw_hash_set*>(set);

    // Unpoison before returning the memory to the allocator.
    SanitizerUnpoisonMemoryRegion(slot_mem, sizeof(slot_type) * n);

    Deallocate<alignof(slot_type)>(
        &h->alloc_ref(), ctrl,
        AllocSize(n, sizeof(slot_type), alignof(slot_type)));
  }

  static const PolicyFunctions& GetPolicyFunctions() {
    static constexpr PolicyFunctions value = {
        sizeof(slot_type),
        &raw_hash_set::hash_slot_fn,
        PolicyTraits::transfer_uses_memcpy()
            ? TransferRelocatable<sizeof(slot_type)>
            : &raw_hash_set::transfer_slot_fn,
        (std::is_same<SlotAlloc, std::allocator<slot_type>>::value
             ? &DeallocateStandard<alignof(slot_type)>
             : &raw_hash_set::dealloc_fn),
    };
    return value;
  }

  Memory hashtable_memory_;

  // Bundle together CommonFields plus other objects which might be empty.
  // CompressedTuple will ensure that sizeof is not affected by any of the empty
  // fields that occur after CommonFields.
  absl::container_internal::CompressedTuple<CommonFields, hasher, key_equal,
                                            allocator_type>
      settings_{CommonFields{}, hasher{}, key_equal{}, allocator_type{}};
};

// Erases all elements that satisfy the predicate `pred` from the container `c`.
template <typename P, typename H, typename E, typename A, typename Predicate>
typename raw_hash_set<P, H, E, A>::size_type EraseIf(
    Predicate& pred, raw_hash_set<P, H, E, A>* c) {
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

namespace hashtable_debug_internal {
template <typename Set>
struct HashtableDebugAccess<Set, absl::void_t<typename Set::raw_hash_set>> {
  using Traits = typename Set::PolicyTraits;
  using Slot = typename Traits::slot_type;

  static size_t GetNumProbes(const Set& set,
                             const typename Set::key_type& key) {
    size_t num_probes = 0;
    size_t hash = set.hash_ref()(key);
    auto seq = probe(set.common(), hash);
    const ctrl_t* ctrl = set.control();
    while (true) {
      container_internal::Group g{ctrl + seq.offset()};
      for (uint32_t i : g.Match(container_internal::H2(hash))) {
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
    size_t m = AllocSize(capacity, sizeof(Slot), alignof(Slot));

    size_t per_slot = Traits::space_used(static_cast<const Slot*>(nullptr));
    if (per_slot != ~size_t{}) {
      m += per_slot * c.size();
    } else {
      const ctrl_t* ctrl = c.control();
      for (size_t i = 0; i != capacity; ++i) {
#if 0
        if (container_internal::IsFull(ctrl[i])) {
          m += Traits::space_used(c.slot_array() + i);
        }
#endif
      }
    }
    return m;
  }

  static size_t LowerBoundAllocatedByteSize(size_t size) {
    size_t capacity = /*GrowthToLowerboundCapacity*/(size);
    if (capacity == 0) return 0;
    size_t m =
        AllocSize(/*NormalizeCapacity*/(capacity), sizeof(Slot), alignof(Slot));
    size_t per_slot = Traits::space_used(static_cast<const Slot*>(nullptr));
    if (per_slot != ~size_t{}) {
      m += per_slot * size;
    }
    return m;
  }
};

}  // namespace hashtable_debug_internal
}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl

#undef ABSL_SWISSTABLE_ENABLE_GENERATIONS

#endif  // GRAVEYARD_CONTAINER_INTERNAL_RAW_HASH_SET_H_
