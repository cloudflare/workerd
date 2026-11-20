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

// navigator.language and navigator.languages exist.
export const hasLanguage = flags['enable_navigator_language'] === true;

// navigator.storage exists.
export const hasStorage = flags['enable_web_file_system'] === true;

// Navigator.prototype has a Symbol.toStringTag.
export const hasToStringTag = flags['set_tostring_tag'] === true;

// Navigator.prototype (the property of the constructor) is read-only.
export const immutablePrototypes = flags['immutable_api_prototypes'] === true;

// Function lengths count the required arguments, and the constructor is
// also an own property of the global.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;
