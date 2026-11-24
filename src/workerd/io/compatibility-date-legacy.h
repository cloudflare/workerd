// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

// The C++ compiler of compatibility flags, which compatibility-date.h uses unless the
// RUST_COMPAT_DATE autogate is on. Call compatibility-date.h instead; this exists for the autogate
// and for compatibility-date-legacy-test.c++, and goes once the gate is removed.

#include <workerd/io/compatibility-date.h>

namespace workerd::compat_date_legacy {

void compileCompatibilityFlags(kj::StringPtr compatDate,
    capnp::List<capnp::Text>::Reader compatFlags,
    CompatibilityFlags::Builder output,
    ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation,
    kj::ArrayPtr<const kj::StringPtr> allowedExperimentalFlags,
    MainModuleIsPython mainModuleIsPython = MainModuleIsPython::NO);
void compileCompatibilityFlags(kj::StringPtr compatDate,
    kj::ArrayPtr<const kj::String> compatFlags,
    CompatibilityFlags::Builder output,
    ValidationErrorReporter& errorReporter,
    bool allowExperimentalFeatures,
    CompatibilityDateValidation dateValidation,
    kj::ArrayPtr<const kj::StringPtr> allowedExperimentalFlags,
    MainModuleIsPython mainModuleIsPython = MainModuleIsPython::NO);
kj::Array<kj::StringPtr> decompileCompatibilityFlagsForFl(CompatibilityFlags::Reader input);
kj::Array<kj::StringPtr> decompileCompatibilityFlags(CompatibilityFlags::Reader input);
kj::Maybe<kj::String> normalizeCompatDate(kj::StringPtr date);
kj::String currentDateStr();
kj::String newestCompatDateStr();

}  // namespace workerd::compat_date_legacy
