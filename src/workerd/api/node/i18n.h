// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <workerd/api/node/encoding.h>
#include <workerd/jsg/jsg.h>

#include <kj/common.h>
#include <kj/string.h>

namespace workerd::api::node {

namespace i18n {

// Whether `transcode` supports `encoding`, as either source or destination. Used by
// BufferUtil::transcode.
bool canBeTranscoded(Encoding encoding);

jsg::JsUint8Array transcode(
    jsg::Lock& js, kj::ArrayPtr<kj::byte> source, Encoding fromEncoding, Encoding toEncoding);

}  // namespace i18n

}  // namespace workerd::api::node
