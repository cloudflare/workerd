// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <rust/cxx.h>

#include <cstddef>

// The C++ side of the `autogate` crate's bridge (ffi.rs). A gate is named by its index in
// util::getAutogateKeys().
namespace workerd::autogate_bridge {

// The number of autogates.
size_t autogate_count();

// Whether the gate `index` is enabled: util::Autogate::isEnabled().
bool autogate_is_enabled(size_t index);

// The name of the gate `index`, in kebab-case without the `workerd-autogate-` prefix, as
// KJ_STRINGIFY(AutogateKey) gives it. The string is static.
::rust::Str autogate_name(size_t index);

}  // namespace workerd::autogate_bridge
