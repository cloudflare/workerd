#pragma once

#include <kj/async.h>
#include <kj/common.h>

#include <workerd/api/basics.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {

class WebWorker: public EventTarget {

public:
  WebWorker(kj::Promise<uint>& subrequestChannel)
    : subrequestChannel(subrequestChannel) {}

  struct Options {
    jsg::Optional<kj::String> type;
    jsg::Optional<kj::String> credentials;
    jsg::Optional<kj::String> name;

    JSG_STRUCT(type, credentials, name);
    JSG_STRUCT_TS_OVERRIDE(Options {
      type: 'classic' | 'module';
      credentials: 'omit' | 'same-origin' | 'include';
      name: string;
    });
  };

  static jsg::Ref<WebWorker> constructor(kj::String aUrl, jsg::Optional<Options> options);

  kj::Promise<void> postMessage(kj::String message);

  JSG_RESOURCE_TYPE(WebWorker) {
    JSG_METHOD(postMessage);
  }

private:
  kj::Promise<uint>& subrequestChannel;
};

#define EW_WEB_WORKER_ISOLATE_TYPES \
  api::WebWorker,                   \
  api::WebWorker::Options

}  // namespace workerd::api
