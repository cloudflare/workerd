// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/////////////////////////////////////////////////////////////////////////////
// Binding contract to create a custom binding                             //
/////////////////////////////////////////////////////////////////////////////
//
// With the r2_binding_jsrpc compatibility flag, an R2 bucket binding that targets a Worker
// entrypoint calls these named JSRPC methods on it instead of speaking the R2 HTTP protocol.
// R2Bucket and R2MultipartUpload in src/workerd/api/r2-bucket.c++ and r2-multipart.c++ are the
// callers; this file describes what they send and what they accept back.
//
// The backend receives the public R2 option types without client-side validation or normalization.
// It is responsible for validating them. The only compatibility adjustment is to list(): callers
// without r2_list_honor_include request both metadata fields when their include list is absent or
// contains only recognized values.
//
// Results are the native R2 types, so a backend can return what a native R2 binding returns.
// Two constraints the types cannot express: an R2ObjectBody is rejected wherever only object
// metadata is expected, and a native R2Object returned from head, get, put, or
// completeMultipartUpload must include httpMetadata and customMetadata.

// Request body passed to put and uploadPart. ArrayBufferViews arrive as ArrayBuffers, and
// binary, string, and Blob values above an inline size limit arrive as a ReadableStream. Known
// stream lengths are preserved across JSRPC.
export type R2BindingBody = ReadableStream | ArrayBuffer | string | Blob;

export type R2BindingService = {
  head(key: string): Promise<R2Object | null>;

  // Returns object metadata without a body when an onlyIf precondition fails.
  get(
    key: string,
    options: R2GetOptions | undefined
  ): Promise<R2ObjectBody | R2Object | null>;

  put(
    key: string,
    value: R2BindingBody | null,
    options: R2PutOptions | undefined
  ): Promise<R2Object | null>;

  // The result is ignored.
  delete(keys: string | string[]): Promise<void>;

  list(options: R2ListOptions | undefined): Promise<R2Objects>;

  createMultipartUpload(
    key: string,
    options: R2MultipartOptions | undefined
  ): Promise<{ uploadId: string }>;

  // The returned partNumber is ignored in favour of the requested one, but must be present.
  uploadPart(
    key: string,
    uploadId: string,
    partNumber: number,
    value: R2BindingBody,
    options: R2UploadPartOptions | undefined
  ): Promise<R2UploadedPart>;

  abortMultipartUpload(key: string, uploadId: string): Promise<void>;

  completeMultipartUpload(
    key: string,
    uploadId: string,
    uploadedParts: R2UploadedPart[]
  ): Promise<R2Object>;
};

/////////////////////////////////////////////////////////////////////////////
// End of the binding contract                                             //
/////////////////////////////////////////////////////////////////////////////
