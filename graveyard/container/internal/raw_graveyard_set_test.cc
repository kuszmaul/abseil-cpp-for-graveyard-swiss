// Copyright 2023 Bradley C. Kuszmaul
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

#include "absl/log/log.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using absl::container_internal::HashTableMemory;
using absl::container_internal::ctrl_t;
using absl::container_internal::search_distance_t;

static_assert(sizeof(ctrl_t) == 1);
static_assert(sizeof(search_distance_t) == 2);

struct uint64_layout {
  ctrl_t ctrl[14];
  search_distance_t search_distance;
  uint64_t slots[14];
};

static_assert(sizeof(uint64_layout) == 128);

TEST(HashTableMemoryTest, Uint64Move) {
  constexpr size_t kBinCount = 30;
  //struct uint64_layout data[kBinCount];
  HashTableMemory<14, uint64_t> hm(kBinCount);
  EXPECT_NE(hm.RawMemory(), nullptr);
  EXPECT_EQ(hm.RawMemory(), hm.RawMemory());
  const char *raw_memory = hm.RawMemory();
  HashTableMemory<14, uint64_t> hm2(std::move(hm));
  EXPECT_EQ(hm.RawMemory(), nullptr);
  EXPECT_EQ(hm2.RawMemory(), raw_memory);
  HashTableMemory<14, uint64_t> hm3 = std::move(hm2);
  EXPECT_EQ(hm2.RawMemory(), nullptr);
  EXPECT_EQ(hm3.RawMemory(), raw_memory);
  HashTableMemory<14, uint64_t> hm4(kBinCount + 1);
  std::swap(hm3, hm4);
  EXPECT_EQ(hm4.RawMemory(), raw_memory);
  EXPECT_EQ(hm3.PhysicalBinCount(), kBinCount + 1);
  EXPECT_EQ(hm4.PhysicalBinCount(), kBinCount);
}

TEST(HashTableMemoryTest, Uint64Offsets) {
  constexpr size_t kBinCount = 30;
  //struct uint64_layout data[kBinCount];
  HashTableMemory<14, uint64_t> hm(kBinCount);
  const char *raw_memory = hm.RawMemory();
  const uint64_layout *struct_memory = reinterpret_cast<const uint64_layout*>(raw_memory);
  EXPECT_EQ(reinterpret_cast<char*>(hm.ControlOf(0)), raw_memory);
  EXPECT_EQ(hm.ControlOf(0), &struct_memory->ctrl[0]);
  EXPECT_EQ(hm.ControlOf(0), &struct_memory[0].ctrl[0]);
  EXPECT_EQ(hm.ControlOf(1), &struct_memory[1].ctrl[0]);
  EXPECT_EQ(hm.SearchDistanceOf(0), &struct_memory[0].search_distance);
  EXPECT_EQ(hm.SearchDistanceOf(1), &struct_memory[1].search_distance);
  EXPECT_EQ(hm.SlotOf(0, 0), &struct_memory[0].slots[0]);
  EXPECT_EQ(hm.SlotOf(0, 1), &struct_memory[0].slots[1]);
  EXPECT_EQ(hm.SlotOf(0, 2), &struct_memory[0].slots[2]);
  EXPECT_EQ(hm.SlotOf(1, 0), &struct_memory[1].slots[0]);
  EXPECT_EQ(hm.SlotOf(1, 1), &struct_memory[1].slots[1]);
  EXPECT_EQ(hm.SlotOf(1, 2), &struct_memory[1].slots[2]);
  EXPECT_EQ(hm.SizeOf(), sizeof(*struct_memory) * kBinCount);
  EXPECT_EQ(hm.ControlOf(1) - hm.ControlOf(0), sizeof(uint64_layout));
  LOG(INFO) << "Difference=" << hm.ControlOf(1) - hm.ControlOf(1) << " struct size=" << sizeof(uint64_layout);
}

struct my_string {
  char *data;
  size_t size;
  size_t capacity;
};

struct my_string_layout {
  ctrl_t ctrl[14];
  search_distance_t search_distance;
  my_string slots[14];
};

TEST(HashTableMemoryTest, MyStringOffsets) {
  constexpr size_t kBinCount = 20;
  HashTableMemory<14, my_string> hm(kBinCount);
  const char *raw_memory = hm.RawMemory();
  const my_string_layout *struct_memory = reinterpret_cast<const my_string_layout*>(raw_memory);
  EXPECT_EQ(reinterpret_cast<char*>(hm.ControlOf(0)), raw_memory);
  EXPECT_EQ(hm.ControlOf(0), &struct_memory->ctrl[0]);
  EXPECT_EQ(hm.ControlOf(0), &struct_memory[0].ctrl[0]);
  EXPECT_EQ(hm.ControlOf(1), &struct_memory[1].ctrl[0]);
  EXPECT_EQ(hm.SearchDistanceOf(0), &struct_memory[0].search_distance);
  EXPECT_EQ(hm.SearchDistanceOf(1), &struct_memory[1].search_distance);
  EXPECT_EQ(hm.SlotOf(0, 0), &struct_memory[0].slots[0]);
  EXPECT_EQ(hm.SlotOf(0, 1), &struct_memory[0].slots[1]);
  EXPECT_EQ(hm.SlotOf(0, 2), &struct_memory[0].slots[2]);
  EXPECT_EQ(hm.SlotOf(1, 0), &struct_memory[1].slots[0]);
  EXPECT_EQ(hm.SlotOf(1, 1), &struct_memory[1].slots[1]);
  EXPECT_EQ(hm.SlotOf(1, 2), &struct_memory[1].slots[2]);
  EXPECT_EQ(hm.SizeOf(), sizeof(*struct_memory) * kBinCount);
  EXPECT_EQ(hm.ControlOf(1) - hm.ControlOf(0), sizeof(my_string_layout));
  LOG(INFO) << "Difference=" << hm.ControlOf(1) - hm.ControlOf(1) << " struct size=" << sizeof(my_string_layout);
}
