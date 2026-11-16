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

// Attributes are prototype accessors rather than own data properties of
// each instance.
export const onPrototype =
  flags['workers_api_getters_setters_on_prototype'] === true;

// Prototypes carry Symbol.toStringTag.
export const setToStringTag = flags['set_tostring_tag'] === true;

// Body.blob() normalizes the content type it takes the Blob's type from.
export const blobStandardMimeType = flags['blob_standard_mime_type'] === true;

// Blobs reject resizable and growable buffers.
export const rejectResizable =
  flags['no_resizable_array_buffer_in_blob'] === true;

// Globals as own properties of globalThis, non-enumerable, as well.
export const specCompliantPropertyAttributes =
  flags['spec_compliant_property_attributes'] === true;

// Constructors' `prototype` is non-writable.
export const immutableApiPrototypes =
  flags['immutable_api_prototypes'] === true;
