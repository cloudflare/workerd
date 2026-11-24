// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which implementation this cell runs, and the compat flags the suite
// branches on. The implementation is selected by an autogate, which user
// code cannot read, so each cell says which one it is meant to run through
// its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;

// The enable-flags the cell's config names, which are therefore set whatever
// the variant's date.
export const namedFlags = env.NAMED_FLAGS;

const flags = globalThis.Cloudflare.compatibilityFlags;

// The prototype of the Cloudflare object has a Symbol.toStringTag.
export const hasToStringTag = flags['set_tostring_tag'] === true;

// The `prototype` of the Cloudflare object's constructor is read-only.
export const immutablePrototypes = flags['immutable_api_prototypes'] === true;
