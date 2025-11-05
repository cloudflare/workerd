#pragma once

#include <workerd/server/workerd.capnp.h>

#include <kj/common.h>
#include <kj/map.h>
#include <kj/one-of.h>
#include <kj/string.h>

namespace workerd::fallback {

// The fallback service is a mechanism used only in workerd local development.
// It is used to use an external http service to resolve module specifiers
// dynamically if the module is not found in the static bundles. A worker
// must be configured to use the fallback service and workerd must be started
// with the --experimental CLI flag.
//
// There are two versions of the fallback service protocol:
//
// V1: The request is sent to the fallback service as a GET request using
// query strings to pass the details. The specifier and referrer are treated
// as strings. Import attributes are not included.
//
// V2: The request is sent to the fallback service as a POST request using
// JSON to pass the details. The specifier and referrer are treated as URLs.
// Import attributes are included.
//
// The fallback service may return either a JSON string describing the module
// configuration, a 301 redirect to a different module specifier, or an error.
enum class ImportType {
  // The import is a static or dynamic import
  IMPORT,
  // The import is a CommonJs-style require()
  REQUIRE,
  // The import originated from inside the runtime
  INTERNAL,
};

enum class Version {
  // With V1 of the fallback service, the request is sent to the fallback
  // service as a GET request using query strings to pass the details.
  // The specifier and referrer are treated as strings. Import attributes
  // are not included.
  V1,
  // With V2 of the fallback service, the request is sent to the fallback
  // service as a POST request using JSON to pass the details. The specifier
  // and referrer are treated as URLs. Import attributes are included.
  V2,
};

using ModuleOrRedirect =
    kj::Maybe<kj::OneOf<kj::String, kj::Own<server::config::Worker::Module::Reader>>>;

// Tries to resolve the module using the fallback service.
// If a string is returned, the fallback service redirected us to resolve a
// different module whose specifier is given by the returned string. Otherwise,
// the fallback service returns a module configuration object.
ModuleOrRedirect tryResolve(Version version,
    ImportType type,
    kj::StringPtr address,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes);

}  // namespace workerd::fallback
