// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu.h"
#include <dawn/dawn_proc.h>
#include <dawn/native/DawnNative.h>

namespace workerd::gpu {

void initialize() {
  // Dawn native initialization. Dawn proc allows us to point the webgpu methods
  // to different implementations such as native, wire, or our custom
  // implementation. For now we will use the native version but in the future we
  // can make use of the wire version if we separate the GPU process or a custom
  // version as a stub in tests.
  DawnProcTable backendProcs = dawn::native::GetProcs();
  dawnProcSetProcs(&backendProcs);
}

} // namespace workerd::gpu
