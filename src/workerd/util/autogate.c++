// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
#include "autogate.h"

#include <workerd/util/sentry.h>

#include <stdlib.h>

#include <capnp/message.h>
#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::util {

kj::Maybe<Autogate> globalAutogate;

namespace {
constexpr auto WORKERD_PREFIX = "workerd-autogate-"_kj;

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
  for (auto name: autogates) {
    if (!name.startsWith(WORKERD_PREFIX)) {
      LOG_ERROR_ONCE("Autogate configuration includes gate with invalid prefix.");
      continue;
    }
    auto sliced = name.slice(WORKERD_PREFIX.size());

    // Parse the gate name into a AutogateKey.
    for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys;
         i = AutogateKey(static_cast<int>(i) + 1)) {
      if (kj::str(i) == sliced) {
        gates[static_cast<unsigned long>(i)] = true;
        break;
      }
    }
  }
}

bool Autogate::isEnabled(AutogateKey key) {
  KJ_IF_SOME(a, globalAutogate) {
    return a.gates[static_cast<unsigned long>(key)];
  }

  static const bool defaultResult = getenv("WORKERD_ALL_AUTOGATES") != nullptr;
  return defaultResult;
}

void Autogate::initAutogate(
    capnp::List<capnp::Text>::Reader gates, IgnoreAllAutogatesEnv ignoreEnv) {
  // If the WORKERD_ALL_AUTOGATES env var is set, enable all gates regardless of what
  // was passed in. This ensures the @all-autogates test variant works even when
  // initAutogate({}) is called early (e.g. by TestFixture), which would otherwise
  // set globalAutogate to all-false and prevent isEnabled() from reaching its env var
  // fallback.
  //
  // Callers (e.g. the production server) that manage the all-autogates behavior themselves and
  // build selective gate configs can pass IgnoreAllAutogatesEnv::YES to skip this override.
  if (!ignoreEnv && getenv("WORKERD_ALL_AUTOGATES") != nullptr) {
    return initAllAutogates();
  }
  globalAutogate = Autogate(gates);
}

void Autogate::deinitAutogate() {
  globalAutogate = kj::none;
}

void Autogate::initAllAutogates() {
  Autogate autogate;
  for (AutogateKey i = AutogateKey(0); i < AutogateKey::NumOfKeys;
       i = AutogateKey(static_cast<int>(i) + 1)) {
    autogate.gates[static_cast<unsigned long>(i)] = true;
  }
  globalAutogate = kj::mv(autogate);
}

void Autogate::initAutogateNamesForTest(std::initializer_list<kj::StringPtr> gateNames) {
  initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr>(gateNames.begin(), gateNames.size()));
}

void Autogate::initAutogateNamesForTest(kj::ArrayPtr<const kj::StringPtr> gateNames) {
  capnp::MallocMessageBuilder message;
  auto orphanage = message.getOrphanage();
  auto gatesOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(gateNames.size());
  auto gates = gatesOrphan.get();
  size_t count = 0;
  for (auto name: gateNames) {
    gates.set(count++, kj::str(WORKERD_PREFIX, name));
  }
  Autogate::initAutogate(gates.asReader());
}

}  // namespace workerd::util
