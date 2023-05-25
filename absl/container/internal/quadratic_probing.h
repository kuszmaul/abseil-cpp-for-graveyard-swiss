// This file implements quadratic probing, managing raw memory that can be
// converted to slot_pointers.

#ifndef ABSL_CONTAINER_INTERNAL_QUADRATIC_PROBING_H_
#define ABSL_CONTAINER_INTERNAL_QUADRATIC_PROBING_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

template <class SlotType, class Alloc>
class QuadraticProbing {
  // QuadraticProbing should be put into, e.g., `flat_hash_set`, as a member
  // variable.  TODO: Describe what this abstract is.

 public:
  using slot_type = SlotType;
  using allocator_type = Alloc;
  struct Iterator;

 private:
  using AllocTraits = std::allocator_traits<allocator_type>;

 public:

  explicit QuadraticProbing(const allocator_type& alloc, size_t reserved_size = 0);

  ~QuadraticProbing();

  void swap(QuadraticProbing& that) noexcept(
      std::is_nothrow_swappable<allocator_type>(
          typename AllocTraits::propagate_on_container_swap{}));

  // Looks for values that match hash, and uses `p(value)` to determine
  // which one, if any is found.  ALl the magic
  //Iterator Find(size_t hash, std::function<bool(const slot_type &slot)> predicate);

  // Assuming that we hae a value with hash `hash`, sets up a slot marked
  // to hold that value, and returns it.  The slot doesn't actually have the
  // value yet.
  Iterator PrepareInssert(size_t hash);

  // Markes the slot_type at iterator as not-present and destructs it.
  void Erase(Iterator iterator);

  bool NeedsRehash() const;

  // rehashes so that we can grow some fraction.
  void Rehash(const allocator_type& alloc);

  void CopyFrom(const allocator_type& alloc, const QuadraticProbing &table);

  // TODO: Figure out how to get the iterator generations into this properly.
  // We'd like not to repeat the generations code in the graveyard hashing
  // implementation.
  struct Iterator {

  };
  struct ConstIterator {

  };
 private:
};

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_CONTAINER_INTERNAL_QUADRATIC_PROBING_H_
