// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which implementation this cell runs, and the compat flags the suite
// branches on. The implementation is selected by the TS_URL autogate, which
// user code cannot read, so each cell says which one it is meant to run
// through its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;

const flags = globalThis.Cloudflare.compatibilityFlags;

// The ada-backed parser; otherwise the legacy kj::Url one.
export const urlStandard = flags['url_standard'] === true;

// URLSearchParams delete() and has() take an optional value argument.
export const deleteHasValueArg =
  flags['urlsearchparams_delete_has_value_arg'] === true;

// The legacy URL's attributes are prototype accessors rather than own
// properties of each instance.
export const gettersSettersOnPrototype =
  flags['workers_api_getters_setters_on_prototype'] === true;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// JSG makes JSG_STRUCT values, the C++ iterators' results among them, from
// a template of own data properties rather than by assignment.
export const fastJsgStruct = flags['enable_fast_jsg_struct'] === true;

// WebIDL names and lengths for accessor functions; globals as own
// properties of globalThis.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;

// Constructors' `prototype` is non-writable.
export const immutableApiPrototypes =
  flags['immutable_api_prototypes'] === true;

// Whether the C++ already matches WebIDL for the property attributes the
// opt-in flags control. The TypeScript implementation always does.
export const webIdlAttributes = usingTsImpl || specCompliantPropertyAttributes;
export const nonWritablePrototype = usingTsImpl || immutableApiPrototypes;
