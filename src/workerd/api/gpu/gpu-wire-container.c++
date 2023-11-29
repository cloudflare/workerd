// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-wire-container.h"

namespace workerd::api::gpu {

DawnWireContainer::DawnWireContainer() {
  // serializer is configured here, optional memory transfer service
  // is not configured at this time
  auto& io = IoContext::current();
  stream_ = io.getIoChannelFactory().getGPUConnection();
  serializer_ = kj::heap<voodoo::DawnRemoteSerializer>(io.getWaitUntilTasks(), stream_);

  // spawn task to handle incoming commands on stream
  io.addTask(serializer_->handleIncomingCommands());

  // create dawn wire client
  dawn::wire::WireClientDescriptor clientDesc = {};
  clientDesc.serializer = serializer_;
  wireClient_ = kj::heap<dawn::wire::WireClient>(clientDesc);

  serializer_->onDawnBuffer = [&](const char* data, size_t len) {
    KJ_ASSERT(data != nullptr);
    if (wireClient_->HandleCommands(data, len) == nullptr) {
      KJ_LOG(ERROR, "onDawnBuffer: wireClient_->HandleCommands failed");
    }
    if (!serializer_->Flush()) {
      KJ_LOG(ERROR, "serializer->Flush() failed");
    }
  };

  instanceReservation_ = wireClient_->ReserveInstance();
}

wgpu::Instance DawnWireContainer::getInstance() {
  return wgpu::Instance::Acquire(instanceReservation_.instance);
}

void DawnWireContainer::Flush() {
  serializer_->Flush();
}

} // namespace workerd::api::gpu
