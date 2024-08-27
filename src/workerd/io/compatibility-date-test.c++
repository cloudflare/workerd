// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "compatibility-date.h"
#include <kj/debug.h>
#include <kj/test.h>
#include <capnp/message.h>
#include <capnp/serialize-text.h>

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
       "2021-11-03 so does not need to be specified anymore."});
  expectCompileCompatibilityFlags(
      "2021-05-17", {"unknown_feature"_kj}, "()", {"No such compatibility flag: unknown_feature"});

  expectCompileCompatibilityFlags("2252-04-01", {}, "()",
      {"Can't set compatibility date in the future: 2252-04-01"},
      CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE);

  expectCompileCompatibilityFlags("2252-04-01", {}, "()",
      {kj::str("This Worker requires compatibility date \"2252-04-01\", but the newest date "
               "supported by this server binary is \"",
          SUPPORTED_COMPATIBILITY_DATE, "\".")},
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
      {"Invalid compatibility date: abcd",
        "Compatibility flag specified multiple times: fetch_refuses_unknown_protocols",
        "Compatibility flag specified multiple times: formdata_parser_supports_files",
        "No such compatibility flag: another_feature",
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
      " webgpu = false,"
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
      " internalWritableStreamAbortClearsQueue = true)",
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
      " webgpu = false,"
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

}  // namespace
}  // namespace workerd
