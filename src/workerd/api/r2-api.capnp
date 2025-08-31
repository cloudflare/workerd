# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xfb0dc52eec08c4d2;

using Cxx = import "/capnp/c++.capnp";
using Json = import "/capnp/compat/json.capnp";

$Cxx.namespace("workerd::api::public_beta");
$Cxx.allowCancellation;

const versionPublicBeta :UInt32 = 1;

struct R2BindingRequest {
  version @0 :UInt32;
  payload :union $Json.flatten() $Json.discriminator(name="method") {
    head @1 :R2HeadRequest $Json.flatten();
    get @2 :R2GetRequest $Json.flatten();
    put @3 :R2PutRequest $Json.flatten();
    copy @4 :R2CopyRequest $Json.flatten();
    list @5 :R2ListRequest $Json.flatten();
    delete @6 :R2DeleteRequest $Json.flatten();
    createBucket @7 :R2CreateBucketRequest $Json.flatten();
    listBucket @8 :R2ListBucketRequest $Json.flatten();
    deleteBucket @9 :R2DeleteBucketRequest $Json.flatten();
    createMultipartUpload @10 :R2CreateMultipartUploadRequest $Json.flatten();
    uploadPart @11 :R2UploadPartRequest $Json.flatten();
    uploadPartCopy @12 :R2UploadPartCopyRequest $Json.flatten();
    completeMultipartUpload @13 :R2CompleteMultipartUploadRequest $Json.flatten();
    abortMultipartUpload @14 :R2AbortMultipartUploadRequest $Json.flatten();
  }
}

struct Record {
  k @0 :Text;
  v @1 :Text;
}

struct R2Range {
  offset @0 :UInt64 = 0xffffffffffffffff;
  length @1 :UInt64 = 0xffffffffffffffff;
  suffix @2 :UInt64 = 0xffffffffffffffff;
}

struct R2Etag {
  value @0 :Text;
  type :union $Json.flatten() $Json.discriminator(name="type") {
    strong @1 :Void;
    weak @2 :Void;
    wildcard @3 :Void;
  }
}

struct R2Conditional {
  etagMatches @0 :List(R2Etag);
  etagDoesNotMatch @1 :List(R2Etag);
  uploadedBefore @2 :UInt64 = 0xffffffffffffffff;
  uploadedAfter @3 :UInt64 = 0xffffffffffffffff;
  secondsGranularity @4 :Bool;
  # Should uploadedBefore / uploadedAfter be evaluated against the seconds granularity of the upload
  # timestamp.
}

struct R2SSECOptions {
  key @0 :Text;
}

struct R2Checksums {
  # The JSON name of these fields must comform to the representation of the ChecksumAlgorithm in
  # the R2 gateway worker.
  md5 @0 :Data $Json.hex $Json.name("0");
  sha1 @1 :Data $Json.hex $Json.name("1");
  sha256 @2 :Data $Json.hex $Json.name("2");
  sha384 @3 :Data $Json.hex $Json.name("3");
  sha512 @4 :Data $Json.hex $Json.name("4");
}

struct R2PublishedPart {
  etag @0 :Text;
  part @1 :UInt32;
}

struct R2HttpFields {
  contentType @0 :Text;
  contentLanguage @1 :Text;
  contentDisposition @2 :Text;
  contentEncoding @3 :Text;
  cacheControl @4 :Text;
  cacheExpiry @5 :UInt64 = 0xffffffffffffffff;
}

struct R2HeadRequest {
  object @0 :Text;
}

struct R2GetRequest {
  object @0 :Text;
  range @1 :R2Range;
  rangeHeader @3 :Text;
  onlyIf @2 :R2Conditional;
  ssec @4 :R2SSECOptions;
}

struct R2PutRequest {
  object @0 :Text;
  customFields @1 :List(Record);
  httpFields @2 :R2HttpFields;
  onlyIf @3 :R2Conditional;
  md5 @4 :Data $Json.base64;
  sha1 @5 :Data $Json.hex;
  sha256 @6 :Data $Json.hex;
  sha384 @7 :Data $Json.hex;
  sha512 @8 :Data $Json.hex;
  storageClass @9 :Text;
  ssec @10 :R2SSECOptions;
}

struct R2CopySource {
    bucket @0 :Text;
    object @1 :Text;
    onlyIf @2 :R2Conditional;
    ssec @3 :R2SSECOptions;
}

enum R2CopyMetadataDirective {
  copy @0;
  replace @1;
  merge @2;
}

struct R2CopyRequest {
  object @0 :Text;
  source @1 :R2CopySource;
  metadataDirective @2 :Text;
  customFields @3 :List(Record);
  httpFields @4 :R2HttpFields;
  onlyIf @5 :R2Conditional;
  storageClass @6 :Text;
  ssec @7 :R2SSECOptions;
}

