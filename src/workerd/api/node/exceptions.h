#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd::api::node {

// Utilities for creating Node.js-style exceptions.

// Most Node.js exceptions are represented as either Error,
// TypeError, or RangeError.
enum class JsErrorType {
  Error,
  TypeError,
  RangeError,
};

// Node.js Exception Codes
#define NODE_EXCEPTION_CODE_LIST(V)                                                                \
  V(ERR_FS_CP_EEXIST)                                                                              \
  V(ERR_FS_CP_DIR_TO_NON_DIR)                                                                      \
  V(ERR_FS_CP_NON_DIR_TO_DIR)                                                                      \
  V(ERR_FS_EISDIR)

enum class NodeExceptionCode {
#define V(name) name,
  NODE_EXCEPTION_CODE_LIST(V)
#undef V
};

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
#define UV_ERRNO_MAP(V)                                                                            \
  V(EACCES, "permission denied")                                                                   \
  V(EBADF, "bad file descriptor")                                                                  \
  V(EEXIST, "file already exists")                                                                 \
  V(EFBIG, "file too large")                                                                       \
  V(EINVAL, "invalid argument")                                                                    \
  V(EISDIR, "illegal operation on a directory")                                                    \
  V(ELOOP, "too many symbolic links encountered")                                                  \
  V(EMFILE, "too many open files")                                                                 \
  V(ENAMETOOLONG, "name too long")                                                                 \
  V(ENFILE, "file table overflow")                                                                 \
  V(ENOBUFS, "no buffer space available")                                                          \
  V(ENODEV, "no such device")                                                                      \
  V(ENOENT, "no such file or directory")                                                           \
  V(ENOMEM, "not enough memory")                                                                   \
  V(ENOSPC, "no space left on device")                                                             \
  V(ENOSYS, "function not implemented")                                                            \
  V(ENOTDIR, "not a directory")                                                                    \
  V(ENOTEMPTY, "directory not empty")                                                              \
  V(EPERM, "operation not permitted")                                                              \
  V(EMLINK, "too many links")                                                                      \
  V(EIO, "input/output error")

#define V(code, _) constexpr int UV_##code = -code;
UV_ERRNO_MAP(V)
#undef V

#define V(code, _)                                                                                 \
  [[noreturn]] inline void THROW_ERR_UV_##code(jsg::Lock& js, kj::StringPtr syscall,               \
      kj::StringPtr message = nullptr, kj::StringPtr path = nullptr,                               \
      kj::StringPtr dest = nullptr) {                                                              \
    throwUVException(js, UV_##code, syscall, message, path, dest);                                 \
  }
UV_ERRNO_MAP(V)
#undef V

}  // namespace workerd::api::node
