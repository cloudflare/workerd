// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which implementation this cell runs, and the compat flags the suite
// branches on. The implementation is selected by an autogate, which user
// code cannot read, so each cell says which one it is meant to run through
// its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;

const flags = globalThis.Cloudflare.compatibilityFlags;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// Errors thrown over RPC are serialized rather than tunneled through their
// description.
export const enhancedErrorSerialization =
  flags['enhanced_error_serialization'] === true;

// Globals as own properties of globalThis, non-enumerable.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;

// Constructors' `prototype` is non-writable.
export const immutableApiPrototypes =
  flags['immutable_api_prototypes'] === true;

// Whether the C++ already matches WebIDL for the attributes the opt-in
// flags control. The TypeScript implementation always does.
export const webIdlAttributes = usingTsImpl || specCompliantPropertyAttributes;
export const nonWritablePrototype = usingTsImpl || immutableApiPrototypes;
