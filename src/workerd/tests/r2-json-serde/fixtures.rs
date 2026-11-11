// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The JSON documents that `bench-r2-json` decodes.
//!
//! They take the forms R2 exchanges them in: responses as the R2 gateway Worker writes them
//! (integers as numbers), requests as capnp's `JsonCodec` writes them (64-bit integers as
//! strings, default-valued fields omitted).

use std::fmt::Write;
use std::sync::LazyLock;

use crate::ffi::Fixture;

pub const OBJECTS_IN_SMALL_LIST: usize = 10;
pub const OBJECTS_IN_LARGE_LIST: usize = 1000;
pub const OBJECTS_IN_DELETE: usize = 100;
pub const PARTS_IN_COMPLETE: usize = 100;

/// A `get` of an object stored with only a content type.
const HEAD_RESPONSE_MINIMAL: &str = r#"{"name":"index.html","version":"7e5a3c4f2b8d4e1fa6c9b0d3e2f1a4b5","size":5123,"etag":"5eb63bbbe01eeed093cb22bb8f5acdc3","uploaded":1720000000000,"httpFields":{"contentType":"text/html; charset=utf-8"},"customFields":[],"checksums":{"0":"5eb63bbbe01eeed093cb22bb8f5acdc3"},"storageClass":"Standard"}"#;

/// A ranged `get` of an object stored with every optional field.
const HEAD_RESPONSE_FULL: &str = r#"{"name":"photos/2024/07/IMG_0001.jpg","version":"7e5a3c4f2b8d4e1fa6c9b0d3e2f1a4b5","size":4831029,"etag":"5eb63bbbe01eeed093cb22bb8f5acdc3","uploaded":1720000000000,"httpFields":{"contentType":"image/jpeg","contentLanguage":"en-US","contentDisposition":"attachment; filename=\"IMG_0001.jpg\"","contentEncoding":"identity","cacheControl":"public, max-age=31536000, immutable","cacheExpiry":1751536000000},"customFields":[{"k":"camera","v":"Pixel 8 Pro"},{"k":"location","v":"San Francisco, CA"},{"k":"uploaded-by","v":"user-42"},{"k":"album","v":"Summer \u2600 2024"}],"range":{"offset":0,"length":1048576},"checksums":{"0":"5eb63bbbe01eeed093cb22bb8f5acdc3","1":"2aae6c35c94fcfb415dbe95f408b9ce91ee846ed","2":"b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9","3":"fdbd8e75a67f29f701a4e040385e2e23986303ea10239211af907fcbb83578b3e417cb71ce646efd0819dd8c088de1bd","4":"309ecc489c12d6eb4cc40f50c902f2b4d0ed77ee511a7c7a9bcd3ca86d4cd86f989dd35bc5ff499670da34255b45b0cfd830e81f605dcf7dc5542e93ae9cd76f"},"storageClass":"InfrequentAccess","ssec":{"algorithm":"AES256","keyMd5":"3c6e0b8a9c15224a8228b9a98ca1531d"}}"#;

const CREATE_MULTIPART_UPLOAD_RESPONSE: &str =
    r#"{"uploadId":"AJ9xQnVd3mS1kz0YbWg7RcT4pLhEfUa2oNiG8vKjX5yZqBwC6tMrDsH-IeOlP_FuA"}"#;

const UPLOAD_PART_RESPONSE: &str = r#"{"etag":"8a1b2c3d4e5f60718293a4b5c6d7e8f9"}"#;

const ERROR_RESPONSE: &str =
    r#"{"version":1,"v4code":10007,"message":"The specified key does not exist."}"#;

const GET_REQUEST: &str = r#"{"version":1,"method":"get","object":"photos/2024/07/IMG_0001.jpg","range":{"offset":"0","length":"1048576"},"onlyIf":{"etagMatches":[{"value":"5eb63bbbe01eeed093cb22bb8f5acdc3","type":"strong"}],"uploadedAfter":"1719999999000"}}"#;

const PUT_REQUEST: &str = r#"{"version":1,"method":"put","object":"photos/2024/07/IMG_0001.jpg","customFields":[{"k":"camera","v":"Pixel 8 Pro"},{"k":"location","v":"San Francisco, CA"}],"httpFields":{"contentType":"image/jpeg","cacheControl":"public, max-age=31536000, immutable"},"md5":"XrY7u+Ae7tCTyyK7j1rNww==","sha256":"b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9","storageClass":"Standard"}"#;

