// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "requirements.h"

#include <cctype>

namespace workerd::api::pyodide {

// getField gets a field of a JSON object by key
capnp::json::Value::Reader getField(
    capnp::List<::capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &object,
    kj::StringPtr name) {
  for (const auto &ent: object) {
    if (ent.getName() == name) {
      return ent.getValue();
    }
  }

  KJ_FAIL_ASSERT("Expected key in JSON object", name);
}

kj::String canonicalizePythonPackageName(kj::StringPtr name) {
  kj::Vector<char> res(name.size());

  auto isSeparator = [](char c) { return c == '-' || c == '_' || c == '.'; };

  for (int i = 0; i < name.size(); i++) {
    if (isSeparator(name[i])) {
      res.add('-');
      // make i point to the last separator in the sequence
      while (isSeparator(name[i])) i++;
      i--;
      continue;
    }

    res.add(std::tolower(name[i]));
  }

  res.add(0);  // NUL terminator

  return kj::String(res.releaseAsArray());
}

// getDepMapFromPackagesLock computes a dependency map (a mapping from requirement to list of dependencies) from the Pyodide lock file JSON
DepMap getDepMapFromPackagesLock(
    capnp::List<capnp::json::Value::Field, capnp::Kind::STRUCT>::Reader &packages) {
  DepMap res;

  for (const auto &ent: packages) {
    auto packageObj = ent.getValue().getObject();
    auto depends = getField(packageObj, "depends").getArray();

    auto &[_, deps] = res.insert(kj::str(ent.getName()), kj::Vector<kj::String>(depends.size()));

    for (const auto &dep: depends) {
      deps.add(kj::str(dep.getString()));
    }
  }

  return res;
}

// addWithRecursiveDependencies adds a requirement along with all its dependencies (according to the dependency map) to the requirements set
void addWithRecursiveDependencies(
    kj::StringPtr requirement, const DepMap &depMap, kj::HashSet<kj::String> &requirementsSet) {
  auto normalizedName = canonicalizePythonPackageName(requirement);
  if (requirementsSet.contains(normalizedName)) {
    return;
  }

  requirementsSet.insert(kj::str(normalizedName));

  KJ_IF_SOME(deps, depMap.find(normalizedName)) {
    for (const auto &dep: deps) {
      addWithRecursiveDependencies(dep, depMap, requirementsSet);
    }
  }
}

kj::Own<capnp::List<capnp::json::Value::Field>::Reader> parseLockFile(
    kj::StringPtr lockFileContents) {
  capnp::JsonCodec json;
  capnp::MallocMessageBuilder message;

  auto lock = message.initRoot<capnp::JsonValue>();
  json.decodeRaw(lockFileContents, lock);

  auto object = lock.getObject().asReader();
  auto packages = getField(object, "packages").getObject();
  return capnp::clone(packages);
}

kj::HashSet<kj::String> getPythonPackageNames(
    capnp::List<capnp::json::Value::Field>::Reader packages,
    const DepMap &depMap,
    kj::ArrayPtr<kj::String> requirements,
    kj::StringPtr packagesVersion) {

  kj::HashSet<kj::String> allRequirements;  // Requirements including their recursive dependencies.

  // Potentially add the stdlib packages and their recursive dependencies.
  // TODO: Loading stdlib and its dependencies breaks package snapshots on "20240829.4".
  // Remove this version check once a new package/python release is made.
  if (packagesVersion != "20240829.4") {
    // We need to scan the packages list for any packages that need to be included because they
    // are part of Python's stdlib (hashlib etc). These need to be implicitly treated as part of
    // our `requirements`.
    for (const auto &ent: packages) {
      auto name = ent.getName();
      auto obj = ent.getValue().getObject();
      auto packageType = getField(obj, "package_type").getString();

      if (packageType == "cpython_module"_kj) {
        addWithRecursiveDependencies(name, depMap, allRequirements);
      }
    }
  }

  // Add all recursive dependencies of each requirement.
  for (const auto &req: requirements) {
    addWithRecursiveDependencies(req, depMap, allRequirements);
  }

  return allRequirements;
}

}  // namespace workerd::api::pyodide
