// Copyright (c) 2017-2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "export-loopback.h"

#include <workerd/io/frankenvalue.h>

namespace workerd::api {

jsg::Ref<Fetcher> LoopbackServiceStub::callImpl(jsg::Lock& js,
    jsg::Optional<jsg::JsRef<jsg::JsObject>> propsMaybe,
    jsg::Optional<OptionsWithVersion::Version> versionMaybe) {
  Frankenvalue props;
  KJ_IF_SOME(p, propsMaybe) {
    props = Frankenvalue::fromJs(js, p.getHandle(js));
  }
  kj::Maybe<IoChannelFactory::VersionRequest> versionRequest;
  KJ_IF_SOME(version, kj::mv(versionMaybe)) {
    versionRequest = IoChannelFactory::VersionRequest{
      .cohort = kj::mv(version.cohort).orDefault(kj::none),
    };
  }

  IoContext& ioctx = IoContext::current();
  auto channelObj = ioctx.getIoChannelFactory().getSubrequestChannel(
      channel, kj::mv(props), kj::mv(versionRequest));
  return js.alloc<Fetcher>(ioctx.addObject(kj::mv(channelObj)));
}

jsg::Ref<DurableObjectClass> LoopbackDurableObjectClass::call(jsg::Lock& js, Options options) {
  Frankenvalue props;
  KJ_IF_SOME(p, options.props) {
    props = Frankenvalue::fromJs(js, p.getHandle(js));
  }

  IoContext& ioctx = IoContext::current();
  auto channelObj = ioctx.getIoChannelFactory().getActorClass(channel, kj::mv(props));
  return js.alloc<DurableObjectClass>(ioctx.addObject(kj::mv(channelObj)));
}

}  // namespace workerd::api
