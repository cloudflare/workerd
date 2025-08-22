#include "checked-queue.h"

#include <kj/test.h>

namespace workerd::util {

struct MovableNotCopyable {
  MovableNotCopyable(int value): value(value) {}
  MovableNotCopyable(MovableNotCopyable&&) = default;
  MovableNotCopyable& operator=(MovableNotCopyable&&) = default;
  KJ_DISALLOW_COPY(MovableNotCopyable);

  int value = 0;
};

struct Regular {
  int value = 0;
};

KJ_TEST("CheckedQueue works - Regular") {
  Queue<Regular> queue;
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(queue.pop() == kj::none);
  KJ_ASSERT(queue.peek() == kj::none);
  KJ_ASSERT(
      queue.drainTo([](Regular&&) { KJ_FAIL_ASSERT("Should not be called on empty queue"); }) == 0);
  queue.clear();
  queue.push(Regular{1});
  KJ_ASSERT(!queue.empty());
  KJ_ASSERT(queue.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.pop()).value == 1);
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(queue.pop() == kj::none);
  KJ_ASSERT(queue.peek() == kj::none);
  queue.push(Regular{2});
  KJ_ASSERT(queue.drainTo([](Regular&& item) { KJ_ASSERT(item.value == 2); }) == 1);
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);
}

KJ_TEST("CheckedQueue works - MovableNotCopyable") {
  Queue<MovableNotCopyable> queue;
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(queue.pop() == kj::none);
  KJ_ASSERT(queue.peek() == kj::none);
  KJ_ASSERT(queue.drainTo([](MovableNotCopyable&&) {
    KJ_FAIL_ASSERT("Should not be called on empty queue");
  }) == 0);
  queue.clear();
  queue.push(MovableNotCopyable(1));
  KJ_ASSERT(!queue.empty());
  KJ_ASSERT(queue.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.pop()).value == 1);
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);
  KJ_ASSERT(queue.pop() == kj::none);
  KJ_ASSERT(queue.peek() == kj::none);
  queue.push(MovableNotCopyable(2));
  KJ_ASSERT(queue.drainTo([](MovableNotCopyable&& item) { KJ_ASSERT(item.value == 2); }) == 1);
  KJ_ASSERT(queue.empty());
  KJ_ASSERT(queue.size() == 0);

  queue.emplace(1);
  KJ_ASSERT(!queue.empty());
  KJ_ASSERT(queue.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.pop()).value == 1);
  KJ_ASSERT(queue.empty());

  Queue<MovableNotCopyable> queue2;
  queue2.push(MovableNotCopyable(3));
  KJ_ASSERT(!queue2.empty());
  KJ_ASSERT(queue2.size() == 1);
  queue.swap(queue2);
  KJ_ASSERT(queue.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 3);
  KJ_ASSERT(queue2.size() == 0);
  KJ_ASSERT(queue2.peek() == kj::none);

  queue.emplace(2);
  KJ_ASSERT(queue.size() == 2);
  KJ_ASSERT(queue.deleteIf([](const auto& item) { return item.value == 3; }) == 1);
  KJ_ASSERT(queue.size() == 1);
  KJ_ASSERT(KJ_ASSERT_NONNULL(queue.peek()).value == 2);

  queue.emplace(4);
  KJ_ASSERT(queue.size() == 2);
  KJ_ASSERT(queue.forEach([](const auto& item) {
    KJ_ASSERT(item.value == 2);
    return false;
  }) == 1);

  queue.emplace(5);
  KJ_ASSERT(queue.size() == 3);
  auto removed = KJ_ASSERT_NONNULL(queue.takeIf([](const auto& item) { return item.value == 5; }));
  KJ_ASSERT(removed.value == 5);
  KJ_ASSERT(queue.size() == 2);
}

}  // namespace workerd::util
