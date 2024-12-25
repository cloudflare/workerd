#include <workerd/rust/async/await.h>

namespace workerd::rust::async {

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

BoxFutureVoidAwaiter operator co_await(kj::_::CoroutineBase::Await<BoxFutureVoid&> await) {
  return BoxFutureVoidAwaiter{await.coroutine, kj::mv(await.awaitable)};
}

}  // namespace workerd::rust::async
