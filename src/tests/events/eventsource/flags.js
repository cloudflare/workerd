// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// The streams implementation and compat flags the suite branches on.

import { env } from 'cloudflare:workers';

const flags = globalThis.Cloudflare.compatibilityFlags;

// The streams an EventSource reads are the TypeScript implementation's. The
// TS_WEB_STREAMS autogate selects them, and user code cannot read autogates,
// so each cell declares the implementation through its USING_TS_IMPL binding.
export const usingTsStreams = env.USING_TS_IMPL === true;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// WebIDL names and lengths for functions, non-configurable constants, and
// globals as own properties of globalThis.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;

// on<type> event handlers and listeners are called with the target as `this`.
export const setEventTargetThis = flags['set_event_target_this'] === true;
