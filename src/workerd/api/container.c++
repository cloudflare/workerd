// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/io/io-context.h>

namespace workerd::api {

Container::Container(rpc::Container::Client rpcClient)
    : rpcClient(IoContext::current().addObject(kj::heap(kj::mv(rpcClient)))) {}

}  // namespace workerd::api
