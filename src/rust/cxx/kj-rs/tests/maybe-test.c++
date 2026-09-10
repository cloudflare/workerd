#include "kj-rs-demo/lib.rs.h"

#include <kj/test.h>

namespace kj_rs_demo {
namespace {

KJ_TEST("struct with maybe") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto promise = pass_struct_with_maybe(::kj_rs_demo::StructWithMaybe());
}

}  // namespace
}  // namespace kj_rs_demo