const LIST_REQUEST: &str = r#"{"version":1,"method":"list","limit":1000,"prefix":"photos/2024/","cursor":"eyJ2IjoxLCJzdGFydEFmdGVyIjoicGhvdG9zLzIwMjQvMDcvSU1HXzAwMDEuanBnIn0","delimiter":"/","include":[0,1],"newRuntime":true}"#;

/// An object in a list response: as a `head` of it, with the http and custom fields a list
/// includes when asked.
fn write_listed_object(json: &mut String, index: usize) {
    let _ = write!(
        json,
        r#"{{"name":"photos/2024/07/IMG_{index:04}.jpg","version":"7e5a3c4f2b8d4e1fa6c9b0d3e2f1{index:04x}","size":{size},"etag":"5eb63bbbe01eeed093cb22bb8f5a{index:04x}","uploaded":{uploaded},"httpFields":{{"contentType":"image/jpeg","cacheControl":"public, max-age=31536000"}},"customFields":[{{"k":"camera","v":"Pixel 8 Pro"}}],"checksums":{{"0":"5eb63bbbe01eeed093cb22bb8f5a{index:04x}"}},"storageClass":"Standard"}}"#,
        size = 4_000_000 + index * 7919,
        uploaded = 1_720_000_000_000_u64 + index as u64 * 60_000,
    );
}

fn list_response(objects: usize) -> String {
    let mut json = String::from(r#"{"objects":["#);
    for index in 0..objects {
        if index > 0 {
            json.push(',');
        }
        write_listed_object(&mut json, index);
    }
    json.push_str(r#"],"truncated":true,"cursor":"eyJ2IjoxLCJzdGFydEFmdGVyIjoicGhvdG9zLzIwMjQvMDcvSU1HXzA5OTkuanBnIn0","delimitedPrefixes":["photos/2024/07/raw/","photos/2024/07/edited/"]}"#);
    json
}

fn delete_request(objects: usize) -> String {
    let mut json = String::from(r#"{"version":1,"method":"delete","objects":["#);
    for index in 0..objects {
        if index > 0 {
            json.push(',');
        }
        let _ = write!(json, r#""photos/2024/07/IMG_{index:04}.jpg""#);
    }
    json.push_str("]}");
    json
}

fn complete_multipart_upload_request(parts: usize) -> String {
    let mut json = String::from(
        r#"{"version":1,"method":"completeMultipartUpload","object":"videos/2024/07/keynote.mp4","uploadId":"AJ9xQnVd3mS1kz0YbWg7RcT4pLhEfUa2oNiG8vKjX5yZqBwC6tMrDsH-IeOlP_FuA","parts":["#,
    );
    for index in 0..parts {
        if index > 0 {
            json.push(',');
        }
        let _ = write!(
            json,
            r#"{{"etag":"8a1b2c3d4e5f60718293a4b5c6d7{index:04x}","part":{part}}}"#,
            part = index + 1,
        );
    }
    json.push_str("]}");
    json
}

static LIST_RESPONSE_SMALL: LazyLock<String> =
    LazyLock::new(|| list_response(OBJECTS_IN_SMALL_LIST));
static LIST_RESPONSE_LARGE: LazyLock<String> =
    LazyLock::new(|| list_response(OBJECTS_IN_LARGE_LIST));
static DELETE_REQUEST: LazyLock<String> = LazyLock::new(|| delete_request(OBJECTS_IN_DELETE));
static COMPLETE_MULTIPART_UPLOAD_REQUEST: LazyLock<String> =
    LazyLock::new(|| complete_multipart_upload_request(PARTS_IN_COMPLETE));

pub fn fixture(fixture: Fixture) -> &'static str {
    match fixture {
        Fixture::HeadResponseMinimal => HEAD_RESPONSE_MINIMAL,
        Fixture::HeadResponseFull => HEAD_RESPONSE_FULL,
        Fixture::ListResponseSmall => &LIST_RESPONSE_SMALL,
        Fixture::ListResponseLarge => &LIST_RESPONSE_LARGE,
        Fixture::CreateMultipartUploadResponse => CREATE_MULTIPART_UPLOAD_RESPONSE,
        Fixture::UploadPartResponse => UPLOAD_PART_RESPONSE,
        Fixture::ErrorResponse => ERROR_RESPONSE,
        Fixture::GetRequest => GET_REQUEST,
        Fixture::PutRequest => PUT_REQUEST,
        Fixture::ListRequest => LIST_REQUEST,
        Fixture::DeleteRequest => &DELETE_REQUEST,
        Fixture::CompleteMultipartUploadRequest => &COMPLETE_MULTIPART_UPLOAD_REQUEST,
        _ => unreachable!("unknown fixture"),
    }
}

#[cfg(test)]
#[path = "fixtures-test.rs"]
mod tests;
