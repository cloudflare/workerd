// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/jsg/jsg.h>

namespace workerd {

kj::Array<kj::byte> serializeV8Value(jsg::Lock& js, kj::StringPtr key, const jsg::JsValue& value);

jsg::JsValue deserializeV8Value(jsg::Lock& js, kj::StringPtr key, kj::ArrayPtr<const kj::byte> buf);

}  // namespace workerd
