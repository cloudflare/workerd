// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <capnp/compat/json.capnp.h>
#include <capnp/list.h>
#include <kj/common.h>

namespace workerd::api::pyodide {

capnp::json::Value::Reader getField(
    capnp::List<::capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &object,
    kj::StringPtr name);

kj::Own<capnp::List<capnp::json::Value::Field>::Reader> parseLockFile(
    kj::StringPtr lockFileContents);

}  // namespace workerd::api::pyodide