struct R2CreateMultipartUploadRequest {
  object @0 :Text;
  customFields @1 :List(Record);
  httpFields @2 :R2HttpFields;
  storageClass @3 :Text;
  ssec @4 :R2SSECOptions;
}

struct R2UploadPartRequest {
  object @0 :Text;
  uploadId @1 :Text;
  partNumber @2 :UInt32;
  ssec @3 :R2SSECOptions;
}

struct R2UploadPartCopySource {
    bucket @0 :Text;
    object @1 :Text;
    onlyIf @2 :R2Conditional;
    range @3 :R2Range;
    rangeHeader @4 :Text;
    ssec @5 :R2SSECOptions;
}

struct R2UploadPartCopyRequest {
  object @0 :Text;
  uploadId @1 :Text;
  source @2 :R2UploadPartCopySource;
  partNumber @3 :UInt32;
  ssec @4 :R2SSECOptions;
}

struct R2CompleteMultipartUploadRequest {
  object @0 :Text;
  uploadId @1 :Text;
  parts @2 :List(R2PublishedPart);
}

struct R2AbortMultipartUploadRequest {
  object @0 :Text;
  uploadId @1 :Text;
}

struct R2ListRequest {
  limit @0 :UInt32 = 0xffffffff;

  prefix @1 :Text;
  cursor @2 :Text;
  delimiter @3 :Text;
  startAfter @4 :Text;

  include @5 :List(UInt16);
  # Additional fields to include in the response that might not normally be rendered.
  # The values are all IncludeField but we can't actually have that here because otherwise
  # capnp's builtin JSON encoder will print the enum name instead of the value.

  enum IncludeField @0xc02f1c58744671f1 {
    http @0;
    custom @1;
  }
}

struct R2DeleteRequest {
  union {
    object @0 :Text;
    objects @1 :List(Text);
  }
}

struct R2CreateBucketRequest {
  bucket @0 :Text;
}

struct R2ListBucketRequest {
  limit @0 :UInt32 = 0xffffffff;

  prefix @1 :Text;
  cursor @2 :Text;
}

struct R2DeleteBucketRequest {
  bucket @0 :Text;
}

struct R2ErrorResponse {
  version @0 :UInt32;
  v4code @1 :UInt32;
  # Bindings use the same error code space as the V4 API.
  message @2 :Text;
}

struct R2SSECResponse {
  algorithm @0 :Text;
  keyMd5 @1 :Text;
}

struct R2HeadResponse {
  name @0 :Text;
  # The name of the object.

  version @1 :Text;
  # The version ID of the object.

  size @2 :UInt64;
  # The total size of the object in bytes.

  etag @3 :Text;
  # The ETag the object has currently.

  uploadedMillisecondsSinceEpoch @4 :UInt64 $Json.name("uploaded");
  # The timestamp of when the object was uploaded.

  httpFields @5 :R2HttpFields;
  # The HTTP headers that we were asked to associate with this object on upload.

  customFields @6 :List(Record);
  # Arbitrary key-value pairs that we were asked to associate with this object on upload.
  # Since cap'n'proto doesn't really have a natural key-value type, we emulate it with an exploded
  # list of the entries of the map.

  range @7 :R2Range;
  # If set, an echo of the range that was requested.

  checksums @8 :R2Checksums;
  # If set, the available checksums for this object

  storageClass @9 :Text;
  # The storage class of the object. Standard or Infrequent Access.
  # Provided on object creation to specify which storage tier R2 should use for this object.

  ssec @10 :R2SSECResponse;
  # The algorithm/key hash used for encryption(if the user used SSE-C)
}

using R2GetResponse = R2HeadResponse;

using R2PutResponse = R2HeadResponse;

using R2CopyResponse = R2HeadResponse;

struct R2CreateMultipartUploadResponse {
  uploadId @0 :Text;
  # The unique identifier of this object, required for subsequent operations on
  # this multipart upload.
  ssec @1 :R2SSECResponse;
  # The algorithm/key hash used for encryption(if the user used SSE-C)
}

struct R2UploadPartResponse {
  etag @0 :Text;
  # The ETag the of the uploaded part.
  # This ETag is required in order to complete the multipart upload.
}

using R2UploadPartCopyResponse = R2UploadPartResponse;

using R2CompleteMultipartUploadResponse = R2PutResponse;

struct R2AbortMultipartUploadResponse {}

struct R2ListResponse {
  objects @0 :List(R2HeadResponse);
  truncated @1 :Bool;
  cursor @2 :Text;
  delimitedPrefixes @3 :List(Text);
}

struct R2DeleteResponse {}

struct R2CreateBucketResponse {}
struct R2ListBucketResponse {
  buckets @0 :List(Bucket);
  truncated @1 :Bool;
  cursor @2 :Text;

  struct Bucket {
    name @0 :Text;
    createdMillisecondsSinceEpoch @1 :UInt64 $Json.name("created");
  }
}
struct R2DeleteBucketResponse {}
