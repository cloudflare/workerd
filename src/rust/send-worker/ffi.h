// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ side of the send worker's bridge (ffi.rs). Included by the generated bridge header.

#include <workerd/io/worker-interface.h>
#include <workerd/rust/kj/ffi.h>

#include <rust/cxx.h>

#include <kj/async-io.h>
#include <kj/compat/http.h>

namespace workerd::rust::send_worker {

// One HTTP request on a WorkerInterface, in the shape of kj::HttpClient: the request body is a
// stream the caller writes, and the response is a head followed by a stream the caller reads.
// kj::newHttpClient(kj::HttpService&) does the adapting.
//
// The object owns the WorkerInterface and everything that refers to it; destroying it cancels
// the request. The request body it hands out is one end of a pipe and may outlive it.
class Subrequest {
 public:
  Subrequest(kj::Own<WorkerInterface> worker,
      kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize);
  KJ_DISALLOW_COPY_AND_MOVE(Subrequest);

  kj::Own<kj::AsyncOutputStream> takeRequestBody();

  // Resolves once the response head arrived; getResponse() requires that it has.
  kj::Promise<void> awaitResponse();
  kj::HttpClient::Response& getResponse();
  const kj::HttpClient::Response& getResponse() const;
  kj::Maybe<uint64_t> tryGetBodyLength();

 private:
  // Destroyed in reverse: the response body and the pending response, which run the request and
  // refer to the client, go first, the WorkerInterface the client refers to last.
  kj::Own<WorkerInterface> worker;
  kj::Own<kj::HttpClient> client;
  kj::Maybe<kj::Own<kj::AsyncOutputStream>> requestBody;
  kj::Maybe<kj::Promise<kj::HttpClient::Response>> pending;
  kj::Maybe<kj::HttpClient::Response> response;
};

kj::Own<Subrequest> subrequest_start(kj::Own<WorkerInterface> worker,
    kj::HttpMethod method,
    ::rust::Str url,
    const kj::HttpHeaders& headers,
    kj::Maybe<uint64_t> expectedBodySize);

inline kj::Own<kj::AsyncOutputStream> subrequest_take_request_body(Subrequest& call) {
  return call.takeRequestBody();
}

inline kj::Promise<void> subrequest_response(Subrequest& call) {
  return call.awaitResponse();
}

inline uint32_t subrequest_status(const Subrequest& call) {
  return call.getResponse().statusCode;
}

::rust::String subrequest_status_text(const Subrequest& call);

inline const kj::HttpHeaders& subrequest_headers(const Subrequest& call) {
  return *call.getResponse().headers;
}

inline kj::Maybe<uint64_t> subrequest_body_length(Subrequest& call) {
  return call.tryGetBodyLength();
}

inline kj::Promise<size_t> subrequest_read(
    Subrequest& call, ::rust::Slice<kj::byte> buffer, size_t minBytes) {
  return call.getResponse().body->tryRead(buffer.data(), minBytes, buffer.size());
}

}  // namespace workerd::rust::send_worker
