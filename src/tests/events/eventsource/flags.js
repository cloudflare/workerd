// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The compat flags the suite branches on.

const flags = globalThis.Cloudflare.compatibilityFlags;

// The streams an EventSource reads are the TypeScript implementation's.
export const usingTsStreams = flags['typescript_implemented_streams'] === true;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// WebIDL names and lengths for functions, non-configurable constants, and
// globals as own properties of globalThis.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;
