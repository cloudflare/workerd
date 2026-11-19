#pragma once

#include <workerd/api/node/exception-type.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// Utilities for creating Node.js-style exceptions. The Rust node-exceptions crate
// (`src/rust/node-exceptions/lib.rs`) builds the error objects.
//
// JsErrorType, NodeExceptionCode and NODE_EXCEPTION_CODE_LIST are defined in
// exception-type.h (a dependency-free header) so they can be shared with that
// crate as cxx extern enums.

// A Node.js style exception is just a JS error with a string "code" property.
jsg::JsValue createNodeException(
    jsg::Lock& js, NodeExceptionCode code, JsErrorType type, kj::StringPtr message = nullptr);

[[noreturn]] inline void throwNodeException(jsg::Lock& js,
    NodeExceptionCode code,
    JsErrorType type = JsErrorType::Error,
    kj::StringPtr message = nullptr) {
  js.throwException(createNodeException(js, code, type, message));
}

#define V(name)                                                                                    \
  [[noreturn]] inline void THROW_##name(jsg::Lock& js, kj::StringPtr message = nullptr) {          \
    throwNodeException(js, NodeExceptionCode::name, JsErrorType::Error, message);                  \
  }

NODE_EXCEPTION_CODE_LIST(V)
#undef V

// Create a Node.js-style "UVException". The UVException is an
// ordinary Error object with additional properties like code,
// syscall, path, and destination properties. It is primarily
// used to represent file system API errors.
jsg::JsValue createUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message = nullptr,
    kj::StringPtr path = nullptr,
    kj::StringPtr dest = nullptr);

// Throw a Node.js-style "UVException". The UVException is an
// ordinary Error object with additional properties like code,
// syscall, path, and destination properties. It is primarily
// used to represent file system API errors.
[[noreturn]] inline void throwUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message,
    kj::StringPtr path,
    kj::StringPtr dest) {
  js.throwException(createUVException(js, errorno, syscall, message, path, dest));
}

// This is an intentionally truncated list of the error codes that Node.js/libuv
// uses. We won't need all of the error codes that libuv uses.
//
// The Rust node-exceptions crate keeps its own copy of this list, with each
// code's name and default message (`UV_ERRNO_MAP` in
// `src/rust/node-exceptions/lib.rs`); the two must list the same codes. A code
// missing from the Rust copy gets the code "UNKNOWN".
#define UV_ERRNO_MAP(V)                                                                            \
  V(EACCES)                                                                                        \
  V(EBADF)                                                                                         \
  V(EEXIST)                                                                                        \
  V(EFBIG)                                                                                         \
  V(EINVAL)                                                                                        \
  V(EISDIR)                                                                                        \
  V(ELOOP)                                                                                         \
  V(EMFILE)                                                                                        \
  V(ENAMETOOLONG)                                                                                  \
  V(ENFILE)                                                                                        \
  V(ENOBUFS)                                                                                       \
  V(ENODEV)                                                                                        \
  V(ENOENT)                                                                                        \
  V(ENOMEM)                                                                                        \
  V(ENOSPC)                                                                                        \
  V(ENOSYS)                                                                                        \
  V(ENOTDIR)                                                                                       \
  V(ENOTEMPTY)                                                                                     \
  V(EPERM)                                                                                         \
  V(EMLINK)                                                                                        \
  V(EIO)

#define V(code) constexpr int UV_##code = -code;
UV_ERRNO_MAP(V)
#undef V

#define V(code)                                                                                    \
  [[noreturn]] inline void THROW_ERR_UV_##code(jsg::Lock& js, kj::StringPtr syscall,               \
      kj::StringPtr message = nullptr, kj::StringPtr path = nullptr,                               \
      kj::StringPtr dest = nullptr) {                                                              \
    throwUVException(js, UV_##code, syscall, message, path, dest);                                 \
  }
UV_ERRNO_MAP(V)
#undef V

}  // namespace workerd::api::node
