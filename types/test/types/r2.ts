// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { expectTypeOf } from 'expect-type';

// `ssecKey` is unwrapped from any 32-byte ArrayBuffer or ArrayBufferView, or a
// 64-character hex string, like the checksum fields declared next to it.
type SsecKey = ArrayBuffer | ArrayBufferView | string | undefined;
expectTypeOf<R2GetOptions['ssecKey']>().toEqualTypeOf<SsecKey>();
expectTypeOf<R2PutOptions['ssecKey']>().toEqualTypeOf<SsecKey>();
expectTypeOf<R2MultipartOptions['ssecKey']>().toEqualTypeOf<SsecKey>();
expectTypeOf<R2UploadPartOptions['ssecKey']>().toEqualTypeOf<SsecKey>();

declare const bucket: R2Bucket;
declare const upload: R2MultipartUpload;
const ssecKey = new Uint8Array(32);
void bucket.get('key', { ssecKey });
void bucket.put('key', 'value', { ssecKey });
void bucket.createMultipartUpload('key', { ssecKey });
void upload.uploadPart(1, 'value', { ssecKey });
