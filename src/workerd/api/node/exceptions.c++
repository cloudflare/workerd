#include "exceptions.h"

namespace workerd::api::node {

namespace {
jsg::JsObject createJsError(jsg::Lock& js, JsErrorType type, kj::StringPtr message) {
  switch (type) {
    case JsErrorType::TypeError: {
      return KJ_ASSERT_NONNULL(js.typeError(message).tryCast<jsg::JsObject>());
    }
    case JsErrorType::RangeError: {
      return KJ_ASSERT_NONNULL(js.rangeError(message).tryCast<jsg::JsObject>());
    }
    case JsErrorType::Error: {
      KJ_FALLTHROUGH;
    }
    default: {
      return KJ_ASSERT_NONNULL(js.error(message).tryCast<jsg::JsObject>());
    }
  }
  KJ_UNREACHABLE;
}

constexpr kj::StringPtr getMessage(NodeExceptionCode code, kj::StringPtr message) {
  if (message != nullptr) return message;
  switch (code) {
    case NodeExceptionCode::ERR_FS_CP_EEXIST:
      return "File already exists";
    case NodeExceptionCode::ERR_FS_CP_DIR_TO_NON_DIR:
      return "Cannot copy directory to non-directory";
    case NodeExceptionCode::ERR_FS_CP_EINVAL:
      return "Invalid cp operation";
    case NodeExceptionCode::ERR_FS_CP_NON_DIR_TO_DIR:
      return "Cannot copy non-directory to directory";
    case NodeExceptionCode::ERR_FS_EISDIR:
      return "Expected a file but found a directory";
    default:
      return "Unknown Node.js exception";
  }
}

constexpr kj::StringPtr getCode(NodeExceptionCode code) {
#define V(name)                                                                                    \
  case NodeExceptionCode::name:                                                                    \
    return #name;
  switch (code) {
    NODE_EXCEPTION_CODE_LIST(V)
    default:
      return "UNKNOWN";
  }
#undef V
}
}  // namespace

jsg::JsValue createNodeException(
    jsg::Lock& js, NodeExceptionCode code, JsErrorType type, kj::StringPtr message) {
  auto err = createJsError(js, type, getMessage(code, message));
  err.set(js, "code"_kj, js.str(getCode(code)));
  return err;
}

namespace {
// Generates the error name.
#define UV_ERR_NAME_GEN(name, _)                                                                   \
  case UV_##name:                                                                                  \
    return js.str(#name##_kj);
jsg::JsValue uv_err_name(jsg::Lock& js, int err) {
  switch (err) { UV_ERRNO_MAP(UV_ERR_NAME_GEN) }
  return js.str("UNKNOWN"_kj);
}
#undef UV_ERR_NAME_GEN
}  // namespace

jsg::JsValue createUVException(jsg::Lock& js,
    int errorno,
    kj::StringPtr syscall,
    kj::StringPtr message,
    kj::StringPtr path,
    kj::StringPtr dest) {
  KJ_DASSERT(syscall != nullptr, "syscall must not be null");

  // Format the message to match Node.js format: "ENOENT: no such file or directory, open 'path'"
  kj::String formattedMessage;
  if (message == nullptr) {
    // Get the default error message
    kj::String defaultMsg;
    switch (errorno) {
#define UV_MSG_GEN(name, msg)                                                                      \
  case UV_##name:                                                                                  \
    defaultMsg = kj::str(msg);                                                                     \
    break;
      UV_ERRNO_MAP(UV_MSG_GEN)
#undef UV_MSG_GEN
      default:
        defaultMsg = kj::str("unknown error: ", errorno);
        break;
    }

    if (path != nullptr) {
      formattedMessage = kj::str(defaultMsg, ", ", syscall, " '", path, "'");
    } else {
      formattedMessage = kj::mv(defaultMsg);
    }
  } else {
    formattedMessage = kj::str(message);
  }

  jsg::JsObject obj = KJ_ASSERT_NONNULL(js.error(formattedMessage).tryCast<jsg::JsObject>());
  obj.set(js, "syscall"_kj, js.str(syscall));
  obj.set(js, "code"_kj, uv_err_name(js, errorno));

  if (path != nullptr) {
    obj.set(js, "path"_kj, js.str(path));
  }
  if (dest != nullptr) {
    obj.set(js, "dest"_kj, js.str(dest));
  }

  return obj;
}

}  // namespace workerd::api::node
