#include "websocket-error-handler.h"

#include <workerd/jsg/exception.h>

#include <kj/common.h>
#include <kj/exception.h>
#include <kj/string.h>

namespace workerd {

kj::Exception JsgifyWebSocketErrors::handleWebSocketProtocolError(
    kj::WebSocket::ProtocolError protocolError) {
  kj::Exception baseExc =
      kj::WebSocketErrorHandler::handleWebSocketProtocolError(kj::mv(protocolError));
  auto newDescription = kj::str(JSG_EXCEPTION(Error), ": ", baseExc.getDescription());
  return kj::Exception(
      baseExc.getType(), baseExc.getFile(), baseExc.getLine(), kj::mv(newDescription));
}

}  // namespace workerd
