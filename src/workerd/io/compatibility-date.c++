// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <stdio.h>
#include "compatibility-date.h"
#include "time.h"
#include <capnp/schema.h>
#include <capnp/dynamic.h>
#include <kj/map.h>

namespace workerd {

using kj::uint;

namespace {

struct CompatDate {
  uint year;
  uint month;
  uint day;

  inline bool operator==(const CompatDate& other) const {
    return year == other.year && month == other.month && day == other.day;
  }
  inline bool operator<(const CompatDate& other) const {
    if (year < other.year) return true;
    if (year > other.year) return false;
    if (month < other.month) return true;
    if (month > other.month) return false;
    return day < other.day;
  }
  inline bool operator>=(const CompatDate& other) const {
    return !(*this < other);
  }

  static kj::Maybe<CompatDate> parse(kj::StringPtr text) {
    // Basic sanity check that years are 4-digit in the [2000,2999] range. If it is the year 3000
    // and this code broke, all I can say is: haha, take that robots, humans screwed you over yet
    // again, you can Roko's basilisk me all you want I don't care.
    if (!text.startsWith("2")) return nullptr;
    // Force 4-digit year, 2-digit month, and 2-digit day.
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
      return nullptr;
    }
    // Validate the date contains only digits and dashes.
    for (char c: text) {
      if ((c < '0' || '9' < c) && c != '-') return nullptr;
    }
    uint year, month, day;
    // TODO(someday): use `kj::parse` here instead
    auto result = sscanf(text.cStr(), "%d-%d-%d", &year, &month, &day);
    if (result == EOF) return nullptr;
    // Basic validation, notably this will happily accept invalid dates like 2022-02-30
    if (year < 2000 || year >= 3000) return nullptr;
    if (month < 1 || month > 12) return nullptr;
    if (day < 1 || day > 31) return nullptr;
    return CompatDate { year, month, day };
  }

  static CompatDate parse(kj::StringPtr text, Worker::ValidationErrorReporter& errorReporter) {
    static constexpr CompatDate DEFAULT_DATE { 2021, 5, 1 };
    KJ_IF_MAYBE(v, parse(text)) {
      return *v;
    } else {
      errorReporter.addError(kj::str("Invalid compatibility date: ", text));
      return DEFAULT_DATE;
    }
  }

  static CompatDate today() {
    time_t now = time(nullptr);
#if _MSC_VER
    // `gmtime` is thread-safe on Windows: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/gmtime-gmtime32-gmtime64?view=msvc-170#return-value
    auto t = *gmtime(&now);
#else
    struct tm t;
    KJ_ASSERT(gmtime_r(&now, &t) == &t);
#endif
    return { (uint)(t.tm_year + 1900), (uint)(t.tm_mon + 1), (uint)t.tm_mday };
  }

  kj::String toString() {
    return kj::str(year, '-', month < 10 ? "0" : "",  month, '-', day < 10 ? "0" : "", day);
  }
};

}  // namespace

