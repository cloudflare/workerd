// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which implementation this cell runs, and the compat flag the suite
// branches on. The implementation is selected by an autogate, which user
// code cannot read, so each cell says which one it is meant to run through
// its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;

// Function lengths count the required arguments.
export const specCompliantPropertyAttributes =
  globalThis.Cloudflare.compatibilityFlags[
    'spec_compliant_property_attributes'
  ] === true;
