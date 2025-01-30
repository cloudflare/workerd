#include <workerd/rust/async/promise-boilerplate.h>

namespace workerd::rust::async {

namespace {

template <typename T>
void unwrapNode(OwnPromiseNode node) {
  kj::_::ExceptionOr<kj::_::FixVoid<T>> result;

  node->get(result);
  KJ_IF_SOME(exception, kj::runCatchingExceptions([&node]() {
    node = nullptr;
  })) {
    result.addException(kj::mv(exception));
  }

  return kj::_::convertToReturn(kj::mv(result));
}

}  // namespace

// TODO(now): Generate boilerplate with a macro.
OwnPromiseNode promise_into_own_promise_node_void(PromiseVoid promise) {
  return kj::_::PromiseNode::from(kj::mv(promise));
};
void promise_drop_in_place_void(PtrPromiseVoid promise) {
  kj::dtor(*promise);
}
void own_promise_node_unwrap_void(OwnPromiseNode node) {
  return unwrapNode<void>(kj::mv(node));
}

}  // namespace workerd::rust::async
