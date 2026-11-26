// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "autogate.h"

#include <workerd/util/sentry.h>
#include <workerd/util/zlib-router.h>

#include <stdlib.h>

#include <capnp/message.h>
#include <kj/common.h>
#include <kj/debug.h>
#include <kj/vector.h>

namespace workerd::util {

kj::Maybe<Autogate> globalAutogate;

namespace {
constexpr auto WORKERD_PREFIX = "workerd-autogate-"_kj;

// Propagates the COMPRESSION_RS gate to the zlib routing layer, which cannot depend on autogate
// itself. Called whenever the global gate state changes; also run at static init so that
// processes that never call initAutogate() (e.g. kj_test binaries under WORKERD_ALL_AUTOGATES)
// still route accordingly.
void syncZlibRouter() {
  workerd_zlib_router_set_rs(Autogate::isEnabled(AutogateKey::COMPRESSION_RS));
}
[[maybe_unused]] const bool zlibRouterInitialSync = (syncZlibRouter(), true);

// Converts a SCREAMING_SNAKE_CASE string to kebab-case at compile time.
template <size_t N>
struct ScreamingSnakeToKebab {
  char value[N]{};

  consteval ScreamingSnakeToKebab(const char (&s)[N]) {
    for (size_t i = 0; i < N; ++i) {
      if (s[i] == '_') {
        value[i] = '-';
      } else if (s[i] >= 'A' && s[i] <= 'Z') {
        value[i] = static_cast<char>(s[i] + ('a' - 'A'));
      } else {
        value[i] = s[i];
      }
    }
  }

