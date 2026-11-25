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

// The prototype of the scheduler has a Symbol.toStringTag.
export const hasToStringTag = flags['set_tostring_tag'] === true;

// The `prototype` of the scheduler's constructor is read-only.
export const immutablePrototypes = flags['immutable_api_prototypes'] === true;

// A method's `length` counts its required arguments; without the flag it is
// 0.
export const specCompliantAttributes =
  flags['spec_compliant_property_attributes'] === true;

// A failure of wait() past its receiver check rejects its promise; without
// the flag it is thrown.
export const capturesThrows = flags['capture_async_api_throws'] === true;
