// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>

namespace workerd {

// Attaches the given object to a `Request` so that it lives as long as the request's properties.
// The given object must support `kj::addRef()` (e.g. `kj::Refcount`).
template<typename T>
kj::HttpClient::Request attachToRequest(kj::HttpClient::Request req, T&& rcAttachment) {
  req.body = req.body.attach(kj::addRef(*rcAttachment));
  req.response = ([](auto promise, T&& rcAttachment) mutable
      -> kj::Promise<kj::HttpClient::Response> {
    kj::HttpClient::Response response = co_await promise;
    response.body = response.body.attach(kj::mv(rcAttachment));
    co_return kj::mv(response);
  })(kj::mv(req.response), kj::mv(rcAttachment));
  return req;
}

// Attaches the given object to a `WebSocketResponse` promise so that it lives as long as the
// returned response's properties.
template<typename T>
kj::Promise<kj::HttpClient::WebSocketResponse> attachToWebSocketResponse(
    kj::Promise<kj::HttpClient::WebSocketResponse> promise, T&& attachment) {
  kj::HttpClient::WebSocketResponse&& response = co_await promise;
  KJ_SWITCH_ONEOF(response.webSocketOrBody) {
    KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncInputStream>) {
      response.webSocketOrBody = stream.attach(kj::mv(attachment));
    }
    KJ_CASE_ONEOF(ws, kj::Own<kj::WebSocket>) {
      response.webSocketOrBody = ws.attach(kj::mv(attachment));
    }
  }
  co_return kj::mv(response);
}

}  // namespace workerd
