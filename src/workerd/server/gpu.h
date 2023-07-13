// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// Very experimental initial webgpu support based on the Dawn library. Currently
// we just link against Dawn and initialize it at startup pointing it at a Dawn
// native instance.
namespace workerd::gpu {
void initialize();
};
