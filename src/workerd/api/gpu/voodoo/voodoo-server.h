// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This server interacts directly with the GPU, and listens on a UNIX socket for clients
// of the Dawn Wire protocol.

#pragma once

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>
#include <dawn/wire/WireServer.h>
#include <filesystem>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <kj/exception.h>
#include <workerd/api/gpu/voodoo/voodoo-pipe.h>
#include <workerd/api/gpu/voodoo/voodoo-protocol.h>

namespace workerd::api::gpu::voodoo {

class VoodooServer : public kj::TaskSet::ErrorHandler {
public:
  VoodooServer(kj::StringPtr path) : listenPath(path), nativeProcs(dawn::native::GetProcs()) {}

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, "task failed handling connection", exception);
  }

  void startServer();

private:
  kj::Promise<void> acceptLoop(kj::Own<kj::ConnectionReceiver>&& listener);
  kj::Promise<void> handleConnection(kj::Own<kj::AsyncIoStream> stream);
  kj::Promise<void> flushAfterEvents(DawnRemoteSerializer& serializer);

  kj::StringPtr listenPath;
  DawnProcTable nativeProcs;
  dawn::native::Instance instance;
};

} // namespace workerd::api::gpu::voodoo
