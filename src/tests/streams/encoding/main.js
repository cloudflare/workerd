// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Entry point for the encoding streams suite. Re-exports are explicit (not
// `export *`) so a name collision between modules is a load-time
// SyntaxError instead of a silently dropped test.

export { encoderEncoding, decoderOptionsReflection } from 'api-surface';

export { encoderCoercesChunksToString } from 'encode-coercion';

export { big5StreamingDecode } from 'decode-non-utf8';

export { encoderDecoderPipeline } from 'pipe-integration';
