// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#pragma once

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/common.h>
#include <kj/map.h>

namespace workerd::api::pyodide {

capnp::json::Value::Reader getField(
    capnp::List<::capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &object,
    kj::StringPtr name);

kj::String canonicalizePythonPackageName(kj::StringPtr name);

// map from requirement to list of dependencies
typedef kj::HashMap<kj::String, kj::Vector<kj::String>> DepMap;

DepMap getDepMapFromPackagesLock(
    capnp::List<capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &packages);

kj::Own<capnp::List<capnp::json::Value::Field>::Reader> parseLockFile(
    kj::StringPtr lockFileContents);

kj::HashSet<kj::String> getPythonPackageNames(
    capnp::List<capnp::json::Value::Field>::Reader packages,
    const DepMap &depMap,
    kj::ArrayPtr<kj::String> requirements,
    kj::StringPtr packagesVersion);

}  // namespace workerd::api::pyodide
