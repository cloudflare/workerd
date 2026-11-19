#include "exceptions.h"

#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/node-exceptions/lib.rs.h>

#include <kj-rs/convert.h>

namespace workerd::api::node {

namespace rust_exc = ::workerd::rust::node_exceptions;

// An empty result means the Rust code failed or panicked, leaving an exception pending.
static jsg::JsValue fromRust(::workerd::rust::jsg::MaybeLocal value) {
  v8::Local<v8::Value> local;
  if (!::workerd::rust::jsg::maybe_local_from_ffi<v8::Value>(kj::mv(value)).ToLocal(&local)) {
    throw jsg::JsExceptionThrown();
  }
  return jsg::JsValue(local);
}

jsg::JsValue createNodeException(
    jsg::Lock& js, NodeExceptionCode code, JsErrorType type, kj::StringPtr message) {
  // Exception messages may be arbitrary, potentially non-UTF-8 byte strings, so they're passed
  // as raw bytes (kj::none for null) rather than ::rust::Str (which validates UTF-8 and throws);
  // V8 then renders invalid sequences lossily.
  return fromRust(rust_exc::create_node_exception(js.v8Isolate, code, type,
      message == nullptr ? kj::none : kj::Maybe(message.asBytes().as<kj_rs::Rust>())));
}

jsg::JsValue createUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message,
    kj::StringPtr path,
    kj::StringPtr dest) {
  // syscall is always a fixed ASCII literal, so it's passed as a UTF-8 ::rust::Str. message/path/
  // dest may be arbitrary, potentially non-UTF-8 byte strings, so they're passed as raw bytes
  // (kj::none for null) and rendered lossily by V8.
  return fromRust(
      rust_exc::create_uv_exception(js.v8Isolate, errorno, syscall.as<kj_rs::RustUncheckedUtf8>(),
          message == nullptr ? kj::none : kj::Maybe(message.asBytes().as<kj_rs::Rust>()),
          path == nullptr ? kj::none : kj::Maybe(path.asBytes().as<kj_rs::Rust>()),
          dest == nullptr ? kj::none : kj::Maybe(dest.asBytes().as<kj_rs::Rust>())));
}

}  // namespace workerd::api::node
