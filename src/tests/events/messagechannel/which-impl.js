// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Which implementation this cell runs, and the compat flags the suite
// branches on. The implementation is selected by the TS_MESSAGE_CHANNEL
// autogate, which user code cannot read, so each cell says which one it is
// meant to run through its USING_TS_IMPL binding.
import { env } from 'cloudflare:workers';

export const usingTsImpl = env.USING_TS_IMPL === true;

const flags = globalThis.Cloudflare.compatibilityFlags;

// Listener exceptions are reported, and the port dispatches 'messageerror'.
export const specCompliantDispatchExceptions =
  flags['spec_compliant_dispatch_exceptions'] === true;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// WebIDL names and lengths for accessor functions; globals as own
// properties of globalThis.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;

// Constructors' `prototype` is non-writable.
export const immutableApiPrototypes =
  flags['immutable_api_prototypes'] === true;
