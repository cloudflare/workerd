// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// This server interacts directly with the GPU, and listens on a UNIX socket for clients
// of the Dawn Wire protocol.

#ifdef _WIN32
#include <io.h>
#define unlink _unlink
#else
#include <unistd.h>
#endif

#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>
#include <dawn/webgpu_cpp.h>
#include <dawn/wire/WireServer.h>
#include <filesystem>
#include <kj/async-io.h>
#include <kj/debug.h>
#include <thread>
#include <workerd/api/gpu/voodoo/voodoo-pipe.h>
#include <workerd/api/gpu/voodoo/voodoo-protocol.h>
#include <workerd/api/gpu/voodoo/voodoo-server.h>

namespace workerd::api::gpu::voodoo {

void VoodooServer::startServer() {
  KJ_LOG(INFO, listenPath, "will start listening server");

  // initialize dawn
  dawnProcSetProcs(&nativeProcs);
  auto adapters = instance.EnumerateAdapters();
  KJ_REQUIRE(!adapters.empty(), "no GPU adapters found");

  // initialize event loop
  kj::AsyncIoContext io = kj::setupAsyncIo();

  // create listening socket
  unlink(listenPath.cStr());
  auto addr =
      io.provider->getNetwork().parseAddress(kj::str("unix:", listenPath)).wait(io.waitScope);

  auto listener = addr->listen();

  // process requests
  auto promise = acceptLoop(kj::mv(listener));
  promise.wait(io.waitScope);
  return;
}

kj::Promise<void> VoodooServer::acceptLoop(kj::Own<kj::ConnectionReceiver>&& listener) {
  kj::TaskSet tasks(*this);

  for (;;) {
    auto connection = co_await listener->accept();
    tasks.add(handleConnection(kj::mv(connection)));
  }
}

kj::Promise<void> VoodooServer::flushAfterEvents(DawnRemoteSerializer& serializer) {
  WGPUInstance wgpuInstance = instance.Get();

  while (dawn::native::InstanceProcessEvents(wgpuInstance)) {
    // TODO(soon): how to sleep here without blocking the thread?
    // I don't have an IoContext to use setTimeout.
    // This is currently a busy-loop. :(
  }
  if (!serializer.Flush()) {
    KJ_LOG(ERROR, "serializer->Flush() failed");
  }
  co_return;
}

kj::Promise<void> VoodooServer::handleConnection(kj::Own<kj::AsyncIoStream> stream) {
  KJ_LOG(INFO, "handling connection");

  // setup wire
  DawnRemoteErrorHandler dawnErrorHandler(stream);
  kj::TaskSet tasks(dawnErrorHandler);
  auto serializer = kj::heap<DawnRemoteSerializer>(tasks, stream);
  dawn::wire::WireServerDescriptor wDesc{
      .procs = &nativeProcs,
      .serializer = serializer,
  };

  auto wireServer = kj::heap<dawn::wire::WireServer>(wDesc);
  wireServer->InjectInstance(instance.Get(), {1, 0});

  serializer->onDawnBuffer = [&](const char* data, size_t len) {
    KJ_ASSERT(data != nullptr);
    if (wireServer->HandleCommands(data, len) == nullptr) {
      KJ_LOG(ERROR, "onDawnBuffer: wireServer->HandleCommands failed");
    }
    tasks.add(flushAfterEvents(*serializer));
  };

  // process commands
  co_await serializer->handleIncomingCommands();

  KJ_LOG(INFO, "connection is done");
  co_return;
}

} // namespace workerd::api::gpu::voodoo
