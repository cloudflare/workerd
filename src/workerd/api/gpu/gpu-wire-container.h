// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include "gpu-container.h"
#include <dawn/wire/WireClient.h>
#include <workerd/api/gpu/voodoo/voodoo-protocol.h>

namespace workerd::api::gpu {

class DawnWireContainer : public DawnContainer {
public:
  wgpu::Instance getInstance() override;
  void Flush() override;
  DawnWireContainer();
  ~DawnWireContainer() noexcept override {}

private:
  kj::Own<kj::AsyncIoStream> stream_;
  kj::Own<voodoo::DawnRemoteSerializer> serializer_;
  kj::Own<dawn::wire::WireClient> wireClient_;
  dawn::wire::ReservedInstance instanceReservation_;
};

} // namespace workerd::api::gpu
