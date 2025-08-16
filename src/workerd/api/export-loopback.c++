// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "export-loopback.h"

#include <workerd/io/frankenvalue.h>

namespace workerd::api {

namespace {

class LoopbackServiceStubOutgoingFactory final: public Fetcher::CrossContextOutgoingFactory {
 public:
  LoopbackServiceStubOutgoingFactory(uint channelNumber, Frankenvalue props)
      : channelNumber(channelNumber),
        props(kj::mv(props)) {}

  kj::Own<WorkerInterface> newSingleUseClient(
      IoContext& context, kj::Maybe<kj::String> cfStr) override {
    auto channel = context.getIoChannelFactory().getSubrequestChannel(channelNumber, props.clone());

    auto workerInterface = context.getSubrequest(
        [&](TraceContext& tracing, IoChannelFactory& ioChannelFactory) {
      return channel->startRequest({.cfBlobJson = kj::mv(cfStr), .tracing = tracing});
    },
        {.inHouse = true,
          .wrapMetrics = true,
          .operationName = kj::ConstString("loopback_export_with_props"_kjc)});

    return context.getMetrics().wrapSubrequestClient(workerInterface.attach(kj::mv(channel)));
  }

 private:
  uint channelNumber;
  Frankenvalue props;
};

}  // namespace

jsg::Ref<Fetcher> LoopbackServiceStub::call(jsg::Lock& js, Options options) {
  Frankenvalue props;
  KJ_IF_SOME(p, options.props) {
    props = Frankenvalue::fromJs(js, p.getHandle(js));
  }

  return jsg::alloc<Fetcher>(kj::heap<LoopbackServiceStubOutgoingFactory>(channel, kj::mv(props)),
      RequiresHostAndProtocol::YES, /*isInHouse=*/true);
}

jsg::Ref<DurableObjectClass> LoopbackDurableObjectClass::call(jsg::Lock& js, Options options) {
  Frankenvalue props;
  KJ_IF_SOME(p, options.props) {
    props = Frankenvalue::fromJs(js, p.getHandle(js));
  }

  return jsg::alloc<DurableObjectClass>(channel, kj::mv(props));
}

}  // namespace workerd::api
