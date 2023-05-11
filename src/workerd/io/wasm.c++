#include "wasm.h"
#include <cstdint>
#include <workerd/io/io-context.h>

namespace workerd {

struct WasmArena {
  WasmArena(FreestandingWasmContext& context) : context(context) {}
  WasmArena(WasmArena&&) = default;
  KJ_DISALLOW_COPY(WasmArena);

  // needs to be called before destruction
  void free(jsg::Lock& js) {
    for (auto& ptr: toFree) {
      context.free(js, ptr.ptr, ptr.len);
    }
  }

  // main alloc routine, all the others should go through here
  template<typename T>
  WasmSlice<T> alloc(jsg::Lock& js, uint32_t len) {
    uint32_t allocSize = len * sizeof(T);

    // big allocations always go to the app
    if (allocSize > WASM_PAGE_SIZE) {
      return { .ptr = appAlloc(js, allocSize).ptr.as<T>(), .len = len };
    }

    // request new page if current page is too small
    if (currentPage.len < allocSize) {
      currentPage = appAlloc(js, WASM_PAGE_SIZE);
    }

    // shrink current page
    auto ptr = currentPage.ptr.as<T>();
    currentPage.ptr.ptr += allocSize;
    currentPage.len -= allocSize;

    return { .ptr = ptr, .len = len };
  }

  template<typename T>
  WasmPointer<T> alloc(jsg::Lock& js) {
    return alloc<T>(js, 1).ptr;
  }

  wit::cloudflare_string_t alloc(jsg::Lock& js, kj::StringPtr str) {
    auto result = alloc<char>(js, str.size());
    memcpy(result(memory(js)).begin(), str.begin(), str.size());
    return result;
  }

private:
  // ask application for memory
  WasmSlice<kj::byte> appAlloc(jsg::Lock& js, uint32_t len) {
    auto ptr = context.alloc(js, len);
    auto slice = WasmSlice<kj::byte>{.ptr = ptr, .len = len};
    toFree.add(slice);
    return slice;
  }

  v8::Local<v8::WasmMemoryObject> memory(jsg::Lock& js) {
    return KJ_REQUIRE_NONNULL(context.exports).memory.getHandle(js);
  }

  FreestandingWasmContext& context;
  kj::Vector<WasmSlice<kj::byte>> toFree;
  WasmSlice<kj::byte> currentPage = {{0}, 0};
};

kj::Promise<void> FreestandingWasmContext::request(
    Worker::Lock& lock,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::HttpService::Response& response) {
  auto& exports = KJ_REQUIRE_NONNULL(this->exports);
  auto memory = exports.memory.getHandle(lock);
  WasmArena arena(*this);

  // prepare request
  auto wasmRequest = arena.alloc<wit::http_request_t>(lock);
  {
    auto requestPtr = wasmRequest(memory);
    requestPtr->url = arena.alloc(lock, url);

    requestPtr->headers = arena.alloc<wit::http_tuple2_string_string_t>(lock, headers.size());
    auto headersArray = requestPtr->headers(memory);
    size_t i = 0;
    headers.forEach([&](kj::StringPtr name, kj::StringPtr value) {
      headersArray[i].f0 = arena.alloc(lock, name);
      headersArray[i].f1 = arena.alloc(lock, value);
      i++;
    });
  }

  // prepare response
  auto wasmResponse = arena.alloc<wit::http_response_t>(lock);
  *wasmResponse(memory) = { .status = 200, .headers = {{0}, 0}, .body = {{0}, 0} };

  // call wasm
  exports.worker_fetch(lock, wasmRequest, wasmResponse);

  // send data back
  auto& ioContext = IoContext::current();
  kj::HttpHeaders responseHeaders(ioContext.getHeaderTable());
  auto bodyStream = response.send(wasmResponse(memory)->status, ""_kj, responseHeaders);
  auto body = wasmResponse(memory)->body(memory);
  auto promise = bodyStream->write(body.begin(), body.size());
  return promise.attach(kj::mv(bodyStream)).then(
    [arena = kj::mv(arena), &ioContext]() mutable {
      return ioContext.run([arena = kj::mv(arena)](Worker::Lock& lock) mutable {
        arena.free(lock);
      });
    }
  );
}

}
