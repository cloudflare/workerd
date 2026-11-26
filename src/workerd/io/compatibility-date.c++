// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Under the RUST_COMPAT_DATE autogate the compiler is Rust (`compat-date/`), and this file
// adapts its bridge to KJ types; otherwise it is the C++ one in compatibility-date-legacy.c++.

#include "compatibility-date.h"

#include <workerd/io/compat-date/bridge.rs.h>
#include <workerd/io/compatibility-date-legacy.h>
#include <workerd/util/autogate.h>

#include <kj-rs/convert.h>

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/schema.h>
#include <kj/debug.h>

namespace workerd {

namespace ffi = rust::compat_date;

namespace {

::rust::Slice<const uint8_t> bytesOf(kj::StringPtr s) {
  return {reinterpret_cast<const uint8_t*>(s.begin()), s.size()};
}

// A list as the bridge takes one: a view of each string, which the list still owns.
template <typename T>
kj::Array<ffi::Bytes> bytesOfEach(const T& strings) {
  return KJ_MAP(s, strings) { return ffi::Bytes{bytesOf(s)}; };
}

ffi::DateValidation toFfi(CompatibilityDateValidation v) {
  switch (v) {
    case CompatibilityDateValidation::CODE_VERSION:
      return ffi::DateValidation::CodeVersion;
    case CompatibilityDateValidation::CURRENT_DATE_FOR_CLOUDFLARE:
      return ffi::DateValidation::CurrentDateForCloudflare;
    case CompatibilityDateValidation::FUTURE_FOR_TEST:
      return ffi::DateValidation::FutureForTest;
  }
  KJ_UNREACHABLE;
}

void compileFlags(kj::StringPtr compatDate,
    kj::ArrayPtr<const ffi::Bytes> flags,
    CompatibilityFlags::Builder output,
    ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation,
    kj::ArrayPtr<const kj::StringPtr> allowedExperimentalFlags,
    MainModuleIsPython mainModuleIsPython) {
  auto allowed = bytesOfEach(allowedExperimentalFlags);
  auto result = ffi::compile_compatibility_flags(bytesOf(compatDate), {flags.begin(), flags.size()},
      {allowed.begin(), allowed.size()},
      ffi::CompileOptions{
        .date_validation = toFfi(dateValidation),
        .allow_experimental = allowExperimentalFeatures,
        .main_module_is_python = mainModuleIsPython.toBool(),
      });

  auto fields = capnp::Schema::from<CompatibilityFlags>().getFields();
  KJ_ASSERT(result.enabled.size() == fields.size());
  auto dynamicOutput = capnp::toDynamic(output);
  for (auto i: kj::indices(fields)) {
    dynamicOutput.set(fields[i], result.enabled[i]);
  }

  for (auto& diagnostic: result.diagnostics) {
    auto message = kj::heapString(
        reinterpret_cast<const char*>(diagnostic.message.data()), diagnostic.message.size());
    if (diagnostic.kind == ffi::DiagnosticKind::Warning) {
      errorReporter.addWarning(kj::mv(message));
    } else {
      errorReporter.addError(kj::mv(message));
    }
  }
}

// The field's $compatEnableFlag, which lives in the schema, and so outlives every caller.
kj::StringPtr enableFlagName(capnp::StructSchema::Field field) {
  for (auto annotation: field.getProto().getAnnotations()) {
    if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
      return annotation.getValue().getText();
    }
  }
  KJ_FAIL_ASSERT("decompiled a field without a $compatEnableFlag", field.getProto().getName());
}

kj::Array<kj::StringPtr> decompileFlags(CompatibilityFlags::Reader input, ffi::Decompile which) {
  auto canonical = capnp::canonicalize(input);
  auto bytes = canonical.asBytes();
  auto indices = ffi::decompile_compatibility_flags({bytes.begin(), bytes.size()}, which);
  auto fields = capnp::Schema::from<CompatibilityFlags>().getFields();
  auto names = kj::heapArrayBuilder<kj::StringPtr>(indices.size());
  for (auto index: indices) {
    names.add(enableFlagName(fields[index]));
  }
  return names.finish();
}

bool useRust() {
  return util::Autogate::isEnabled(util::AutogateKey::RUST_COMPAT_DATE);
}

}  // namespace

kj::String currentDateStr() {
  if (!useRust()) return compat_date_legacy::currentDateStr();
  return kj::str(ffi::current_date_str());
}

kj::String newestCompatDateStr() {
  if (!useRust()) return compat_date_legacy::newestCompatDateStr();
  return kj::str(ffi::newest_compat_date_str());
}

void compileCompatibilityFlags(kj::StringPtr compatDate,
    capnp::List<capnp::Text>::Reader compatFlags,
    CompatibilityFlags::Builder output,
    ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation,
    kj::ArrayPtr<const kj::StringPtr> allowedExperimentalFlags,
    MainModuleIsPython mainModuleIsPython) {
  if (!useRust()) {
    return compat_date_legacy::compileCompatibilityFlags(compatDate, compatFlags, output,
        errorReporter, allowExperimentalFeatures, dateValidation, allowedExperimentalFlags,
        mainModuleIsPython);
  }
  compileFlags(compatDate, bytesOfEach(compatFlags), output, errorReporter,
      allowExperimentalFeatures, dateValidation, allowedExperimentalFlags, mainModuleIsPython);
}

void compileCompatibilityFlags(kj::StringPtr compatDate,
    kj::ArrayPtr<const kj::String> compatFlags,
    CompatibilityFlags::Builder output,
    ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation,
    kj::ArrayPtr<const kj::StringPtr> allowedExperimentalFlags,
    MainModuleIsPython mainModuleIsPython) {
  if (!useRust()) {
    return compat_date_legacy::compileCompatibilityFlags(compatDate, compatFlags, output,
        errorReporter, allowExperimentalFeatures, dateValidation, allowedExperimentalFlags,
        mainModuleIsPython);
  }
  compileFlags(compatDate, bytesOfEach(compatFlags), output, errorReporter,
      allowExperimentalFeatures, dateValidation, allowedExperimentalFlags, mainModuleIsPython);
}

kj::Array<kj::StringPtr> decompileCompatibilityFlagsForFl(CompatibilityFlags::Reader input) {
  if (!useRust()) return compat_date_legacy::decompileCompatibilityFlagsForFl(input);
  return decompileFlags(input, ffi::Decompile::NeededByFl);
}

kj::Array<kj::StringPtr> decompileCompatibilityFlags(CompatibilityFlags::Reader input) {
  if (!useRust()) return compat_date_legacy::decompileCompatibilityFlags(input);
  return decompileFlags(input, ffi::Decompile::All);
}

kj::Maybe<kj::String> normalizeCompatDate(kj::StringPtr date) {
  if (!useRust()) return compat_date_legacy::normalizeCompatDate(date);
  auto normalized = ffi::normalize_compat_date(bytesOf(date));
  if (normalized.empty()) return kj::none;
  return kj::str(normalized);
}

}  // namespace workerd
