#pragma once

#include <kj/compat/http.h>
#include <kj/exception.h>

namespace workerd {
class JsgifyWebSocketErrors final: public kj::WebSocketErrorHandler {
 public:
  // Does what kj::WebSocketErrorHandler does, but formatted as a JSG exception.
  kj::Exception handleWebSocketProtocolError(kj::WebSocket::ProtocolError protocolError) override;
};

}  // namespace workerd