void compileCompatibilityFlags(kj::StringPtr compatDate, capnp::List<capnp::Text>::Reader compatFlags,
                         CompatibilityFlags::Builder output,
                         Worker::ValidationErrorReporter& errorReporter,
                         bool allowExperimentalFeatures,
                         CompatibilityDateValidation dateValidation) {
  auto parsedCompatDate = CompatDate::parse(compatDate, errorReporter);

  switch (dateValidation) {
    case CompatibilityDateValidation::CODE_VERSION:
      if (KJ_ASSERT_NONNULL(CompatDate::parse(SUPPORTED_COMPATIBILITY_DATE)) < parsedCompatDate) {
        errorReporter.addError(kj::str(
            "This Worker requires compatibility date \"", parsedCompatDate, "\", but the newest "
            "date supported by this server binary is \"", SUPPORTED_COMPATIBILITY_DATE, "\"."));
      }
      break;

    case CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE:
      if (CompatDate::today() < parsedCompatDate) {
        errorReporter.addError(kj::str(
            "Can't set compatibility date in the future: ", parsedCompatDate));
      }
      break;

    case CompatibilityDateValidation::FUTURE_FOR_TEST:
      // No validation.
      break;
  }

  kj::HashSet<kj::String> flagSet;
  for (auto flag: compatFlags) {
    flagSet.upsert(kj::str(flag), [&](auto& existing, auto&& newValue) {
      errorReporter.addError(kj::str("Feature flag specified multiple times: ", flag));
    });
  }

  auto schema = capnp::Schema::from<CompatibilityFlags>();
  auto dynamicOutput = capnp::toDynamic(output);

  for (auto field: schema.getFields()) {
    bool enableByDate = false;
    bool enableByFlag = false;
    bool disableByFlag = false;
    bool isExperimental = false;

    kj::Maybe<CompatDate> enableDate;
    kj::StringPtr enableFlagName;
    kj::StringPtr disableFlagName;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        enableFlagName = annotation.getValue().getText();
        KJ_IF_MAYBE(entry, flagSet.find(enableFlagName)) {
          enableByFlag = true;
          flagSet.erase(*entry);
        }
      } else if (annotation.getId() == COMPAT_DISABLE_FLAG_ANNOTATION_ID) {
        disableFlagName = annotation.getValue().getText();
        KJ_IF_MAYBE(entry, flagSet.find(disableFlagName)) {
          disableByFlag = true;
          flagSet.erase(*entry);
        }
      } else if (annotation.getId() == COMPAT_ENABLE_DATE_ANNOTATION_ID) {
        auto parsedDate = KJ_ASSERT_NONNULL(CompatDate::parse(annotation.getValue().getText()));
        enableDate = parsedDate;
        enableByDate = parsedCompatDate >= parsedDate;
      } else if (annotation.getId() == COMPAT_ENABLE_ALL_DATES_ANNOTATION_ID) {
        enableByDate = true;
      } else if (annotation.getId() == EXPERIMENTAl_ANNOTATION_ID) {
        isExperimental = true;
      }
    }

    // Check for conflicts.
    if (enableByFlag && disableByFlag) {
      errorReporter.addError(kj::str(
          "Compatibility flags are mutually contradictory: ",
          enableFlagName, " vs ", disableFlagName));
    }
    if (enableByFlag && enableByDate) {
      KJ_IF_MAYBE(d, enableDate) {
        errorReporter.addError(kj::str(
            "The compatibility flag ", enableFlagName, " became the default as of ",
            *d, " so does not need to be specified anymore."));
      } else {
        errorReporter.addError(kj::str(
            "The compatibility flag ", enableFlagName,
            " is the default, so does not need to be specified anymore."));
      }
    }
    if (disableByFlag && !enableByDate) {
      // We don't consider it an error to specify a disable flag when the compatibility date makes
      // it redundant, because at a future date it won't be redundant, and someone could want to
      // set the flag early to make sure they don't forget later.
    }
    if (enableByFlag && isExperimental && !allowExperimentalFeatures) {
      if (dateValidation == CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE) {
        errorReporter.addError(kj::str("The compatibility flag ", enableFlagName,
            " is experimental and cannot yet be used in Workers deployed to Cloudflare."));
      } else {
        errorReporter.addError(kj::str(
            "The compatibility flag ", enableFlagName, " is experimental and may break or be "
            "removed in a future version of workerd. To use this flag, you must pass --experimental "
            "on the command line."));
      }
    }

    dynamicOutput.set(field, enableByFlag || (enableByDate && !disableByFlag));
  }

  for (auto& flag: flagSet) {
    errorReporter.addError(kj::str("No such feature flag: ", flag));
  }
}

namespace {

struct ParsedField {
  kj::StringPtr enableFlag;
  capnp::StructSchema::Field field;
};

kj::Array<const ParsedField> makeFieldTable(
    capnp::StructSchema::FieldList fields) {
  kj::Vector<ParsedField> table(fields.size());

  for (auto field: fields) {
    kj::Maybe<kj::StringPtr> enableFlag;
    bool neededByFl = false;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        enableFlag = annotation.getValue().getText();
      } else if (annotation.getId() == NEEDED_BY_FL) {
        neededByFl = true;
      }
    }

    if (neededByFl) {
      table.add(ParsedField {
        .enableFlag = KJ_REQUIRE_NONNULL(enableFlag),
        .field = field,
      });
    }
  }

  return table.releaseAsArray();
}

}  // namespace

kj::Array<kj::StringPtr> decompileCompatibilityFlagsForFl(CompatibilityFlags::Reader input) {
  static auto fieldTable = makeFieldTable(
      capnp::Schema::from<CompatibilityFlags>().getFields());

  kj::Vector<kj::StringPtr> enableFlags;

  for (auto field: fieldTable) {
    if (capnp::toDynamic(input).get(field.field).as<bool>()) {
      enableFlags.add(field.enableFlag);
    }
  }

  return enableFlags.releaseAsArray();
}

kj::Maybe<kj::String> normalizeCompatDate(kj::StringPtr date) {
  return CompatDate::parse(date).map([](auto v) { return v.toString(); });
}

}  // namespace workerd
