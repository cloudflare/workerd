// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compatibility-date.h"

#include <workerd/io/maximum-compatibility-date.embed.h>

#include <capnp/message.h>
#include <capnp/serialize-text.h>
#include <kj/debug.h>
#include <kj/test.h>

#include <chrono>
#include <format>

namespace workerd {
namespace {

KJ_TEST("compatibility date parsing") {
  auto expectParseTo = [](kj::StringPtr input, kj::StringPtr expected) {
    KJ_IF_SOME(actual, normalizeCompatDate(input)) {
      KJ_EXPECT(actual == expected);
    } else {
      KJ_FAIL_EXPECT("couldn't parse", input);
    }
  };

  auto expectNoParse = [](kj::StringPtr input) {
    KJ_IF_SOME(actual, normalizeCompatDate(input)) {
      KJ_FAIL_EXPECT("expected couldn't parse", input, actual);
    }
  };

  expectParseTo("2021-05-17", "2021-05-17");
  expectParseTo("2021-05-01", "2021-05-01");
  expectParseTo("2000-01-01", "2000-01-01");
  expectParseTo("2999-12-31", "2999-12-31");
  expectParseTo("2024-02-29", "2024-02-29");
  expectParseTo("2112-04-01", "2112-04-01");

  // Alas, strptime() accepts February 30 as a perfectly valid date.
  //expectNoParse("2024-2-30");
  //expectNoParse("2023-2-29");

  expectNoParse("2024-2-32");
  expectNoParse("3000-01-01");
  expectNoParse("1999-12-31");
  expectNoParse("123-01-01");
  expectNoParse("2021-13-01");
  expectNoParse("2021-12-32");
  expectNoParse("2021-00-01");
  expectNoParse("2021-01-00");

  expectNoParse(" 2021-05-17");
  expectNoParse("2021 -05-17");
  expectNoParse("2021- 05-17");
  expectNoParse("2021-05 -17");
  expectNoParse("2021-05- 17");
  expectNoParse("2021-05-17 ");
  expectNoParse("2021/05/17");
  expectNoParse("2021_05_17");

  expectNoParse("2021-5-07");
  expectNoParse("2021-05-7");
  expectNoParse("202-05-07");
}

KJ_TEST("compatibility flag parsing") {
  auto expectCompileCompatibilityFlags =
      [](kj::StringPtr compatDate, kj::ArrayPtr<const kj::StringPtr> featureFlags,
          kj::StringPtr expectedOutput, kj::ArrayPtr<const kj::StringPtr> expectedErrors = nullptr,
          CompatibilityDateValidation dateValidation = CompatibilityDateValidation::FUTURE_FOR_TEST,
          bool r2InternalBetaApiSet = false, bool experimental = false) {
    capnp::MallocMessageBuilder message;
    auto orphanage = message.getOrphanage();

    auto flagListOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(featureFlags.size());
    auto flagList = flagListOrphan.get();
    for (auto i: kj::indices(featureFlags)) {
      flagList.set(i, featureFlags.begin()[i]);
    }

    auto outputOrphan = orphanage.newOrphan<CompatibilityFlags>();
    auto output = outputOrphan.get();

    SimpleWorkerErrorReporter errorReporter;
    compileCompatibilityFlags(
        compatDate, flagList.asReader(), output, errorReporter, experimental, dateValidation);

    capnp::TextCodec codec;
    auto parsedExpectedOutput = codec.decode<CompatibilityFlags>(expectedOutput, orphanage);

    if (!r2InternalBetaApiSet) {
      // The r2PublicBetaApi is always expected by default regardless of compat date unless
      // explicitly disabled.
      parsedExpectedOutput.get().setR2PublicBetaApi(true);
    }

    // If errors are expected, then the output is irrelevant.
    if (expectedErrors.size() == 0) {
      KJ_EXPECT(kj::str(output) == kj::str(parsedExpectedOutput.getReader()));
    }
    KJ_EXPECT(kj::strArray(errorReporter.errors, "\n") == kj::strArray(expectedErrors, "\n"));
  };

  expectCompileCompatibilityFlags("2021-05-17", {}, "()");

  expectCompileCompatibilityFlags("2021-11-02", {}, "(formDataParserSupportsFiles = false)");
  expectCompileCompatibilityFlags("2021-11-03", {}, "(formDataParserSupportsFiles = true)");
  expectCompileCompatibilityFlags("2021-11-04", {}, "(formDataParserSupportsFiles = true)");
  expectCompileCompatibilityFlags("2021-11-03", {"formdata_parser_converts_files_to_strings"},
      "(formDataParserSupportsFiles = false)");

  // Test compatibility flag overrides.
  expectCompileCompatibilityFlags(
      "2021-05-17", {"formdata_parser_supports_files"_kj}, "(formDataParserSupportsFiles = true)");
  expectCompileCompatibilityFlags("2021-05-17", {"fetch_refuses_unknown_protocols"_kj},
      "(fetchRefusesUnknownProtocols = true)");
  expectCompileCompatibilityFlags("2021-05-17",
      {"formdata_parser_supports_files"_kj, "fetch_refuses_unknown_protocols"_kj},
      "(formDataParserSupportsFiles = true, fetchRefusesUnknownProtocols = true)");
  expectCompileCompatibilityFlags("2021-11-04", {"fetch_refuses_unknown_protocols"_kj},
      "(formDataParserSupportsFiles = true, fetchRefusesUnknownProtocols = true)");

  // Test errors.
  expectCompileCompatibilityFlags("abcd", {}, "()", {"Invalid compatibility date: abcd"});
  expectCompileCompatibilityFlags("2021-05-17",
      {"formdata_parser_supports_files"_kj, "formdata_parser_supports_files"_kj},
      "(formDataParserSupportsFiles = true)",
      {"Compatibility flag specified multiple times: formdata_parser_supports_files"});
  expectCompileCompatibilityFlags("2021-05-17",
      {"formdata_parser_supports_files"_kj, "formdata_parser_converts_files_to_strings"_kj},
      "(formDataParserSupportsFiles = true)",
      {"Compatibility flags are mutually contradictory: "
       "formdata_parser_supports_files vs formdata_parser_converts_files_to_strings"});
  expectCompileCompatibilityFlags("2021-11-04", {"formdata_parser_supports_files"_kj},
      "(formDataParserSupportsFiles = true)",
      {"The compatibility flag formdata_parser_supports_files became the default as of "
       "2021-11-03 so does not need to be specified anymore."},
      CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE);
  expectCompileCompatibilityFlags(
      "2021-05-17", {"unknown_feature"_kj}, "()", {"No such compatibility flag: unknown_feature"});

  expectCompileCompatibilityFlags("2252-04-01", {}, "()",
      {"Can't set compatibility date in the future: 2252-04-01"},
      CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE);

  expectCompileCompatibilityFlags("2252-04-01", {}, "()",
      {kj::str("This Worker requires compatibility date \"2252-04-01\", but the newest date "
               "supported by this server binary is \"",
           MAXIMUM_COMPATIBILITY_DATE, "\"."),
        kj::str("Can't set compatibility date in the future: \"2252-04-01\". Today's date "
                "(UTC) is \"",
            currentDateStr(), "\".")},
      CompatibilityDateValidation::CODE_VERSION);

  // Test experimental requirement using durable_object_rename as it is obsolete
  expectCompileCompatibilityFlags("2020-01-01", {"durable_object_rename"_kj}, "(obsolete19 = true)",
      {"The compatibility flag durable_object_rename is experimental and may break or be removed "
       "in a future version of workerd. To use this flag, you must pass --experimental on the "
       "command line."_kj},
      CompatibilityDateValidation::CODE_VERSION, false, false);
  expectCompileCompatibilityFlags("2020-01-01", {"durable_object_rename"_kj}, "(obsolete19 = true)",
      {}, CompatibilityDateValidation::CODE_VERSION, false, true);

  // Test experimental requirement using the durable_object_alarms flag since we know this flag
  // is obsolete and will never have a date set. (Should always pass, even if experimental flags
  // aren't allowed)
  expectCompileCompatibilityFlags("2020-01-01", {"durable_object_alarms"_kj}, "(obsolete14 = true)",
      {}, CompatibilityDateValidation::CODE_VERSION, false, false);
  expectCompileCompatibilityFlags("2020-01-01", {"durable_object_alarms"_kj}, "(obsolete14 = true)",
      {}, CompatibilityDateValidation::CODE_VERSION, false, true);

  // Multiple errors.
  expectCompileCompatibilityFlags("abcd",
      {"formdata_parser_supports_files"_kj, "fetch_refuses_unknown_protocols"_kj,
        "unknown_feature"_kj, "fetch_refuses_unknown_protocols"_kj, "another_feature"_kj,
        "formdata_parser_supports_files"_kj},
      "(formDataParserSupportsFiles = true, fetchRefusesUnknownProtocols = true)",
      {"Compatibility flag specified multiple times: fetch_refuses_unknown_protocols",
        "Compatibility flag specified multiple times: formdata_parser_supports_files",
        "Invalid compatibility date: abcd", "No such compatibility flag: another_feature",
        "No such compatibility flag: unknown_feature"});

  // Can explicitly disable flag that's enabled for all dates.s
  expectCompileCompatibilityFlags("2021-05-17", {"r2_internal_beta_bindings"}, "()", {},
      CompatibilityDateValidation::FUTURE_FOR_TEST, true, false);

  // nodejs_compat implies nodejs_compat_v2 on or after 2024-09-23
  expectCompileCompatibilityFlags("2024-09-23", {"nodejs_compat"},
      "(formDataParserSupportsFiles = true,"
      " fetchRefusesUnknownProtocols = true,"
      " esiIncludeIsVoidTag = false,"
      " obsolete3 = false,"
      " durableObjectFetchRequiresSchemeAuthority = true,"
      " streamsByobReaderDetachesBuffer = true,"
      " streamsJavaScriptControllers = true,"
      " jsgPropertyOnPrototypeTemplate = true,"
      " minimalSubrequests = true,"
      " noCotsOnExternalFetch = true,"
      " specCompliantUrl = true,"
      " globalNavigator = true,"
      " captureThrowsAsRejections = true,"
      " r2PublicBetaApi = true,"
      " obsolete14 = false,"
      " noSubstituteNull = true,"
      " transformStreamJavaScriptControllers = true,"
      " r2ListHonorIncludeFields = true,"
      " exportCommonJsDefaultNamespace = true,"
      " obsolete19 = false,"
      " webSocketCompression = true,"
      " nodeJsCompat = true,"
      " obsolete22 = false,"
      " specCompliantResponseRedirect = true,"
      " workerdExperimental = false,"
      " durableObjectGetExisting = false,"
      " httpHeadersGetSetCookie = true,"
      " dispatchExceptionTunneling = true,"
      " serviceBindingExtraHandlers = false,"
      " noCfBotManagementDefault = true,"
      " urlSearchParamsDeleteHasValueArg = true,"
      " strictCompression = true,"
      " brotliContentEncoding = true,"
      " strictCrypto = true,"
      " rttiApi = false,"
      " obsolete35 = false,"
      " cryptoPreservePublicExponent = true,"
      " vectorizeQueryMetadataOptional = true,"
      " unsafeModule = false,"
      " jsRpc = false,"
      " noImportScripts = true,"
      " nodeJsAls = false,"
      " queuesJsonMessages = true,"
      " pythonWorkers = false,"
      " fetcherNoGetPutDelete = true,"
      " unwrapCustomThenables = true,"
      " fetcherRpc = true,"
      " internalStreamByobReturn = true,"
      " blobStandardMimeType = true,"
      " fetchStandardUrl = true,"
      " nodeJsCompatV2 = true,"
      " globalFetchStrictlyPublic = false,"
      " newModuleRegistry = false,"
      " allowCustomPorts = true,"
      " internalWritableStreamAbortClearsQueue = true,"
      " nodeJsZlib = true)",
      {}, CompatibilityDateValidation::FUTURE_FOR_TEST, false, false);
  expectCompileCompatibilityFlags("2024-09-22", {"nodejs_compat"},
      "(formDataParserSupportsFiles = true,"
      " fetchRefusesUnknownProtocols = true,"
      " esiIncludeIsVoidTag = false,"
      " obsolete3 = false,"
      " durableObjectFetchRequiresSchemeAuthority = true,"
      " streamsByobReaderDetachesBuffer = true,"
      " streamsJavaScriptControllers = true,"
      " jsgPropertyOnPrototypeTemplate = true,"
      " minimalSubrequests = true,"
      " noCotsOnExternalFetch = true,"
      " specCompliantUrl = true,"
      " globalNavigator = true,"
      " captureThrowsAsRejections = true,"
      " r2PublicBetaApi = true,"
      " obsolete14 = false,"
      " noSubstituteNull = true,"
      " transformStreamJavaScriptControllers = true,"
      " r2ListHonorIncludeFields = true,"
      " exportCommonJsDefaultNamespace = true,"
      " obsolete19 = false,"
      " webSocketCompression = true,"
      " nodeJsCompat = true,"
      " obsolete22 = false,"
      " specCompliantResponseRedirect = true,"
      " workerdExperimental = false,"
      " durableObjectGetExisting = false,"
      " httpHeadersGetSetCookie = true,"
      " dispatchExceptionTunneling = true,"
      " serviceBindingExtraHandlers = false,"
      " noCfBotManagementDefault = true,"
      " urlSearchParamsDeleteHasValueArg = true,"
      " strictCompression = true,"
      " brotliContentEncoding = true,"
      " strictCrypto = true,"
      " rttiApi = false,"
      " obsolete35 = false,"
      " cryptoPreservePublicExponent = true,"
      " vectorizeQueryMetadataOptional = true,"
      " unsafeModule = false,"
      " jsRpc = false,"
      " noImportScripts = true,"
      " nodeJsAls = false,"
      " queuesJsonMessages = true,"
      " pythonWorkers = false,"
      " fetcherNoGetPutDelete = true,"
      " unwrapCustomThenables = true,"
      " fetcherRpc = true,"
      " internalStreamByobReturn = true,"
      " blobStandardMimeType = true,"
      " fetchStandardUrl = true,"
      " nodeJsCompatV2 = false,"
      " globalFetchStrictlyPublic = false,"
      " newModuleRegistry = false,"
      " cacheOptionEnabled = false,"
      " kvDirectBinding = false,"
      " allowCustomPorts = true,"
      " increaseWebsocketMessageSize = false,"
      " internalWritableStreamAbortClearsQueue = true,"
      " pythonWorkersDevPyodide = false,"
      " nodeJsZlib = false)",
      {}, CompatibilityDateValidation::FUTURE_FOR_TEST, false, false);
}

KJ_TEST("encode to flag list for FL") {
  capnp::MallocMessageBuilder message;
  auto orphanage = message.getOrphanage();

  auto compileOwnFeatureFlags =
      [&](kj::StringPtr compatDate, kj::ArrayPtr<const kj::StringPtr> featureFlags,
          CompatibilityDateValidation dateValidation = CompatibilityDateValidation::FUTURE_FOR_TEST,
          bool experimental = false) {
    auto flagListOrphan = orphanage.newOrphan<capnp::List<capnp::Text>>(featureFlags.size());
    auto flagList = flagListOrphan.get();
    for (auto i: kj::indices(featureFlags)) {
      flagList.set(i, featureFlags.begin()[i]);
    }

    auto outputOrphan = orphanage.newOrphan<CompatibilityFlags>();
    auto output = outputOrphan.get();

    SimpleWorkerErrorReporter errorReporter;

    compileCompatibilityFlags(
        compatDate, flagList.asReader(), output, errorReporter, experimental, dateValidation);
    KJ_ASSERT(errorReporter.errors.empty());

    return kj::mv(outputOrphan);
  };

  {
    // Disabled by date.
    auto featureFlagsOrphan = compileOwnFeatureFlags("2021-05-17", {});
    auto featureFlags = featureFlagsOrphan.get();
    auto strings = decompileCompatibilityFlagsForFl(featureFlags);
    KJ_EXPECT(strings.size() == 0);
  }

  {
    // Disabled by date, enabled by flag.
    auto featureFlagsOrphan = compileOwnFeatureFlags("2021-05-17", {"minimal_subrequests"_kj});
    auto featureFlags = featureFlagsOrphan.get();
    auto strings = decompileCompatibilityFlagsForFl(featureFlags);
    KJ_EXPECT(strings.size() == 1);
    KJ_EXPECT(strings[0] == "minimal_subrequests"_kj);
  }

  {
    // Enabled by date.
    auto featureFlagsOrphan = compileOwnFeatureFlags("2022-07-01", {});
    auto featureFlags = featureFlagsOrphan.get();
    auto strings = decompileCompatibilityFlagsForFl(featureFlags);
    KJ_EXPECT(strings.size() == 2);
    KJ_EXPECT(strings[0] == "minimal_subrequests"_kj);
    KJ_EXPECT(strings[1] == "no_cots_on_external_fetch"_kj);
  }

  {
    // Enabled by date, disabled by flag.
    auto featureFlagsOrphan = compileOwnFeatureFlags("2022-07-01", {"cots_on_external_fetch"});
    auto featureFlags = featureFlagsOrphan.get();
    auto strings = decompileCompatibilityFlagsForFl(featureFlags);
    KJ_EXPECT(strings.size() == 1);
    KJ_EXPECT(strings[0] == "minimal_subrequests"_kj);
  }
}

KJ_TEST("compatibility dates must be Tuesday, Wednesday, or Thursday") {
  // List of specific flags that are allowed to use non-conformant dates
  // (already deployed and can't be changed for compatibility reasons)
  kj::HashSet<kj::StringPtr> allowedFlagExceptions;
  allowedFlagExceptions.insertAll(std::initializer_list<kj::StringPtr>{
    // Existing non-conformant dates that are already deployed
    "jsgPropertyOnPrototypeTemplate"_kj,           // 2022-01-31 (Monday)
    "specCompliantUrl"_kj,                         // 2022-10-31 (Monday)
    "globalNavigator"_kj,                          // 2022-03-21 (Monday)
    "captureThrowsAsRejections"_kj,                // 2022-10-31 (Monday)
    "exportCommonJsDefaultNamespace"_kj,           // 2022-10-31 (Monday)
    "urlSearchParamsDeleteHasValueArg"_kj,         // 2023-07-01 (Saturday)
    "brotliContentEncoding"_kj,                    // 2024-04-29 (Monday)
    "cryptoPreservePublicExponent"_kj,             // 2023-12-01 (Friday)
    "noImportScripts"_kj,                          // 2024-03-04 (Monday)
    "queuesJsonMessages"_kj,                       // 2024-03-18 (Monday)
    "unwrapCustomThenables"_kj,                    // 2024-04-01 (Monday)
    "internalStreamByobReturn"_kj,                 // 2024-05-13 (Monday)
    "blobStandardMimeType"_kj,                     // 2024-06-03 (Monday)
    "fetchStandardUrl"_kj,                         // 2024-06-03 (Monday)
    "cacheOptionEnabled"_kj,                       // 2024-11-11 (Monday)
    "allowCustomPorts"_kj,                         // 2024-09-02 (Monday)
    "internalWritableStreamAbortClearsQueue"_kj,   // 2024-09-02 (Monday)
    "handleCrossRequestPromiseResolution"_kj,      // 2024-10-14 (Monday)
    "upperCaseAllHttpMethods"_kj,                  // 2024-10-14 (Monday)
    "noTopLevelAwaitInRequire"_kj,                 // 2024-12-02 (Monday)
    "fixupTransformStreamBackpressure"_kj,         // 2024-12-16 (Monday)
    "obsolete74"_kj,                               // 2025-03-10 (Monday)
    "cacheApiRequestCfOverridesCacheRules"_kj,     // 2025-05-19 (Monday)
    "cacheApiCompatFlags"_kj,                      // 2025-04-19 (Saturday)
    "jsWeakRef"_kj,                                // 2025-05-05 (Monday)
    "enableNavigatorLanguage"_kj,                  // 2025-05-19 (Monday)
    "allowEvalDuringStartup"_kj,                   // 2025-06-01 (Sunday)
    "bindAsyncLocalStorageSnapshot"_kj,            // 2025-06-16 (Monday)
    "throwOnUnrecognizedImportAssertion"_kj,       // 2025-06-16 (Monday)
    "setEventTargetThis"_kj,                       // 2025-08-01 (Friday)
    "enableForwardableEmailFullHeaders"_kj,        // 2025-08-01 (Friday)
    "exposeGlobalMessageChannel"_kj,               // 2025-08-15 (Friday)
    "pythonWorkersForceNewVendorPath"_kj,          // 2025-08-11 (Monday)
    "enableWorkflowScriptValidation"_kj,           // 2025-09-20 (Saturday)
    "stripAuthorizationOnCrossOriginRedirect"_kj,  // 2025-09-01 (Monday)
    "enableCtxExports"_kj,                         // 2025-11-17 (Monday)

    // Non-conformant dates via impliedByAfterDate
    "pythonWorkers"_kj,                  // 2000-01-01 (Saturday) via impliedByAfterDate
    "nodeJsCompatV2"_kj,                 // 2024-09-23 (Monday) via impliedByAfterDate
    "nodeJsZlib"_kj,                     // 2024-09-23 (Monday) via impliedByAfterDate
    "enableNodejsHttpModules"_kj,        // 2025-08-15 (Friday) via impliedByAfterDate
    "enableNodejsHttpServerModules"_kj,  // 2025-09-01 (Monday) via impliedByAfterDate
    "removeNodejsCompatEOL"_kj,          // 2025-09-01 (Monday) via impliedByAfterDate
    "enableNodeJsHttp2Module"_kj,        // 2025-09-01 (Monday) via impliedByAfterDate
    "removeNodejsCompatEOLv23"_kj,       // 2025-09-01 (Monday) via impliedByAfterDate
    "enableNodeJsProcessV2"_kj,          // 2025-09-15 (Monday) via impliedByAfterDate
    "enableNodeJsFsModule"_kj,           // 2025-09-15 (Monday) via impliedByAfterDate
    "enableNodeJsOsModule"_kj,           // 2025-09-15 (Monday) via impliedByAfterDate
    "pythonWorkflows"_kj,                // 2025-09-20 (Saturday) via impliedByAfterDate
    "enableNodeJsConsoleModule"_kj,      // 2025-09-21 (Sunday) via impliedByAfterDate
    "pythonWorkers20250116"_kj,          // 2025-09-29 (Monday) via impliedByAfterDate
    "removeNodejsCompatEOLv22"_kj,       // 2027-04-30 (Friday) via impliedByAfterDate
    "removeNodejsCompatEOLv24"_kj,       // 2028-04-30 (Sunday) via impliedByAfterDate
  });

  // Helper function to suggest the next valid date (Tuesday, Wednesday, or Thursday)
  auto suggestNextValidDate =
      [](const std::chrono::year_month_day& ymd) -> std::chrono::year_month_day {
    auto currentDate = std::chrono::sys_days{ymd};
    auto wd = std::chrono::weekday{currentDate};

    // Calculate days until next Tuesday (2), Wednesday (3), or Thursday (4)
    int currentDay = wd.c_encoding();  // 0 = Sunday, 1 = Monday, etc.
    int daysToAdd;

    if (currentDay <= 1) {
      // Sunday (0) or Monday (1): next valid day is Tuesday
      daysToAdd = 2 - currentDay;
    } else if (currentDay >= 5) {
      // Friday (5) or Saturday (6): next valid day is Tuesday of next week
      daysToAdd = (7 - currentDay) + 2;
    } else {
      // Already Tuesday/Wednesday/Thursday - this shouldn't happen
      daysToAdd = 0;
    }

    return std::chrono::year_month_day{currentDate + std::chrono::days{daysToAdd}};
  };

  // Helper function to parse and validate date
  auto parseDate = [](kj::StringPtr dateStr) -> kj::Maybe<std::chrono::year_month_day> {
    // First validate the date using normalizeCompatDate
    KJ_IF_SOME(normalized, normalizeCompatDate(dateStr)) {
      // Parse the validated date string
      int year, month, day;
      KJ_ASSERT(sscanf(normalized.cStr(), "%d-%d-%d", &year, &month, &day) == 3);

      // Create year_month_day
      auto ymd = std::chrono::year_month_day{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)},
      };
      KJ_ASSERT(ymd.ok());

      return ymd;
    } else {
      return kj::none;
    }
  };

