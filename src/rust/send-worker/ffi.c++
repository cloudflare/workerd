// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <kj/debug.h>

namespace workerd::rust::send_worker {

Subrequest::Subrequest(kj::Own<WorkerInterface> workerParam,
    kj::HttpMethod method,
    kj::StringPtr url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize)
    : worker(kj::mv(workerParam)),
      client(kj::newHttpClient(*worker)) {
  // The client copies the URL and the headers before it returns.
  auto request = client->request(method, url, headers, expectedBodySize);
  requestBody = kj::mv(request.body);
  pending = kj::mv(request.response);
}

kj::Own<kj::AsyncOutputStream> Subrequest::takeRequestBody() {
  auto body = kj::mv(KJ_REQUIRE_NONNULL(requestBody, "the request body was already taken"));
  requestBody = kj::none;
  return body;
}

kj::Promise<void> Subrequest::awaitResponse() {
  auto promise = kj::mv(KJ_REQUIRE_NONNULL(pending, "the response was already awaited"));
  pending = kj::none;
  response = co_await promise;
}

kj::HttpClient::Response& Subrequest::getResponse() {
  return KJ_REQUIRE_NONNULL(response, "the response has not arrived");
}

const kj::HttpClient::Response& Subrequest::getResponse() const {
  return KJ_REQUIRE_NONNULL(response, "the response has not arrived");
}

kj::Maybe<uint64_t> Subrequest::tryGetBodyLength() {
  KJ_IF_SOME(r, response) {
    return r.body->tryGetLength();
  }
  return kj::none;
}

kj::Own<Subrequest> subrequest_start(kj::Own<WorkerInterface> worker,
    kj::HttpMethod method,
    ::rust::Str url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize) {
  return kj::heap<Subrequest>(kj::mv(worker), method, kj::str(url), headers, expectedBodySize);
}

::rust::String subrequest_status_text(const Subrequest& call) {
  auto text = call.getResponse().statusText;
  return ::rust::String(text.begin(), text.size());
}

}  // namespace workerd::rust::send_worker
