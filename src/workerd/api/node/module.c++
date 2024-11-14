// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "module.h"

#include <workerd/io/features.h>
#include <workerd/jsg/url.h>

namespace workerd::api::node {

bool ModuleUtil::isBuiltin(kj::String specifier) {
  return jsg::checkNodeSpecifier(specifier) != kj::none;
}

jsg::JsValue ModuleUtil::createRequire(jsg::Lock& js, kj::String path) {
  // Node.js requires that the specifier path is a File URL or an absolute
  // file path string. To be compliant, we will convert whatever specifier
  // is into a File URL if possible, then take the path as the actual
  // specifier to use.
  auto parsed = JSG_REQUIRE_NONNULL(jsg::Url::tryParse(path.asPtr(), "file:///"_kj), TypeError,
      "The argument must be a file URL object, "
      "a file URL string, or an absolute path string.");

  // We do not currently handle specifiers as URLs, so let's treat any
  // input that has query string params or hash fragments as errors.
  if (parsed.getSearch().size() > 0 || parsed.getHash().size() > 0) {
    JSG_FAIL_REQUIRE(
        Error, "The specifier must not have query string parameters or hash fragments.");
  }

  // The specifier must be a file: URL
  JSG_REQUIRE(parsed.getProtocol() == "file:"_kj, TypeError, "The specifier must be a file: URL.");

  return jsg::JsValue(js.wrapReturningFunction(js.v8Context(),
      [referrer = kj::str(parsed.getPathname())](
          jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>& args) -> v8::Local<v8::Value> {
    auto registry = jsg::ModuleRegistry::from(js);

    // TODO(soon): This will need to be updated to support the new module registry
    // when that is fully implemented.
    JSG_REQUIRE(registry != nullptr, Error, "Module registry not available.");

    auto ref = ([&] {
      try {
        return kj::Path::parse(referrer.slice(1));
      } catch (kj::Exception& e) {
        JSG_FAIL_REQUIRE(Error, kj::str("Invalid referrer path: ", referrer.slice(1)));
      }
    })();

    auto spec = kj::str(args[0]);

    if (jsg::isNodeJsCompatEnabled(js)) {
      KJ_IF_SOME(nodeSpec, jsg::checkNodeSpecifier(spec)) {
        spec = kj::mv(nodeSpec);
      }
    }

    static const kj::Path kRoot = kj::Path::parse("");

    kj::Path targetPath = ([&] {
      // If the specifier begins with one of our known prefixes, let's not resolve
      // it against the referrer.
      try {
        if (spec.startsWith("node:") || spec.startsWith("cloudflare:") ||
            spec.startsWith("workerd:")) {
          return kj::Path::parse(spec);
        }

        return ref == kRoot ? kj::Path::parse(spec) : ref.parent().eval(spec);
      } catch (kj::Exception&) {
        JSG_FAIL_REQUIRE(Error, kj::str("Invalid specifier path: ", spec));
      }
    })();

    // require() is only exposed to worker bundle modules so the resolve here is only
    // permitted to require worker bundle or built-in modules. Internal modules are
    // excluded.
    auto& info = JSG_REQUIRE_NONNULL(
        registry->resolve(js, targetPath, ref, jsg::ModuleRegistry::ResolveOption::DEFAULT,
            jsg::ModuleRegistry::ResolveMethod::REQUIRE, spec.asPtr()),
        Error, "No such module \"", targetPath.toString(), "\".");

    jsg::ModuleRegistry::RequireImplOptions options =
        jsg::ModuleRegistry::RequireImplOptions::DEFAULT;
    if (info.maybeSynthetic != kj::none) {
      options = jsg::ModuleRegistry::RequireImplOptions::EXPORT_DEFAULT;
    }

    return jsg::ModuleRegistry::requireImpl(js, info, options);
  }));
}

}  // namespace workerd::api::node
