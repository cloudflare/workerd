#include "kj-rs-demo/lib.rs.h"
#include "kj-rs-demo/test-own.h"

#include <kj/test.h>

namespace kj_rs_demo {
namespace {

KJ_TEST("Rust drops a kj::Own<SecondBase> through the complete object's address") {
  KJ_EXPECT(rust_drop_recorded_two_base_driver());
}

KJ_TEST("Rust drops a heap TwoBase held as kj::Own<SecondBase> exactly once") {
  KJ_EXPECT(rust_drop_heap_two_base_driver() == 1);
}

}  // namespace
}  // namespace kj_rs_demo
