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

#include "graveyard/container/internal/raw_hash_set.h"

#include <atomic>
#include <cstddef>
#include <cstring>

#include "absl/base/config.h"
#include "absl/base/dynamic_annotations.h"
#include "absl/hash/hash.h"

namespace graveyard {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

#if 0
// A single block of empty control bytes for tables without any slots allocated.
// This enables removing a branch in the hot path of find().
alignas(16) ABSL_CONST_INIT ABSL_DLL const ctrl_t kEmptyGroup[16] = {
    ctrl_t::kSentinel, ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty,
    ctrl_t::kEmpty,    ctrl_t::kEmpty, ctrl_t::kEmpty, ctrl_t::kEmpty};
#endif
alignas(16) ABSL_CONST_INIT ABSL_DLL const ctrl_t kEmptyGroup[16];

#ifdef ABSL_INTERNAL_NEED_REDUNDANT_CONSTEXPR_DECL
constexpr size_t Group::kWidth;
#endif

namespace {

// Returns "random" seed.
inline size_t RandomSeed() {
#ifdef ABSL_HAVE_THREAD_LOCAL
  static thread_local size_t counter = 0;
  // On Linux kernels >= 5.4 the MSAN runtime has a false-positive when
  // accessing thread local storage data from loaded libraries
  // (https://github.com/google/sanitizers/issues/1265), for this reason counter
  // needs to be annotated as initialized.
  ABSL_ANNOTATE_MEMORY_IS_INITIALIZED(&counter, sizeof(size_t));
  size_t value = ++counter;
#else   // ABSL_HAVE_THREAD_LOCAL
  static std::atomic<size_t> counter(0);
  size_t value = counter.fetch_add(1, std::memory_order_relaxed);
#endif  // ABSL_HAVE_THREAD_LOCAL
  return value ^ static_cast<size_t>(reinterpret_cast<uintptr_t>(&counter));
}

}  // namespace

GenerationType* EmptyGeneration() {
  if (SwisstableGenerationsEnabled()) {
    constexpr size_t kNumEmptyGenerations = 1024;
    static constexpr GenerationType kEmptyGenerations[kNumEmptyGenerations]{};
    return const_cast<GenerationType*>(
        &kEmptyGenerations[RandomSeed() % kNumEmptyGenerations]);
  }
  return nullptr;
}

bool CommonFieldsGenerationInfoEnabled::
    should_rehash_for_bug_detection_on_insert(const ctrl_t* ctrl,
                                              size_t capacity) const {
  if (reserved_growth_ == kReservedGrowthJustRanOut) return true;
  if (reserved_growth_ > 0) return false;
  // Note: we can't use the abseil-random library because abseil-random
  // depends on swisstable. We want to return true with probability
  // `min(1, RehashProbabilityConstant() / capacity())`. In order to do this,
  // we probe based on a random hash and see if the offset is less than
  // RehashProbabilityConstant().
  return H1(absl::HashOf(RandomSeed()), capacity) <
         RehashProbabilityConstant();
}

#if 0
bool ShouldInsertBackwards(size_t hash, const ctrl_t* ctrl) {
  // To avoid problems with weak hashes and single bit tests, we use % 13.
  // TODO(kfm,sbenza): revisit after we do unconditional mixing
  return (H1(hash, ctrl) ^ RandomSeed()) % 13 > 6;
}
#endif

void ConvertDeletedToEmptyAndFullToDeleted(ctrl_t* ctrl, size_t capacity) {
  assert(ctrl[capacity] == ctrl_t::kSentinel);
  assert(IsValidCapacity(capacity));
  for (ctrl_t* pos = ctrl; pos < ctrl + capacity; pos += Group::kWidth) {
    Group{pos}.ConvertSpecialToEmptyAndFullToDeleted(pos);
  }
  // Copy the cloned ctrl bytes.
  std::memcpy(ctrl + capacity + 1, ctrl, NumClonedBytes());
#if 0
  ctrl[capacity] = ctrl_t::kSentinel;
#endif
}

// Return address of the ith slot in slots where each slot occupies slot_size.
static inline void* SlotAddress(void* slot_array, size_t slot,
                                size_t slot_size) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot_array) +
                                 (slot * slot_size));
}

// Return the address of the slot just after slot assuming each slot
// has the specified size.
static inline void* NextSlot(void* slot, size_t slot_size) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) + slot_size);
}

// Return the address of the slot just before slot assuming each slot
// has the specified size.
static inline void* PrevSlot(void* slot, size_t slot_size) {
  return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) - slot_size);
}

void EraseMetaOnly(CommonFields& c, ctrl_t* it, size_t slot_size) {
  assert(IsFull(*it) && "erasing a dangling iterator");
  --c.size_;
  const auto index = static_cast<size_t>(it - c.control_);
  const size_t index_before = (index - Group::kWidth) & c.capacity_;
  const auto empty_after = Group(it).MaskEmpty();
  const auto empty_before = Group(c.control_ + index_before).MaskEmpty();

  // We count how many consecutive non empties we have to the right and to the
  // left of `it`. If the sum is >= kWidth then there is at least one probe
  // window that might have seen a full group.
  bool was_never_full = empty_before && empty_after &&
                        static_cast<size_t>(empty_after.TrailingZeros()) +
                                empty_before.LeadingZeros() <
                            Group::kWidth;

#if 0
  SetCtrl(c, index, was_never_full ? ctrl_t::kEmpty : ctrl_t::kDeleted,
          slot_size);
#endif
  c.growth_left() += (was_never_full ? 1 : 0);
  c.infoz().RecordErase();
}

void ClearBackingArray(CommonFields& c, const PolicyFunctions& policy,
                       bool reuse) {
  c.size_ = 0;
  if (reuse) {
    ResetCtrl(c, policy.slot_size);
    c.infoz().RecordStorageChanged(0, c.capacity_);
  } else {
    void* set = &c;
    (*policy.dealloc)(set, policy, c.control_, c.slots_, c.capacity_);
    c.control_ = EmptyGroup();
    c.set_generation_ptr(EmptyGeneration());
    c.slots_ = nullptr;
    c.capacity_ = 0;
    c.growth_left() = 0;
    c.infoz().RecordClearedReservation();
    assert(c.size_ == 0);
    c.infoz().RecordStorageChanged(0, 0);
  }
}

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl
