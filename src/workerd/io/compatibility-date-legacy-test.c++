// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compatibility-date-legacy.h"

#include <workerd/io/maximum-compatibility-date.embed.h>
#include <workerd/util/autogate.h>

#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/test.h>
#include <kj/time.h>
#include <kj/vector.h>

namespace workerd {
namespace {

struct Reporter final: public ValidationErrorReporter {
  kj::Vector<kj::String> errors;
  kj::Vector<kj::String> warnings;

  void addError(kj::String error) override {
    errors.add(kj::mv(error));
  }
  void addWarning(kj::String warning) override {
    warnings.add(kj::mv(warning));
  }
  void addEntrypoint(kj::Maybe<kj::StringPtr> exportName, kj::Array<kj::String> methods) override {
    KJ_UNREACHABLE;
  }
  void addActorClass(kj::StringPtr exportName) override {
    KJ_UNREACHABLE;
  }
  void addWorkflowClass(kj::StringPtr exportName, kj::Array<kj::String> methods) override {
    KJ_UNREACHABLE;
  }
};

// The Rust compiler must agree with the C++ one, output and diagnostics alike. With the
// RUST_COMPAT_DATE autogate on, compatibility-date.h is the Rust compiler and legacy:: the C++ one.
KJ_TEST("the Rust compiler agrees with the C++ compiler") {
  util::Autogate::initAutogateForTest({util::AutogateKey::RUST_COMPAT_DATE});
  KJ_DEFER(util::Autogate::deinitAutogate());

  kj::Vector<kj::String> flagNames;
  kj::Vector<kj::String> dates;
  for (auto field: capnp::Schema::from<CompatibilityFlags>().getFields()) {
    for (auto annotation: field.getProto().getAnnotations()) {
      auto id = annotation.getId();
      if (id == COMPAT_ENABLE_FLAG_ANNOTATION_ID || id == COMPAT_DISABLE_FLAG_ANNOTATION_ID) {
        flagNames.add(kj::str(annotation.getValue().getText()));
      } else if (id == COMPAT_ENABLE_DATE_ANNOTATION_ID) {
        dates.add(kj::str(annotation.getValue().getText()));
      } else if (id == IMPLIED_BY_AFTER_DATE_ANNOTATION_ID) {
        dates.add(
            kj::str(annotation.getValue().getStruct().as<workerd::ImpliedByAfterDate>().getDate()));
      }
    }
  }
  // The day before each date too, where that is a simple matter of the day of the month.
  for (auto i: kj::indices(dates)) {
    auto& d = dates[i];
    unsigned day = (d[8] - '0') * 10 + (d[9] - '0');
    if (day > 1) {
      dates.add(kj::str(d.first(8), day - 1 < 10 ? "0" : "", day - 1));
    }
  }
  for (kj::StringPtr d: {"abcd"_kj, "2021-05-1-"_kj, ""_kj, "2000-01-01"_kj, "2999-12-31"_kj,
         "2252-04-01"_kj, "3000-01-01"_kj, "2021-\xfe"_kj, "\xff\xfe"_kj}) {
    dates.add(kj::str(d));
  }
  // A NUL inside the text, which a kj::String may hold.
  dates.add(kj::heapString("2021-05-17\0x", 12));
  dates.add(currentDateStr());
  dates.add(legacy::currentDateStr());
  dates.add(newestCompatDateStr());
  dates.add(legacy::newestCompatDateStr());
  dates.add(kj::str(MAXIMUM_COMPATIBILITY_DATE));

  constexpr CompatibilityDateValidation validations[] = {
    CompatibilityDateValidation::CODE_VERSION,
    CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE,
    CompatibilityDateValidation::FUTURE_FOR_TEST,
  };

  unsigned cases = 0;
  auto rustTime = 0 * kj::NANOSECONDS;
  auto legacyTime = 0 * kj::NANOSECONDS;
  auto& clock = kj::systemPreciseMonotonicClock();
  auto check = [&](kj::StringPtr date, kj::ArrayPtr<const kj::String> flags,
                   CompatibilityDateValidation validation, bool experimental,
                   kj::ArrayPtr<const kj::StringPtr> allowlist, MainModuleIsPython python) {
    ++cases;
    capnp::MallocMessageBuilder message;
    auto orphanage = message.getOrphanage();
    auto actual = orphanage.newOrphan<CompatibilityFlags>();
    auto expected = orphanage.newOrphan<CompatibilityFlags>();
    Reporter actualReporter;
    Reporter expectedReporter;
    auto start = clock.now();
    compileCompatibilityFlags(
        date, flags, actual.get(), actualReporter, experimental, validation, allowlist, python);
    auto middle = clock.now();
    legacy::compileCompatibilityFlags(
        date, flags, expected.get(), expectedReporter, experimental, validation, allowlist, python);
    rustTime += middle - start;
    legacyTime += clock.now() - middle;

    auto same = [](auto& a, auto& b) {
      if (a.size() != b.size()) return false;
      for (auto i: kj::indices(a)) {
        if (a[i] != b[i]) return false;
      }
      return true;
    };
    auto context = [&]() {
      return kj::str(date, " [", kj::strArray(flags, ", "), "] ", static_cast<int>(validation), " ",
          experimental, " ", kj::strArray(allowlist, ","), " ", python.toBool());
    };
    auto actualBytes = capnp::canonicalize(actual.getReader());
    auto expectedBytes = capnp::canonicalize(expected.getReader());
    if (actualBytes.asBytes() != expectedBytes.asBytes()) {
      KJ_FAIL_ASSERT("flags differ", context(), actual.getReader(), expected.getReader());
    }
    if (!same(actualReporter.errors, expectedReporter.errors)) {
      KJ_FAIL_ASSERT("errors differ", context(), kj::strArray(actualReporter.errors, "\n"),
          kj::strArray(expectedReporter.errors, "\n"));
    }
    if (!same(actualReporter.warnings, expectedReporter.warnings)) {
      KJ_FAIL_ASSERT("warnings differ", context(), kj::strArray(actualReporter.warnings, "\n"),
          kj::strArray(expectedReporter.warnings, "\n"));
    }
    auto all = decompileCompatibilityFlags(actual.getReader());
    auto legacyAll = legacy::decompileCompatibilityFlags(expected.getReader());
    KJ_ASSERT(same(all, legacyAll), context());
    auto fl = decompileCompatibilityFlagsForFl(actual.getReader());
    auto legacyFl = legacy::decompileCompatibilityFlagsForFl(expected.getReader());
    KJ_ASSERT(same(fl, legacyFl), context());
  };

  auto list = [](std::initializer_list<kj::StringPtr> flags) {
    return KJ_MAP(f, flags) { return kj::str(f); };
  };

  // Every date, with no flags.
  for (auto& date: dates) {
    KJ_EXPECT(normalizeCompatDate(date) == legacy::normalizeCompatDate(date), date);
    for (auto validation: validations) {
      check(date, nullptr, validation, false, nullptr, MainModuleIsPython::NO);
      check(date, list({"nodejs_compat"_kj}), validation, false, nullptr, MainModuleIsPython::NO);
      for (auto python: {MainModuleIsPython::NO, MainModuleIsPython::YES}) {
        check(date, list({"auto_inject_python_workers"_kj}), validation, true, nullptr, python);
      }
    }
  }

  // Every flag alone, at a spread of dates.
  for (kj::StringPtr date: {"2000-01-01"_kj, "2022-10-31"_kj, "2024-09-23"_kj, "2026-11-17"_kj,
         "2999-12-31"_kj, "abcd"_kj}) {
    for (auto& flag: flagNames) {
      auto flags = list({flag});
      kj::StringPtr allowlist[] = {"some_other_flag"_kj, flag};
      for (auto validation: validations) {
        check(date, flags, validation, false, nullptr, MainModuleIsPython::NO);
      }
      check(date, flags, CompatibilityDateValidation::CODE_VERSION, true, nullptr,
          MainModuleIsPython::NO);
      check(date, flags, CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE, false, allowlist,
          MainModuleIsPython::NO);
    }
  }

  // Many flags at once, with duplicates, unknown flags and contradictions, in several orders.
  auto all = KJ_MAP(f, flagNames) { return kj::str(f); };
  kj::Vector<kj::String> noisy;
  for (auto i: kj::indices(flagNames)) {
    noisy.add(kj::str(flagNames[i]));
    if (i % 7 == 0) noisy.add(kj::str("unknown_flag_", i));
    if (i % 11 == 0) noisy.add(kj::str(flagNames[i / 2]));
    // Not UTF-8, and duplicated.
    if (i % 13 == 0) noisy.add(kj::str("bad_\xff_flag_", i % 2));
    // Holds a NUL after a real flag's name.
    if (i % 17 == 0) noisy.add(kj::str(flagNames[i], kj::StringPtr("\0x", 2)));
  }
  kj::Vector<kj::String> reversed;
  for (auto i = noisy.size(); i > 0; --i) {
    reversed.add(kj::str(noisy[i - 1]));
  }
  kj::Vector<kj::String> enableOnly;
  for (auto field: capnp::Schema::from<CompatibilityFlags>().getFields()) {
    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        enableOnly.add(kj::str(annotation.getValue().getText()));
      }
    }
  }
  for (kj::StringPtr date: {"2000-01-01"_kj, "2024-09-23"_kj, "2999-12-31"_kj, "abcd"_kj}) {
    for (auto validation: validations) {
      for (bool experimental: {false, true}) {
        for (auto python: {MainModuleIsPython::NO, MainModuleIsPython::YES}) {
          check(date, all, validation, experimental, nullptr, python);
          check(date, noisy, validation, experimental, nullptr, python);
          check(date, reversed, validation, experimental, nullptr, python);
          check(date, enableOnly, validation, experimental, nullptr, python);
        }
      }
    }
  }

  KJ_LOG(INFO, "compared compilations", cases, rustTime, legacyTime);
}

}  // namespace
}  // namespace workerd
