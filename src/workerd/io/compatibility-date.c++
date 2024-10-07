// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compatibility-date.h"

#include "time.h"

#include <capnp/dynamic.h>
#include <capnp/schema.h>
#include <kj/debug.h>
#include <kj/map.h>
#include <kj/vector.h>

#include <cstdio>

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
    if (!text.startsWith("2")) return kj::none;
    // Force 4-digit year, 2-digit month, and 2-digit day.
    if (text.size() != 10 || text[4] != '-' || text[7] != '-') {
      return kj::none;
    }
    // Validate the date contains only digits and dashes.
    for (char c: text) {
      if ((c < '0' || '9' < c) && c != '-') return kj::none;
    }
    uint year, month, day;
    // TODO(someday): use `kj::parse` here instead
    auto result = sscanf(text.cStr(), "%d-%d-%d", &year, &month, &day);
    if (result == EOF || result < 3) return kj::none;
    // Basic validation, notably this will happily accept invalid dates like 2022-02-30
    if (year < 2000 || year >= 3000) return kj::none;
    if (month < 1 || month > 12) return kj::none;
    if (day < 1 || day > 31) return kj::none;
    return CompatDate{year, month, day};
  }

  static CompatDate parse(kj::StringPtr text, Worker::ValidationErrorReporter& errorReporter) {
    static constexpr CompatDate DEFAULT_DATE{2021, 5, 1};
    KJ_IF_SOME(v, parse(text)) {
      return v;
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
    return {(uint)(t.tm_year + 1900), (uint)(t.tm_mon + 1), (uint)t.tm_mday};
  }

  kj::String toString() {
    return kj::str(year, '-', month < 10 ? "0" : "", month, '-', day < 10 ? "0" : "", day);
  }
};
}  // namespace

kj::String currentDateStr() {
  return CompatDate::today().toString();
}

void compileCompatibilityFlags(kj::StringPtr compatDate,
    capnp::List<capnp::Text>::Reader compatFlags,
    CompatibilityFlags::Builder output,
    Worker::ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation) {
  auto parsedCompatDate = CompatDate::parse(compatDate, errorReporter);

  switch (dateValidation) {
    case CompatibilityDateValidation::CODE_VERSION:
      if (KJ_ASSERT_NONNULL(CompatDate::parse(SUPPORTED_COMPATIBILITY_DATE)) < parsedCompatDate) {
        errorReporter.addError(
            kj::str("This Worker requires compatibility date \"", parsedCompatDate,
                "\", but the newest "
                "date supported by this server binary is \"",
                SUPPORTED_COMPATIBILITY_DATE, "\"."));
      }
      break;

    case CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE:
      if (CompatDate::today() < parsedCompatDate) {
        errorReporter.addError(
            kj::str("Can't set compatibility date in the future: ", parsedCompatDate));
      }
      break;

    case CompatibilityDateValidation::FUTURE_FOR_TEST:
      // No validation.
      break;
  }

  kj::HashSet<kj::String> flagSet;
  flagSet.reserve(compatFlags.size());
  for (auto flag: compatFlags) {
    flagSet.upsert(kj::str(flag), [&](auto& existing, auto&& newValue) {
      errorReporter.addError(kj::str("Compatibility flag specified multiple times: ", flag));
    });
  }

  auto schema = capnp::Schema::from<CompatibilityFlags>();
  auto dynamicOutput = capnp::toDynamic(output);

  // For each item added to this list, the flag identified by field will be
  // enabled if the flag identified by other is enabled.
  struct ImpliedBy {
    capnp::StructSchema::Field field;
    capnp::StructSchema::Field other;
  };
  kj::Vector<ImpliedBy> impliedByList(schema.getFields().size());

  for (auto field: schema.getFields()) {
    bool enableByDate = false;
    bool enableByFlag = false;
    bool disableByFlag = false;
    bool isExperimental = false;

    kj::Maybe<CompatDate> enableDate;
    kj::StringPtr enableFlagName;
    kj::StringPtr disableFlagName;
    kj::Vector<ImpliedBy> impliedByVector;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
        enableFlagName = annotation.getValue().getText();
        KJ_IF_SOME(entry, flagSet.find(enableFlagName)) {
          enableByFlag = true;
          flagSet.erase(entry);
        }
      } else if (annotation.getId() == COMPAT_DISABLE_FLAG_ANNOTATION_ID) {
        disableFlagName = annotation.getValue().getText();
        KJ_IF_SOME(entry, flagSet.find(disableFlagName)) {
          disableByFlag = true;
          flagSet.erase(entry);
        }
      } else if (annotation.getId() == COMPAT_ENABLE_DATE_ANNOTATION_ID) {
        auto parsedDate = KJ_ASSERT_NONNULL(CompatDate::parse(annotation.getValue().getText()));
        enableDate = parsedDate;
        enableByDate = parsedCompatDate >= parsedDate;
      } else if (annotation.getId() == COMPAT_ENABLE_ALL_DATES_ANNOTATION_ID) {
        enableByDate = true;
      } else if (annotation.getId() == EXPERIMENTAl_ANNOTATION_ID) {
        isExperimental = true;
      } else if (annotation.getId() == IMPLIED_BY_AFTER_DATE_ANNOTATION_ID) {
        auto value = annotation.getValue();
        auto s = value.getStruct().getAs<workerd::ImpliedByAfterDate>();
        auto parsedDate = KJ_ASSERT_NONNULL(CompatDate::parse(s.getDate()));
        // This flag will be marked as enabled if the flag identified by
        // s.getName() is enabled, but only on or after the specified date.
        if (parsedCompatDate >= parsedDate && !disableByFlag) {
          if (s.hasName()) {
            impliedByVector.add(ImpliedBy{
              .field = field,
              .other = schema.getFieldByName(s.getName()),
            });
          } else if (s.hasNames()) {
            for (auto name: s.getNames()) {
              impliedByVector.add(ImpliedBy{
                .field = field,
                .other = schema.getFieldByName(name),
              });
            }
          }
        }
      }
    }
    for (auto& impliedBy: impliedByVector) {
      // We only want to add the implied by flag if it is not explicitly disabled.
      if (!disableByFlag) {
        impliedByList.add(kj::mv(impliedBy));
      }
    }

    // Check for conflicts.
    if (enableByFlag && disableByFlag) {
      errorReporter.addError(kj::str("Compatibility flags are mutually contradictory: ",
          enableFlagName, " vs ", disableFlagName));
    }
    if (enableByFlag && enableByDate) {
      KJ_IF_SOME(d, enableDate) {
        errorReporter.addError(kj::str("The compatibility flag ", enableFlagName,
            " became the default as of ", d, " so does not need to be specified anymore."));
      } else {
        errorReporter.addError(kj::str("The compatibility flag ", enableFlagName,
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
        errorReporter.addError(kj::str("The compatibility flag ", enableFlagName,
            " is experimental and may break or be "
            "removed in a future version of workerd. To use this flag, you must pass --experimental "
            "on the command line."));
      }
    }

    dynamicOutput.set(field, enableByFlag || (enableByDate && !disableByFlag));
  }

  for (auto& implied: impliedByList) {
    if (capnp::toDynamic(output).get(implied.other).as<bool>()) {
      dynamicOutput.set(implied.field, true);
    }
  }

  for (auto& flag: flagSet) {
    errorReporter.addError(kj::str("No such compatibility flag: ", flag));
  }
}

