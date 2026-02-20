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
    KJ_TRY {
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
    }
    KJ_CATCH(exception) {
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
      KJ_TRY {
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
      }
      KJ_CATCH(exception) {
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
      KJ_TRY {
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
      }
      KJ_CATCH(exception) {
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

// ---- FallbackServiceClient implementation ----

FallbackServiceClient::FallbackServiceClient(kj::String address)
    : ownedAddress(kj::mv(address)),
      thread([this]() { threadMain(); }) {}

FallbackServiceClient::~FallbackServiceClient() noexcept(false) {
  // Signal the background thread to exit. The kj::Thread destructor
  // (which runs after this body) will join the thread.
  auto lock = state.lockExclusive();
  lock->shutdown = true;
}

ModuleOrRedirect FallbackServiceClient::tryResolve(Version version,
    ImportType type,
    kj::StringPtr specifier,
    kj::StringPtr rawSpecifier,
    kj::StringPtr referrer,
    const kj::HashMap<kj::StringPtr, kj::StringPtr>& attributes) {
  // Submit request to background thread.
  {
    auto lock = state.lockExclusive();
    KJ_REQUIRE(!lock->shutdown, "FallbackServiceClient has been shut down");
    lock->version = version;
    lock->type = type;
    lock->specifier = specifier;
    lock->rawSpecifier = rawSpecifier;
    lock->referrer = referrer;
    lock->attributes = &attributes;
    lock->hasRequest = true;
  }

  // Block until the background thread has processed our request.
  return state.when([](const SharedState& s) { return s.responseReady || s.shutdown; },
      [](SharedState& s) -> ModuleOrRedirect {
    if (!s.responseReady) {
      // Background thread shut down without producing a response.
      return kj::none;
    }
    auto result = kj::mv(s.response);
    s.responseReady = false;
    return result;
  });
}

void FallbackServiceClient::threadMain() {
  KJ_TRY {
    // Set up the async I/O context, DNS resolution, and HTTP client once.
    // These are reused for all subsequent requests.
    kj::AsyncIoContext io = kj::setupAsyncIo();
    kj::HttpHeaderTable::Builder builder;
    kj::HttpHeaderId kMethod = builder.add("x-resolve-method");
    auto headerTable = builder.build();

    auto addr = io.provider->getNetwork().parseAddress(ownedAddress, 80).wait(io.waitScope);
    auto client = kj::newHttpClient(io.provider->getTimer(), *headerTable, *addr, {});

    while (true) {
      // Wait for a request or shutdown signal.
      Version version;
      ImportType type;
      kj::StringPtr specifier;
      kj::StringPtr rawSpecifier;
      kj::StringPtr referrer;
      const kj::HashMap<kj::StringPtr, kj::StringPtr>* attributes;
      bool shouldExit = state.when([](const SharedState& s) { return s.hasRequest || s.shutdown; },
          [&](SharedState& s) -> bool {
        if (s.shutdown) return true;
        version = s.version;
        type = s.type;
        specifier = s.specifier;
        rawSpecifier = s.rawSpecifier;
        referrer = s.referrer;
        attributes = s.attributes;
        s.hasRequest = false;
        return false;
      });
      if (shouldExit) return;

      // Process the request using the shared HTTP client.
      ModuleOrRedirect result = kj::none;

      if (version == Version::V1) {
        // === V1: GET request with query parameters ===
        kj::Maybe<kj::String> jsonPayload;
        bool redirect = false;
        bool prefixed = false;
        kj::Url url;
        kj::StringPtr actualSpecifier = nullptr;

        KJ_IF_SOME(pos, specifier.findLast('/')) {
          auto segment = specifier.slice(pos + 1);
          if (segment.startsWith("node:") || segment.startsWith("cloudflare:") ||
              segment.startsWith("workerd:")) {
            actualSpecifier = segment;
            url.query.add(
                kj::Url::QueryParam{.name = kj::str("specifier"), .value = kj::str(segment)});
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

        // Retry once on disconnect (stale pooled connection).
        for (int attempt = 0; attempt < 2; attempt++) {
          KJ_TRY {
            kj::HttpHeaders headers(*headerTable);
            headers.setPtr(kMethod, getMethodFromType(type));
            headers.setPtr(kj::HttpHeaderId::HOST, "localhost"_kj);

            auto request = client->request(kj::HttpMethod::GET, spec, headers, kj::none);
            kj::HttpClient::Response resp = request.response.wait(io.waitScope);

            if (resp.statusCode == 301) {
              KJ_IF_SOME(loc, resp.headers->get(kj::HttpHeaderId::LOCATION)) {
                redirect = true;
                jsonPayload = kj::str(loc);
              } else {
                KJ_LOG(ERROR, "Fallback service returned a redirect with no location", spec);
              }
              // Drain the response body to allow HTTP/1.1 connection reuse.
              resp.body->readAllBytes().wait(io.waitScope);
            } else if (resp.statusCode != 200) {
              auto payload = resp.body->readAllText().wait(io.waitScope);
              KJ_LOG(ERROR, "Fallback service failed to fetch module", payload, spec);
            } else {
              jsonPayload = resp.body->readAllText().wait(io.waitScope);
            }
            break;  // Success, no retry needed.
          }
          KJ_CATCH(exception) {
            if (attempt == 0 && exception.getType() == kj::Exception::Type::DISCONNECTED) {
              // Stale pooled connection; retry with a fresh one.
              continue;
            }
            KJ_LOG(ERROR, "Fallback service failed to fetch module", exception, spec);
          }
        }

        result = handleReturnPayload(kj::mv(jsonPayload), redirect, actualSpecifier);

      } else {
        // === V2: POST request with JSON body ===
        capnp::JsonCodec json;
        capnp::MallocMessageBuilder moduleMessage;
        auto requestMsg = moduleMessage.initRoot<server::config::FallbackServiceRequest>();
        requestMsg.setType(getMethodFromType(type));
        requestMsg.setSpecifier(specifier);
        requestMsg.setReferrer(referrer);

        if (rawSpecifier != nullptr) {
          requestMsg.setRawSpecifier(rawSpecifier);
        }

        KJ_ASSERT(attributes != nullptr);
        if (attributes->size() > 0) {
          auto attrs = requestMsg.initAttributes(attributes->size());
          size_t n = 0;
          for (auto& attr: *attributes) {
            attrs[n].setName(attr.key);
            attrs[n].setValue(attr.value);
            n++;
          }
        }

        auto payload = json.encode(requestMsg);

        kj::Maybe<kj::String> jsonPayload;
        bool redirect = false;

        // Retry once on disconnect (stale pooled connection).
        for (int attempt = 0; attempt < 2; attempt++) {
          KJ_TRY {
            kj::HttpHeaders headers(*headerTable);
            headers.setPtr(kj::HttpHeaderId::HOST, "localhost");

            auto request = client->request(kj::HttpMethod::POST, "/", headers, payload.size());
            request.body->write(payload.asPtr().asBytes()).wait(io.waitScope);

            kj::HttpClient::Response resp = request.response.wait(io.waitScope);

            if (resp.statusCode == 301) {
              KJ_IF_SOME(loc, resp.headers->get(kj::HttpHeaderId::LOCATION)) {
                redirect = true;
                jsonPayload = kj::str(loc);
              } else {
                KJ_LOG(ERROR, "Fallback service returned a redirect with no location", specifier);
              }
              // Drain the response body to allow HTTP/1.1 connection reuse.
              resp.body->readAllBytes().wait(io.waitScope);
            } else if (resp.statusCode != 200) {
              auto body = resp.body->readAllText().wait(io.waitScope);
              KJ_LOG(ERROR, "Fallback service failed to fetch module", body, specifier);
            } else {
              jsonPayload = resp.body->readAllText().wait(io.waitScope);
            }
            break;  // Success, no retry needed.
          }
          KJ_CATCH(exception) {
            if (attempt == 0 && exception.getType() == kj::Exception::Type::DISCONNECTED) {
              // Stale pooled connection; retry with a fresh one.
              continue;
            }
            KJ_LOG(ERROR, "Fallback service failed to fetch module", exception);
          }
        }

        result = handleReturnPayload(kj::mv(jsonPayload), redirect, specifier);
      }

      // Deliver the result to the calling thread.
      {
        auto lock = state.lockExclusive();
        lock->response = kj::mv(result);
        lock->responseReady = true;
      }
    }
  }
  KJ_CATCH(exception) {
    KJ_LOG(ERROR, "Fallback service background thread failed", exception);
    // Signal any waiting caller and prevent future requests.
    auto lock = state.lockExclusive();
    lock->response = kj::none;
    lock->responseReady = true;
    lock->shutdown = true;
  }
}

}  // namespace workerd::fallback
