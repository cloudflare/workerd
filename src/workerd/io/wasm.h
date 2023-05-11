#pragma once

#include <cstdint>
#include <cstring>
#include <limits>
#include <workerd/jsg/jsg.h>
#include <workerd/io/worker.h>

namespace workerd {

template<typename T>
struct WasmPointer {
  uint32_t ptr;

  template<typename G>
  inline WasmPointer<G> as() { return { ptr }; }

  T* operator()(v8::Local<v8::WasmMemoryObject> memory) {
    auto buffer = memory->Buffer();
    KJ_ASSERT(buffer->ByteLength() >= ptr);
    auto data = buffer->Data();
    return static_cast<T*>(static_cast<void*>(static_cast<kj::byte*>(data) + ptr));
  }
};

template<typename T>
struct WasmSlice {
  WasmPointer<T> ptr;
  uint32_t len;

  template<typename G>
  inline WasmSlice<G> as() { return {.ptr = ptr.template as<G>(), .len = len }; }

  kj::ArrayPtr<T> operator()(v8::Local<v8::WasmMemoryObject> memory) {
    auto buffer = memory->Buffer();
    KJ_ASSERT(buffer->ByteLength() >= ptr.ptr);
    KJ_ASSERT(static_cast<size_t>(ptr.ptr) + static_cast<size_t>(len) <= std::numeric_limits<uint32_t>::max());
    KJ_ASSERT(buffer->ByteLength() >= ptr.ptr + len);

    auto data = buffer->Data();
    return kj::ArrayPtr<T>(
      static_cast<T*>(static_cast<void*>(static_cast<kj::byte*>(data) + ptr.ptr)),
      len);
  }
};

template <typename TypeWrapper>
struct WasmPointerWrapper {
  template<typename T>
  static constexpr const char* getName(WasmPointer<T>*) {
    return "WasmPointer<?>";
  }

  template<typename T>
  v8::Local<v8::Number> wrap(
      v8::Local<v8::Context> context, kj::Maybe<v8::Local<v8::Object>> creator,
      const WasmPointer<T>& pointer) {
    return v8::Number::New(context->GetIsolate(), pointer.ptr);
  }


  template<typename T>
  kj::Maybe<WasmPointer<T>> tryUnwrap(
        v8::Local<v8::Context> context, v8::Local<v8::Value> handle, WasmPointer<T>*,
        kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return WasmPointer<T> { .ptr = handle->Uint32Value(context).ToChecked() };
  }

  template<typename T>
  v8::Local<v8::Context> newContext(v8::Isolate* isolate, WasmPointer<T> value) = delete;

  template <typename T, bool isContext = false>
  v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, WasmPointer<T>*) = delete;
};

namespace wit {

using cloudflare_string_t = WasmSlice<char>;

// auto-generated code by bindgen
typedef uint32_t http_response_handle_t;

typedef struct {
  cloudflare_string_t f0;
  cloudflare_string_t f1;
} http_tuple2_string_string_t;

using http_list_tuple2_string_string_t = WasmSlice<http_tuple2_string_string_t>;

typedef struct {
  uint16_t status;
  http_list_tuple2_string_string_t headers;
  cloudflare_string_t body;
} http_response_t;

typedef struct {
  cloudflare_string_t url;
  http_list_tuple2_string_string_t headers;
  cloudflare_string_t body;
} http_request_t;

typedef http_request_t worker_request_t;

typedef http_response_t worker_response_t;

}

// Object corresponding to WasmInstance.exports and contains all exported functions and objects
// from the wasm
struct WasmExports {
  // main fetch handler
  jsg::Function<void(WasmPointer<wit::http_request_t> request, WasmPointer<wit::http_response_t> response)> worker_fetch;

  // interface to the app memory management system
  jsg::Function<WasmPointer<kj::byte> (uint32_t size)> alloc;
  jsg::Function<void (WasmPointer<kj::byte> ptr, uint32_t size)> free;

  // wasm memory
  jsg::V8Ref<v8::WasmMemoryObject> memory;

  JSG_STRUCT(worker_fetch, alloc, free, memory);
};

static constexpr auto WASM_PAGE_SIZE = 65536;

struct FreestandingWasmContext: public jsg::Object {
  FreestandingWasmContext(v8::Isolate* isolate) : isolate(isolate) {}

  wit::http_response_handle_t http_fetch(WasmPointer<wit::http_request_t> req) {
    KJ_FAIL_REQUIRE("NOT IMPLEMENTED");
  }

  void console_log(WasmPointer<wit::cloudflare_string_t> str) {
    auto mem = KJ_REQUIRE_NONNULL(exports).memory.getHandle(isolate);
    auto message = (*str(mem))(mem);
    KJ_DBG("console_log", message);
  }

  JSG_RESOURCE_TYPE(FreestandingWasmContext) {
    JSG_METHOD(console_log);
  };

  kj::Promise<void> request(
      Worker::Lock& lock,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::HttpService::Response& response);

  WasmPointer<kj::byte> alloc(jsg::Lock& js, uint32_t len) {
    KJ_ASSERT(len >= WASM_PAGE_SIZE);
    if (len == WASM_PAGE_SIZE && freePages.size() > 0) {
      auto result = freePages.back();
      freePages.removeLast();
      return result;
    }
    KJ_DBG("alloc", len);
    return KJ_REQUIRE_NONNULL(exports).alloc(js, len);
  }

  void free(jsg::Lock& js, WasmPointer<kj::byte> ptr, uint32_t len) {
    KJ_ASSERT(len >= WASM_PAGE_SIZE);
    if (len == WASM_PAGE_SIZE) {
      freePages.add(ptr);
    } else {
      KJ_DBG("free", len);
      KJ_REQUIRE_NONNULL(exports).free(js, ptr, len);
    }
  }

  v8::Isolate* isolate;

  // todo: initialize in constructor
  kj::Maybe<WasmExports> exports;
  kj::Vector<WasmPointer<kj::byte>> freePages;
};

#define EW_WASM_ISOLATE_TYPES                                           \
  WasmExports,                                                          \
  jsg::TypeWrapperExtension<WasmPointerWrapper>,                        \
  FreestandingWasmContext
}