namespace {

struct ParsedField {
  kj::StringPtr enableFlag;
  capnp::StructSchema::Field field;
};

kj::Array<const ParsedField> makeFieldTable(capnp::StructSchema::FieldList fields) {
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
      table.add(ParsedField{
        .enableFlag = KJ_REQUIRE_NONNULL(enableFlag),
        .field = field,
      });
    }
  }

  return table.releaseAsArray();
}

}  // namespace

kj::Array<kj::StringPtr> decompileCompatibilityFlagsForFl(CompatibilityFlags::Reader input) {
  static const auto fieldTable =
      makeFieldTable(capnp::Schema::from<CompatibilityFlags>().getFields());

  kj::Vector<kj::StringPtr> enableFlags;
  enableFlags.reserve(fieldTable.size());
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

struct PythonSnapshotParsedField {
  PythonSnapshotRelease::Reader pythonSnapshotRelease;
  capnp::StructSchema::Field field;
};

kj::Array<const PythonSnapshotParsedField> makePythonSnapshotFieldTable(
    capnp::StructSchema::FieldList fields) {
  kj::Vector<PythonSnapshotParsedField> table(fields.size());

  for (auto field: fields) {
    kj::Maybe<PythonSnapshotRelease::Reader> maybePythonSnapshotRelease;

    for (auto annotation: field.getProto().getAnnotations()) {
      if (annotation.getId() == PYTHON_SNAPSHOT_RELEASE_ANNOTATION_ID) {
        maybePythonSnapshotRelease =
            annotation.getValue().getStruct().getAs<workerd::PythonSnapshotRelease>();
      }
    }

    KJ_IF_SOME(pythonSnapshotRelease, maybePythonSnapshotRelease) {
      table.add(PythonSnapshotParsedField{
        .pythonSnapshotRelease = pythonSnapshotRelease,
        .field = field,
      });
    }
  }

  return table.releaseAsArray();
}

kj::Maybe<PythonSnapshotRelease::Reader> getPythonSnapshotRelease(
    CompatibilityFlags::Reader featureFlags) {
  uint latestFieldOrdinal = 0;
  kj::Maybe<PythonSnapshotRelease::Reader> result;

  static const auto fieldTable =
      makePythonSnapshotFieldTable(capnp::Schema::from<CompatibilityFlags>().getFields());

  for (auto field: fieldTable) {
    bool isEnabled = capnp::toDynamic(featureFlags).get(field.field).as<bool>();
    if (!isEnabled) {
      continue;
    }

    // We pick the flag with the highest ordinal value that is enabled and has a
    // pythonSnapshotRelease annotation.
    //
    // The fieldTable is probably ordered by the ordinal anyway, but doesn't hurt to be explicit
    // here.
    //
    // TODO(later): make sure this is well tested once we have more than one compat flag.
    if (latestFieldOrdinal < field.field.getIndex()) {
      latestFieldOrdinal = field.field.getIndex();
      result = field.pythonSnapshotRelease;
    }
  }

  return result;
}

kj::String getPythonBundleName(PythonSnapshotRelease::Reader pyodideRelease) {
  if (pyodideRelease.getPyodide() == "dev") {
    return kj::str("dev");
  }
  return kj::str(pyodideRelease.getPyodide(), "_", pyodideRelease.getPyodideRevision(), "_",
      pyodideRelease.getBackport());
}

}  // namespace workerd
