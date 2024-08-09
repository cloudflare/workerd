// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/compat/http.h>

namespace workerd {

// Attaches the given object to a `Request` so that it lives as long as the request's properties.
// The given object must support `kj::addRef()` (e.g. `kj::Refcount`).
template <typename T>
kj::HttpClient::Request attachToRequest(kj::HttpClient::Request req, T&& rcAttachment) {
  req.body = req.body.attach(kj::addRef(*rcAttachment));
  req.response = req.response.then(
      [rcAttachment = kj::mv(rcAttachment)](kj::HttpClient::Response&& response) mutable {
    response.body = response.body.attach(kj::mv(rcAttachment));
    return kj::mv(response);
  });
  return req;
}

// Attaches the given object to a `WebSocketResponse` promise so that it lives as long as the
// returned response's properties.
template <typename T>
kj::Promise<kj::HttpClient::WebSocketResponse> attachToWebSocketResponse(
    kj::Promise<kj::HttpClient::WebSocketResponse> promise, T&& attachment) {
  return promise.then(
      [attachment = kj::mv(attachment)](kj::HttpClient::WebSocketResponse&& response) mutable {
    KJ_SWITCH_ONEOF(response.webSocketOrBody) {
      KJ_CASE_ONEOF(stream, kj::Own<kj::AsyncInputStream>) {
        response.webSocketOrBody = stream.attach(kj::mv(attachment));
      }
      KJ_CASE_ONEOF(ws, kj::Own<kj::WebSocket>) {
        response.webSocketOrBody = ws.attach(kj::mv(attachment));
      }
    }
    return kj::mv(response);
  });
}

}  // namespace workerd
