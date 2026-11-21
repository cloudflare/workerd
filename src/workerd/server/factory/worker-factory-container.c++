// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-factory-impl.h"

#include <kj/compat/http.h>

namespace workerd::server {

namespace {

// The answer of a Worker's `connect()` to a container's TCP egress, whose CONNECT from the
// sidecar is already accepted: an acceptance has nothing more to say.
class TcpEgressConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {}

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
    KJ_FAIL_REQUIRE("TCP egress worker rejected the connection: ", statusCode, " ", statusText);
  }
};

}  // namespace

kj::Own<EgressChannel> factory_decode_egress_token(
    const WorkerFactory& factory, ::rust::Slice<const uint8_t> token) {
  auto channel = factory.getImpl().channelTokenHandler.decodeSubrequestChannelToken(
      IoChannelFactory::ChannelTokenUsage::RPC,
      kj::ArrayPtr<const byte>(token.data(), token.size()));
  return kj::heap<EgressChannel>(kj::mv(channel));
}

kj::Own<WorkerInterface> egress_channel_start_request(const EgressChannel& channel) {
  return channel.channel->startRequest({});
}

kj::Promise<void> egress_channel_connect(const WorkerFactory& factory,
    const EgressChannel& channel,
    ::rust::Str host,
    kj::Own<kj::AsyncIoStream> tunnel) {
  auto worker = channel.channel->startRequest({});
  auto destAddr = kj::str(host);
  TcpEgressConnectResponse response;
  kj::HttpHeaders headers(*factory.getImpl().headerTable);
  co_await worker->connect(destAddr, headers, *tunnel, response, {});
}

}  // namespace workerd::server
