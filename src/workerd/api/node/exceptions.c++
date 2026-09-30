#include "exceptions.h"

#include <workerd/rust/jsg/ffi-inl.h>
#include <workerd/rust/node-exceptions/lib.rs.h>

#include <kj-rs/convert.h>

namespace workerd::api::node {

namespace rust_exc = ::workerd::rust::node_exceptions;

// Maps the C++ NodeExceptionCode to the Rust bridge enum. Following the kj-rs
// convert.h idiom, this is exposed as a `fromImpl` overload so callers use
// `kj::from<rust_exc::NodeExceptionCode>(code)`; ADL finds this overload via the
// `NodeExceptionCode` argument's namespace. It is a `static` (rather than
// anonymous-namespace) function because Clang's ADL does not consider
// unnamed-namespace functions. The switch has no `default` so the compiler flags
// any variant that drifts out of sync between the two enums.
static rust_exc::NodeExceptionCode fromImpl(rust_exc::NodeExceptionCode*, NodeExceptionCode code) {
  switch (code) {
    case NodeExceptionCode::ERR_FS_CP_EEXIST:
      return rust_exc::NodeExceptionCode::ErrFsCpEexist;
    case NodeExceptionCode::ERR_FS_CP_DIR_TO_NON_DIR:
      return rust_exc::NodeExceptionCode::ErrFsCpDirToNonDir;
    case NodeExceptionCode::ERR_FS_CP_EINVAL:
      return rust_exc::NodeExceptionCode::ErrFsCpEinval;
    case NodeExceptionCode::ERR_FS_CP_NON_DIR_TO_DIR:
      return rust_exc::NodeExceptionCode::ErrFsCpNonDirToDir;
    case NodeExceptionCode::ERR_FS_EISDIR:
      return rust_exc::NodeExceptionCode::ErrFsEisdir;
  }
  KJ_UNREACHABLE;
}

jsg::JsValue createNodeException(
    jsg::Lock& js, NodeExceptionCode code, JsErrorType type, kj::StringPtr message) {
  // Exception messages may be arbitrary, potentially non-UTF-8 byte strings, so they're passed
  // as raw bytes (kj::none for null) rather than ::rust::Str (which validates UTF-8 and throws);
  // V8 then renders invalid sequences lossily.
  return jsg::JsValue(::workerd::rust::jsg::local_from_ffi<v8::Value>(
      rust_exc::create_node_exception(js.v8Isolate, kj::from<rust_exc::NodeExceptionCode>(code),
          type, message == nullptr ? kj::none : kj::Maybe(message.asBytes().as<kj_rs::Rust>()))));
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
  return jsg::JsValue(::workerd::rust::jsg::local_from_ffi<v8::Value>(
      rust_exc::create_uv_exception(js.v8Isolate, errorno, syscall.as<kj_rs::RustUncheckedUtf8>(),
          message == nullptr ? kj::none : kj::Maybe(message.asBytes().as<kj_rs::Rust>()),
          path == nullptr ? kj::none : kj::Maybe(path.asBytes().as<kj_rs::Rust>()),
          dest == nullptr ? kj::none : kj::Maybe(dest.asBytes().as<kj_rs::Rust>()))));
}

}  // namespace workerd::api::node
