// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the encoding streams suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export {
  encoderEncoding,
  toStringTagBranding,
  transformStreamInheritance,
  accessorPlacement,
  sidesAreStableStreamInstances,
  prototypeAccessorBrandChecks,
  constructorSurface,
} from 'api-surface';

export {
  decoderOptionsReflection,
  fatalDefaults,
  invalidLabelThrows,
  labelNormalization,
} from 'construction';

export { encoderCoercesChunksToString } from 'encode-coercion';

export {
  decoderAcceptsBufferSources,
  decoderDetachedBufferIsNoop,
  decoderRejectsNonBufferSource,
  encoderSymbolChunkErrorsStream,
} from 'chunk-types';

export { big5StreamingDecode } from 'decode-non-utf8';

export { encoderDecoderPipeline } from 'pipe-integration';
