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

// A Response kj::HttpService::Response implementation that records the status
// code on the response.
class SimpleResponseObserver final: public kj::HttpService::Response {
 public:
  SimpleResponseObserver(kj::uint* statusCode, kj::HttpService::Response& response)
      : inner(response),
        statusCode(statusCode) {}
  KJ_DISALLOW_COPY_AND_MOVE(SimpleResponseObserver);

  kj::Own<kj::AsyncOutputStream> send(kj::uint status,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    *statusCode = status;
    return inner.send(status, statusText, headers, expectedBodySize);
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    return inner.acceptWebSocket(headers);
  }

 private:
  kj::HttpService::Response& inner;
  kj::uint* statusCode;
};

}  // namespace workerd
