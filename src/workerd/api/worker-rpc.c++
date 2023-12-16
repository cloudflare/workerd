// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/worker-rpc.h>

namespace workerd::api {

kj::Maybe<jsg::JsValue> WorkerRpc::getNamed(jsg::Lock& js, kj::StringPtr name) {
    // TODO(soon): The server side PR will fill this function with the RPC call and other stuff,
    //             this is just a temporary placeholder for testing.
    return jsg::JsValue(js.wrapReturningFunction(js.v8Context(),
        [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) {
          return js.error("WorkerRpc is unimplemented");
        }
    ));
  }
}; // namespace workerd::api
