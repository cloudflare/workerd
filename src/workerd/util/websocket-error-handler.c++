#include "websocket-error-handler.h"

#include <workerd/jsg/exception.h>

#include <kj/common.h>
#include <kj/exception.h>
#include <kj/string.h>

namespace workerd {

kj::Exception JsgifyWebSocketErrors::handleWebSocketProtocolError(
    kj::WebSocket::ProtocolError protocolError) {
  // The status code and description are meant to be public. Format them ourselves, because the
  // default description separates them with "; ", which marks internal details that JSG hides.
  return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__,
      kj::str(JSG_EXCEPTION(Error), ": WebSocket protocol error (", protocolError.statusCode,
          "): ", protocolError.description));
}

}  // namespace workerd
