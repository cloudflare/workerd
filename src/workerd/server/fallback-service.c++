#include "fallback-service.h"

#include <capnp/compat/json.h>
#include <capnp/message.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/compat/url.h>
#include <kj/debug.h>
#include <kj/one-of.h>
#include <kj/string.h>
#include <kj/thread.h>

namespace workerd::fallback {
namespace {

constexpr kj::StringPtr getMethodFromType(ImportType type) {
  switch (type) {
    case ImportType::IMPORT:
      return "import"_kjc;
    case ImportType::REQUIRE:
      return "require"_kjc;
    case ImportType::INTERNAL:
      return "internal"_kjc;
  }
  KJ_UNREACHABLE;
}

ModuleOrRedirect handleReturnPayload(
    kj::Maybe<kj::String> jsonPayload, bool redirect, kj::StringPtr specifier) {
  KJ_IF_SOME(payload, jsonPayload) {
    // If the payload is empty then the fallback service failed to fetch the module.
    if (payload.size() == 0) return kj::none;

    // If redirect is true then the fallback service returned a 301 redirect. The
    // payload is the specifier of the new target module.
    if (redirect) {
      return kj::Maybe(kj::mv(payload));
    }

    // The response from the fallback service must be a valid JSON serialization
    // of the workerd module configuration. If it is not, or if there is any other
    // error when processing here, we'll log the exception and return nothing.
    try {
      capnp::MallocMessageBuilder moduleMessage;
      capnp::JsonCodec json;
      json.handleByAnnotation<server::config::Worker::Module>();
      auto moduleBuilder = moduleMessage.initRoot<server::config::Worker::Module>();
      json.decode(payload, moduleBuilder);

      // If the module fallback service returns a name in the module then it has to
      // match the specifier we passed in. This is an optional sanity check.
      if (moduleBuilder.hasName()) {
        if (moduleBuilder.getName() != specifier) {
          KJ_LOG(ERROR,
              "Fallback service failed to fetch module: returned module "
              "name does not match specifier",
              moduleBuilder.getName(), specifier);
          return kj::none;
        }
      } else {
        moduleBuilder.setName(kj::str(specifier));
      }

      kj::Own<server::config::Worker::Module::Reader> ret = capnp::clone(moduleBuilder.asReader());
      return ModuleOrRedirect(kj::mv(ret));
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      KJ_LOG(ERROR, "Fallback service failed to fetch module", exception, specifier);
      return kj::none;
    }
  }

  // If we got here, no jsonPayload was received and we return nothing.
  return kj::none;
}

// The original implementation of the fallback service uses a GET request to
// submit the request to the service.
ModuleOrRedirect tryResolveV1(ImportType type,
    kj::StringPtr address,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer) {
  kj::Maybe<kj::String> jsonPayload;
  bool redirect = false;
  bool prefixed = false;
  kj::Url url;
  kj::StringPtr actualSpecifier = nullptr;

  // TODO(cleanup): This is a bit of a hack based on the current
  // design of the module registry loader algorithms handling of
  // prefixed modules. This will be simplified with the upcoming
  // module registry refactor.
  KJ_IF_SOME(pos, specifier.findLast('/')) {
    auto segment = specifier.slice(pos + 1);
    if (segment.startsWith("node:") || segment.startsWith("cloudflare:") ||
        segment.startsWith("workerd:")) {
      actualSpecifier = segment;
      url.query.add(kj::Url::QueryParam{.name = kj::str("specifier"), .value = kj::str(segment)});
      prefixed = true;
    }
  }
  if (!prefixed) {
    actualSpecifier = specifier;
    if (actualSpecifier.startsWith("/")) {
      actualSpecifier = specifier.slice(1);
    }
    url.query.add(kj::Url::QueryParam{kj::str("specifier"), kj::str(specifier)});
  }
  url.query.add(kj::Url::QueryParam{kj::str("referrer"), kj::str(referrer)});
  url.query.add(kj::Url::QueryParam{kj::str("rawSpecifier"), kj::str(rawSpecifier)});

  auto spec = url.toString(kj::Url::HTTP_REQUEST);

  {
    // Module loading in workerd is expected to be synchronous but we need to perform
    // an async HTTP request to the fallback service. To accomplish that we wrap the
    // actual request in a kj::Thread, perform the GET, and drop the thread immediately
    // so that the destructor joins the current thread (blocking it). The thread will
    // either set the jsonPayload variable or not.
    kj::Thread loaderThread([&spec, address, &jsonPayload, &redirect, type]() mutable {
      try {
        kj::AsyncIoContext io = kj::setupAsyncIo();
        kj::HttpHeaderTable::Builder builder;
        kj::HttpHeaderId kMethod = builder.add("x-resolve-method");
        auto headerTable = builder.build();

        auto addr = io.provider->getNetwork().parseAddress(address, 80).wait(io.waitScope);
        auto client = kj::newHttpClient(io.provider->getTimer(), *headerTable, *addr, {});

        kj::HttpHeaders headers(*headerTable);
        headers.setPtr(kMethod, getMethodFromType(type));
        headers.setPtr(kj::HttpHeaderId::HOST, "localhost"_kj);

        auto request = client->request(kj::HttpMethod::GET, spec, headers, kj::none);

        kj::HttpClient::Response resp = request.response.wait(io.waitScope);

        if (resp.statusCode == 301) {
          // The fallback service responded with a redirect.
          KJ_IF_SOME(loc, resp.headers->get(kj::HttpHeaderId::LOCATION)) {
            redirect = true;
            jsonPayload = kj::str(loc);
          } else {
            KJ_LOG(ERROR, "Fallback service returned a redirect with no location", spec);
          }
        } else if (resp.statusCode != 200) {
          // Failed! Log the body of the response, if any, and fall through without
          // setting jsonPayload to signal that the fallback service failed to return
          // a module for this specifier.
          auto payload = resp.body->readAllText().wait(io.waitScope);
          KJ_LOG(ERROR, "Fallback service failed to fetch module", payload, spec);
        } else {
          jsonPayload = resp.body->readAllText().wait(io.waitScope);
        }
      } catch (...) {
        auto exception = kj::getCaughtExceptionAsKj();
        KJ_LOG(ERROR, "Fallback service failed to fetch module", exception, spec);
      }
    });
  }

  return handleReturnPayload(kj::mv(jsonPayload), redirect, actualSpecifier);
}

ModuleOrRedirect tryResolveV2(ImportType type,
    kj::StringPtr address,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes) {

  capnp::JsonCodec json;
  capnp::MallocMessageBuilder moduleMessage;
  auto request = moduleMessage.initRoot<server::config::FallbackServiceRequest>();
  request.setType(getMethodFromType(type));
  request.setSpecifier(specifier);
  request.setReferrer(referrer);

  if (rawSpecifier != nullptr) {
    request.setRawSpecifier(rawSpecifier);
  }

  if (attributes.size() > 0) {
    auto attrs = request.initAttributes(attributes.size());
    size_t n = 0;
    for (auto& attr: attributes) {
      attrs[n].setName(attr.key);
      attrs[n].setValue(attr.value);
      n++;
    }
  }

  auto payload = json.encode(request);

  kj::Maybe<kj::String> jsonPayload;
  bool redirect = false;

  {
    kj::Thread loaderThread(
        [&address, &jsonPayload, payload = kj::mv(payload), &redirect, &specifier]() mutable {
      try {
        kj::AsyncIoContext io = kj::setupAsyncIo();
        kj::HttpHeaderTable::Builder builder;
        auto headerTable = builder.build();

        auto addr = io.provider->getNetwork().parseAddress(address, 80).wait(io.waitScope);
        auto client = kj::newHttpClient(io.provider->getTimer(), *headerTable, *addr, {});

        kj::HttpHeaders headers(*headerTable);
        headers.setPtr(kj::HttpHeaderId::HOST, "localhost");

        auto request = client->request(kj::HttpMethod::POST, "/", headers, payload.size());
        {
          // Write then drop the payload.
          kj::mv(request.body)->write(payload.asPtr().asBytes()).wait(io.waitScope);
        }

        kj::HttpClient::Response resp = request.response.wait(io.waitScope);

        if (resp.statusCode == 301) {
          // The fallback service responded with a redirect.
          KJ_IF_SOME(loc, resp.headers->get(kj::HttpHeaderId::LOCATION)) {
            redirect = true;
            jsonPayload = kj::str(loc);
          } else {
            KJ_LOG(ERROR, "Fallback service returned a redirect with no location", specifier);
          }
        } else if (resp.statusCode != 200) {
          // Failed! Log the body of the response, if any, and fall through without
          // setting jsonPayload to signal that the fallback service failed to return
          // a module for this specifier.
          auto payload = resp.body->readAllText().wait(io.waitScope);
          KJ_LOG(ERROR, "Fallback service failed to fetch module", payload, specifier);
        } else {
          jsonPayload = resp.body->readAllText().wait(io.waitScope);
        }
      } catch (...) {
        auto exception = kj::getCaughtExceptionAsKj();
        KJ_LOG(ERROR, "Fallback service failed to fetch module", exception);
      }
    });
  }

  return handleReturnPayload(kj::mv(jsonPayload), redirect, specifier);
}
}  // namespace

ModuleOrRedirect tryResolve(Version version,
    ImportType type,
    kj::StringPtr address,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes) {
  return version == Version::V1
      ? tryResolveV1(type, address, specifier, rawSpecifier, referrer)
      : tryResolveV2(type, address, specifier, rawSpecifier, referrer, attributes);
}

}  // namespace workerd::fallback