  // Check all compatibility flag fields
  auto schema = capnp::Schema::from<CompatibilityFlags>();
  auto fields = schema.getFields();

  kj::Vector<kj::String> violations;

  for (auto field: fields) {
    auto fieldName = field.getProto().getName();

    // Skip if this specific flag is in the allowed exceptions list
    if (allowedFlagExceptions.contains(fieldName)) {
      continue;
    }

    for (auto annotation: field.getProto().getAnnotations()) {
      kj::Maybe<kj::StringPtr> maybeDateStr;

      if (annotation.getId() == COMPAT_ENABLE_DATE_ANNOTATION_ID) {
        maybeDateStr = annotation.getValue().getText();
      } else if (annotation.getId() == IMPLIED_BY_AFTER_DATE_ANNOTATION_ID) {
        auto value = annotation.getValue();
        auto s = value.getStruct().getAs<workerd::ImpliedByAfterDate>();
        maybeDateStr = s.getDate();
      }

      KJ_IF_SOME(dateStr, maybeDateStr) {
        auto ymd = KJ_REQUIRE_NONNULL(
            parseDate(dateStr), "Invalid compatibility flag date format: ", dateStr);
        auto suggestedYmd = suggestNextValidDate(ymd);

        // If suggestNextValidDate returns a different date, the original date was invalid
        if (ymd != suggestedYmd) {
          static const char* dayNames[] = {
            "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};

          auto wd = std::chrono::weekday{std::chrono::sys_days{ymd}};
          int dayOfWeek = wd.c_encoding();

          auto suggestedWd = std::chrono::weekday{std::chrono::sys_days{suggestedYmd}};
          int suggestedDayOfWeek = suggestedWd.c_encoding();

          auto suggestedDateStr = std::format("{:%F}", suggestedYmd);
          violations.add(kj::str("Field '", fieldName, "' has date ", dateStr, " which is a ",
              dayNames[dayOfWeek], ". Dates must be Tuesday, Wednesday, or Thursday. ",
              "Suggestion: use ", suggestedDateStr.c_str(), " (", dayNames[suggestedDayOfWeek],
              ") instead."));
        }
      }
    }
  }

  if (!violations.empty()) {
    KJ_FAIL_ASSERT("Compatibility date violations found:\n", kj::strArray(violations, "\n"));
  }
}

}  // namespace
}  // namespace workerd
