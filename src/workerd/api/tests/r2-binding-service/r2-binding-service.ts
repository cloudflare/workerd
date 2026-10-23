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
// Arguments are what the backend receives, which is not always what the caller passed to the
// public R2Bucket method: workerd validates and normalizes options first. Absent option fields may
// arrive as explicit `undefined`.
//
// Results are the native R2 types, so a backend can return what a native R2 binding returns.
// Two constraints the types cannot express: an R2ObjectBody is rejected wherever only object
// metadata is expected, and a native R2Object returned from head, get, put, or
// completeMultipartUpload must include httpMetadata and customMetadata.

// Request body passed to put and uploadPart. ArrayBufferViews arrive as ArrayBuffers, and
// binary, string, and Blob values above an inline size limit arrive as a ReadableStream. Known
// stream lengths are preserved across JSRPC.
export type R2BindingBody = ReadableStream | ArrayBuffer | string | Blob;

// Range passed to get. Contains only the fields the caller set, so it may be empty.
export type R2BindingRange = {
  offset?: number;
  length?: number;
  suffix?: number;
};

// Options for get. `range` is omitted for Headers without a Range header. `ssecKey` is always
// hex-encoded.
export type R2BindingGetOptions = {
  onlyIf?: R2Conditional | Headers | undefined;
  range?: R2BindingRange | Headers | undefined;
  ssecKey?: string | undefined;
};

// Options for put. `httpMetadata` is always an object rather than Headers, and `ssecKey` is
// always hex-encoded. Checksums are forwarded as the caller passed them.
export type R2BindingPutOptions = {
  onlyIf?: R2Conditional | Headers | undefined;
  httpMetadata?: R2HTTPMetadata | undefined;
  customMetadata?: Record<string, string> | undefined;
  md5?: ArrayBuffer | ArrayBufferView | string | undefined;
  sha1?: ArrayBuffer | ArrayBufferView | string | undefined;
  sha256?: ArrayBuffer | ArrayBufferView | string | undefined;
  sha384?: ArrayBuffer | ArrayBufferView | string | undefined;
  sha512?: ArrayBuffer | ArrayBufferView | string | undefined;
  storageClass?: string | undefined;
  ssecKey?: string | undefined;
};

// Options for list. Without the r2_list_honor_include compatibility flag, `include` always
// requests both kinds of metadata, whatever the caller asked for.
export type R2BindingListOptions = {
  limit?: number | undefined;
  prefix?: string | undefined;
  cursor?: string | undefined;
  delimiter?: string | undefined;
  startAfter?: string | undefined;
  include?: ('httpMetadata' | 'customMetadata')[] | undefined;
};

// Options for createMultipartUpload. `httpMetadata` is always an object rather than Headers,
// and `ssecKey` is always hex-encoded.
export type R2BindingMultipartOptions = {
  httpMetadata?: R2HTTPMetadata | undefined;
  customMetadata?: Record<string, string> | undefined;
  storageClass?: string | undefined;
  ssecKey?: string | undefined;
};

// Options for uploadPart. `ssecKey` is always hex-encoded.
export type R2BindingUploadPartOptions = {
  ssecKey?: string | undefined;
};

export type R2BindingService = {
  head(key: string): Promise<R2Object | null>;

  // Returns object metadata without a body when an onlyIf precondition fails.
  get(
    key: string,
    options: R2BindingGetOptions | undefined
  ): Promise<R2ObjectBody | R2Object | null>;

  put(
    key: string,
    value: R2BindingBody | null,
    options: R2BindingPutOptions | undefined
  ): Promise<R2Object | null>;

  // The result is ignored.
  delete(keys: string | string[]): Promise<void>;

  list(options: R2BindingListOptions | undefined): Promise<R2Objects>;

  createMultipartUpload(
    key: string,
    options: R2BindingMultipartOptions | undefined
  ): Promise<{ uploadId: string }>;

  // The returned partNumber is ignored in favour of the requested one, but must be present.
  uploadPart(
    key: string,
    uploadId: string,
    partNumber: number,
    value: R2BindingBody,
    options: R2BindingUploadPartOptions | undefined
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
