// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "batch-queue.h"
#include <kj/test.h>

namespace workerd {
namespace {

static constexpr auto INITIAL_CAPACITY = 8, MAX_CAPACITY = 100;

KJ_TEST("BatchQueue basic operations") {
  BatchQueue<int> batchQueue{INITIAL_CAPACITY, MAX_CAPACITY};

  KJ_EXPECT(batchQueue.empty());
  KJ_EXPECT(batchQueue.size() == 0);

  for ([[maybe_unused]] auto item: batchQueue.pop().asArrayPtr()) {
    KJ_FAIL_EXPECT("Should have been empty");
  }

  batchQueue.push(1);
  KJ_EXPECT(!batchQueue.empty());
  KJ_EXPECT(batchQueue.size() == 1);
  batchQueue.push(2);
  KJ_EXPECT(batchQueue.size() == 2);

  int count = 0;
  for (auto item: batchQueue.pop().asArrayPtr()) {
    KJ_EXPECT(item == ++count);
  }
}

KJ_TEST("BatchQueue::Batch clears the pop buffer when it is destroyed") {
  struct DestructionDetector {
    DestructionDetector(uint& count): count(count) {}
    ~DestructionDetector() noexcept(false) {
      ++count;
    }
    KJ_DISALLOW_COPY_AND_MOVE(DestructionDetector);
    uint& count;
  };

  BatchQueue<kj::Own<DestructionDetector>> batchQueue{INITIAL_CAPACITY, MAX_CAPACITY};

  uint count = 0;
  batchQueue.push(kj::heap<DestructionDetector>(count));
  {
    auto batch = batchQueue.pop();
    KJ_EXPECT(count == 0);
  }
  KJ_EXPECT(count == 1);
}

KJ_TEST("BatchQueue throws if two pop() operations run concurrently") {
  BatchQueue<int> batchQueue{INITIAL_CAPACITY, MAX_CAPACITY};

  batchQueue.push(123);
  auto batch0 = batchQueue.pop();
  KJ_EXPECT_THROW_MESSAGE("pop()'s previous result not yet destroyed", batchQueue.pop());
}

KJ_TEST("BatchQueue uses two buffers") {
  BatchQueue<int> batchQueue{INITIAL_CAPACITY, MAX_CAPACITY};

  batchQueue.push(123);
  auto buffer0 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer1 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer2 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer3 = batchQueue.pop().asArrayPtr();

  KJ_EXPECT(buffer0.begin() != buffer1.begin());
  KJ_EXPECT(buffer0.begin() == buffer2.begin());
  KJ_EXPECT(buffer1.begin() == buffer3.begin());
}

KJ_TEST("BatchQueue reconstructs buffers if they grow above maxCapacity") {
  BatchQueue<int> batchQueue{INITIAL_CAPACITY, MAX_CAPACITY};

  for (auto i = 0; i < MAX_CAPACITY + 1; ++i) {
    batchQueue.push(i);
  }
  auto buffer0 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer1 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer2 = batchQueue.pop().asArrayPtr();
  batchQueue.push(123);
  auto buffer3 = batchQueue.pop().asArrayPtr();

  KJ_EXPECT(buffer0.begin() != buffer1.begin());
  // This next expectation is only reliable because ~Batch() constructs the next buffer before
  // destroying the old one.
  KJ_EXPECT(buffer0.begin() != buffer2.begin());
  KJ_EXPECT(buffer1.begin() == buffer3.begin());
}

}  // namespace
}  // namespace workerd
