#pragma once

#include <rust/cxx.h>

#include <kj/async.h>
#include <kj/debug.h>

// These types are shared with rust
namespace kj_rs {

using OwnPromiseNode = kj::_::OwnPromiseNode;

void own_promise_node_drop_in_place(OwnPromiseNode*);

namespace repr {

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type-c-linkage"

using Result = ::rust::repr::Result;

// ::kj_rs::promise::UnwrapCallback
using UnwrapCallback = Result (*)(void /* kj::_::PromiseNode */* node, void /* T */* ret);

// ::kj_rs::promise::KjPromiseNodeImpl
struct KjPromiseNodeImpl {
  template <typename T>
  inline KjPromiseNodeImpl(kj::Promise<T>&& p);

  kj::_::PromiseNode* node;
  repr::UnwrapCallback unwrap;
};

#pragma GCC diagnostic pop
}  // namespace repr

namespace _ {
template <typename T>
repr::Result unwrapCallback(void* nodePtr, void* ret) noexcept {
  auto node = OwnPromiseNode(reinterpret_cast<kj::_::PromiseNode*>(nodePtr));

  kj::_::ExceptionOr<kj::_::FixVoid<T>> result;
  node->get(result);

  KJ_IF_SOME(e, kj::runCatchingExceptions([&node]() { node = nullptr; })) {
    result.addException(kj::mv(e));
  }

  KJ_IF_SOME(e, result.exception) {
    return repr::Result::error(kj::mv(e));
  } else {
    if constexpr (!kj::isSameType<T, void>()) {
      new (reinterpret_cast<T*>(ret)) T(::kj::mv(KJ_ASSERT_NONNULL(result.value)));
    }
    return repr::Result::ok();
  }
}
}  // namespace _

namespace repr {

template <typename T>
inline KjPromiseNodeImpl::KjPromiseNodeImpl(kj::Promise<T>&& p)
    : node(kj::_::PromiseNode::from(kj::mv(p)).template disown<kj::_::PromiseDisposer>()),
      unwrap(::kj_rs::_::unwrapCallback<T>) {}

}  // namespace repr

}  // namespace kj_rs

namespace rust {

// OwnPromiseNodes happen to follow Rust move semantics.
template <>
struct IsRelocatable<::kj_rs::OwnPromiseNode>: std::true_type {};

}  // namespace rust