  template <size_t M>
  consteval bool operator==(const char (&other)[M]) const {
    if constexpr (N != M) return false;
    for (size_t i = 0; i < N; ++i) {
      if (value[i] != other[i]) return false;
    }
    return true;
  }
};
// Quick compile-time test to ensure the conversion works as expected.
static_assert(ScreamingSnakeToKebab("TEST_WORKERD") == "test-workerd");
}  // namespace

namespace {

kj::Maybe<AutogateKey> tryParseAutogateName(kj::ArrayPtr<const char> name) {
  for (AutogateKey key: getAutogateKeys()) {
    if (kj::toCharSequence(key).asArray() == name) {
      return key;
    }
  }
  return kj::none;
}

// The gates that WORKERD_ALL_AUTOGATES_EXCEPT names, as a comma-separated list of gate names
// without the prefix. An unknown name throws, so that a misspelled exclusion is not ignored.
kj::Vector<AutogateKey> getAllAutogatesExceptions() {
  kj::Vector<AutogateKey> result;
  const char* env = getenv("WORKERD_ALL_AUTOGATES_EXCEPT");
  if (env == nullptr) {
    return result;
  }
  kj::StringPtr list = env;
  for (size_t start = 0; start <= list.size();) {
    size_t end = start;
    while (end < list.size() && list[end] != ',') {
      ++end;
    }
    auto name = list.slice(start, end);
    if (name.size() > 0) {
      result.add(KJ_REQUIRE_NONNULL(tryParseAutogateName(name),
          "WORKERD_ALL_AUTOGATES_EXCEPT names an unknown autogate", name));
    }
    start = end + 1;
  }
  return result;
}

}  // namespace

kj::StringPtr KJ_STRINGIFY(AutogateKey key) {
  switch (key) {
#define V(key)                                                                                     \
  case AutogateKey::key: {                                                                         \
    static constexpr ScreamingSnakeToKebab<sizeof(#key)> s(#key);                                  \
    return kj::StringPtr(s.value, sizeof(#key) - 1);                                               \
  }
    WORKERD_AUTOGATES(V)
#undef V
    case AutogateKey::NumOfKeys:
      KJ_FAIL_ASSERT("NumOfKeys should not be used in getName");
  }
}

Autogate::Autogate(capnp::List<capnp::Text>::Reader autogates) {
  // gates array is zero-initialized by default.
  for (const auto& name: autogates) {
    if (!name.startsWith(WORKERD_PREFIX)) {
      LOG_ERROR_ONCE("Autogate configuration includes gate with invalid prefix.");
      continue;
    }
    auto sliced = name.slice(WORKERD_PREFIX.size());

    // Parse the gate name into an AutogateKey.
    KJ_IF_SOME(key, tryParseAutogateName(sliced.asArray())) {
      gates[autogateToIndex(key)] = true;
    }
  }
}

void Autogate::enableAll(kj::ArrayPtr<const AutogateKey> excluded) {
  bool left[autogateToIndex(AutogateKey::NumOfKeys)] = {};
  for (AutogateKey key: getAllAutogatesExceptions()) {
    left[autogateToIndex(key)] = true;
  }
  for (AutogateKey key: excluded) {
    left[autogateToIndex(key)] = true;
  }
  for (AutogateKey key: getAutogateKeys()) {
    if (!left[autogateToIndex(key)]) {
      gates[autogateToIndex(key)] = true;
    }
  }
}

bool Autogate::isEnabled(AutogateKey key) {
  KJ_IF_SOME(a, globalAutogate) {
    return a.gates[autogateToIndex(key)];
  }

  static const Autogate defaultAutogate = []() {
    Autogate autogate;
    if (getenv("WORKERD_ALL_AUTOGATES") != nullptr) {
      autogate.enableAll();
    }
    return autogate;
  }();
  return defaultAutogate.gates[autogateToIndex(key)];
}

void Autogate::initAutogate(capnp::List<capnp::Text>::Reader gates,
    IgnoreAllAutogatesEnv ignoreEnv,
    kj::ArrayPtr<const AutogateKey> excludedFromAll) {
  Autogate autogate(gates);
  // If the WORKERD_ALL_AUTOGATES env var is set, enable all gates as well as those passed in. This
  // ensures the @all-autogates test variant works even when initAutogate({}) is called early
  // (e.g. by TestFixture), which would otherwise set globalAutogate to all-false and prevent
  // isEnabled() from reaching its env var fallback.
  //
  // Callers (e.g. the production server) that manage the all-autogates behavior themselves and
  // build selective gate configs can pass IgnoreAllAutogatesEnv::YES to skip this override.
  if (!ignoreEnv && getenv("WORKERD_ALL_AUTOGATES") != nullptr) {
    autogate.enableAll(excludedFromAll);
  }
  globalAutogate = kj::mv(autogate);
  syncZlibRouter();
}

void Autogate::deinitAutogate() {
  globalAutogate = kj::none;
  syncZlibRouter();
}

void Autogate::initAllAutogates() {
  // The gates already enabled, such as those a config names, stay enabled.
  Autogate autogate = globalAutogate.orDefault(Autogate());
  autogate.enableAll();
  globalAutogate = kj::mv(autogate);
  syncZlibRouter();
}

void Autogate::initAutogateNamesForTest(
    std::initializer_list<kj::StringPtr> gateNames, IgnoreAllAutogatesEnv ignoreEnv) {
  initAutogateNamesForTest(
      kj::ArrayPtr<const kj::StringPtr>(gateNames.begin(), gateNames.size()), ignoreEnv);
}

void Autogate::initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr> gateNames,
    IgnoreAllAutogatesEnv ignoreEnv,
    kj::ArrayPtr<const AutogateKey> excludedFromAll) {
  capnp::MallocMessageBuilder message;
  auto orphanage = message.getOrphanage();
  auto gatesOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(gateNames.size());
  auto gates = gatesOrphan.get();
  size_t count = 0;
  for (auto name: gateNames) {
    gates.set(count++, kj::str(WORKERD_PREFIX, name));
  }
  Autogate::initAutogate(gates.asReader(), ignoreEnv, excludedFromAll);
}

void Autogate::initAutogateForTest(std::initializer_list<AutogateKey> keys) {
  Autogate autogate;
  for (auto key: keys) {
    autogate.gates[autogateToIndex(key)] = true;
  }
  if (getenv("WORKERD_ALL_AUTOGATES") != nullptr) {
    autogate.enableAll();
  }
  globalAutogate = kj::mv(autogate);
  syncZlibRouter();
}

}  // namespace workerd::util
