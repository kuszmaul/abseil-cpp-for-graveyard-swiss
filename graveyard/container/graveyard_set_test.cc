#include "graveyard/container/flat_hash_set.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

TEST(GraveyardTest, Basic) {
  graveyard::flat_hash_set<uint64_t> table;
  table.insert(0);
}
