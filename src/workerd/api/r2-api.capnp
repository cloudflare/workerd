# Copyright (c) 2017-2022 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

@0xfb0dc52eec08c4d2;

using Cxx = import "/capnp/c++.capnp";
using Json = import "/capnp/compat/json.capnp";

$Cxx.namespace("workerd::api::public_beta");

const versionPublicBeta :UInt32 = 1;

struct R2BindingRequest {
  version @0 :UInt32;
  payload :union $Json.flatten() $Json.discriminator(name="method") {
    head @1 :R2HeadRequest $Json.flatten();
    get @2 :R2GetRequest $Json.flatten();
    put @3 :R2PutRequest $Json.flatten();
    list @4 :R2ListRequest $Json.flatten();
    delete @5 :R2DeleteRequest $Json.flatten();
    createBucket @6 :R2CreateBucketRequest $Json.flatten();
    listBucket @7 :R2ListBucketRequest $Json.flatten();
    deleteBucket @8 :R2DeleteBucketRequest $Json.flatten();
    createMultipartUpload @9 :R2CreateMultipartUploadRequest $Json.flatten();
    uploadPart @10 :R2UploadPartRequest $Json.flatten();
    completeMultipartUpload @11 :R2CompleteMultipartUploadRequest $Json.flatten();
    abortMultipartUpload @12 :R2AbortMultipartUploadRequest $Json.flatten();
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

struct R2Conditional {
  etagMatches @0 :Text;
  etagDoesNotMatch @1 :Text;
  uploadedBefore @2 :UInt64 = 0xffffffffffffffff;
  uploadedAfter @3 :UInt64 = 0xffffffffffffffff;
  secondsGranularity @4 :Bool;
  # Should uploadedBefore / uploadedAfter be evaluated against the seconds granularity of the upload
  # timestamp.
}

struct R2Checksums {
  # The JSON name of these fields must comform to the representation of the ChecksumAlgorithm in
  # the R2 gateway worker.
  md5 @0 :Data $Json.hex $Json.name("0") ;
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
}

struct R2CreateMultipartUploadRequest {
  object @0 :Text;
  customFields @1 :List(Record);
  httpFields @2 :R2HttpFields;
}

struct R2UploadPartRequest {
  object @0 :Text;
  uploadId @1 :Text;
  partNumber @2 :UInt32;
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

  newRuntime @6 :Bool;
  # We used to send the include but not honor it. Newer versions of the runtime will send this to
  # indicate we're sending a version of the `include` that can be honored. Before that the R2 worker
  # should assume include always means http & custom metadata should be returned. This is a
  # transitionary field just to avoid coupling needing to simultaneously release the Worker &
  # runtime and can be removed after the release cut for the week of July 4, 2022.

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
}

using R2GetResponse = R2HeadResponse;

using R2PutResponse = R2HeadResponse;

struct R2CreateMultipartUploadResponse {
  uploadId @0 :Text;
  # The unique identifier of this object, required for subsequent operations on
  # this multipart upload.
}

struct R2UploadPartResponse {
  etag @0 :Text;
  # The ETag the of the uploaded part.
  # This ETag is required in order to complete the multipart upload.
}

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
