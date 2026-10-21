// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

import { WorkerEntrypoint } from 'cloudflare:workers';
import type {
  R2BindingBody,
  R2BindingGetOptions,
  R2BindingListOptions,
  R2BindingMultipartOptions,
  R2BindingPutOptions,
  R2BindingService,
  R2BindingUploadPartOptions,
} from './r2-binding-service';

type Env = {
  REAL_BUCKET: R2Bucket;
};

// JSRPC bodies can arrive as a stream whose length is known only from the separate size argument.
// The native binding requires a known-length stream, so restore it from that argument.
function withKnownLength<T extends R2BindingBody | null>(
  value: T,
  valueSize: number
): T | ReadableStream {
  if (!(value instanceof ReadableStream)) {
    return value;
  }
  const { readable, writable } = new FixedLengthStream(valueSize);
  value.pipeTo(writable).catch(() => {});
  return readable;
}

// Implements the R2 binding's JSRPC methods by forwarding each call to a native R2 binding, in
// the way a customer Worker can stand in for R2. The JSRPC tests target this entrypoint so the
// shared suite exercises the serialization of native R2 results and errors, while REAL_BUCKET
// reaches the same HTTP mock as the HTTP tests. HTTP requests are rejected to detect any
// transport fallback on the JSRPC hop.
export class ObservedR2Binding
  extends WorkerEntrypoint<Env>
  implements R2BindingService
{
  override fetch(): Response {
    throw new Error('R2 JSRPC tests must not use HTTP');
  }

  head(key: string): Promise<R2Object | null> {
    return this.env.REAL_BUCKET.head(key);
  }

  get(
    key: string,
    options: R2BindingGetOptions | undefined
  ): Promise<R2ObjectBody | R2Object | null> {
    // The native binding accepts a range with no fields set, which R2Range does not model.
    return this.env.REAL_BUCKET.get(key, options as R2GetOptions | undefined);
  }

  put(
    key: string,
    value: R2BindingBody | null,
    options: R2BindingPutOptions | undefined,
    valueSize: number
  ): Promise<R2Object | null> {
    return this.env.REAL_BUCKET.put(
      key,
      withKnownLength(value, valueSize),
      options
    );
  }

  delete(keys: string | string[]): Promise<void> {
    return this.env.REAL_BUCKET.delete(keys);
  }

  list(options: R2BindingListOptions | undefined): Promise<R2Objects> {
    return this.env.REAL_BUCKET.list(options);
  }

  async createMultipartUpload(
    key: string,
    options: R2BindingMultipartOptions | undefined
  ): Promise<{ uploadId: string }> {
    const upload = await this.env.REAL_BUCKET.createMultipartUpload(
      key,
      options
    );
    return { uploadId: upload.uploadId };
  }

  uploadPart(
    key: string,
    uploadId: string,
    partNumber: number,
    value: R2BindingBody,
    options: R2BindingUploadPartOptions | undefined,
    valueSize: number
  ): Promise<R2UploadedPart> {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).uploadPart(
      partNumber,
      withKnownLength(value, valueSize),
      options
    );
  }

  abortMultipartUpload(key: string, uploadId: string): Promise<void> {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).abort();
  }

  completeMultipartUpload(
    key: string,
    uploadId: string,
    uploadedParts: R2UploadedPart[]
  ): Promise<R2Object> {
    return this.env.REAL_BUCKET.resumeMultipartUpload(key, uploadId).complete(
      uploadedParts
    );
  }
}
