// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn snake_case_splits_words() {
    assert_eq!(snake_case("object"), "object");
    assert_eq!(snake_case("keyMd5"), "key_md5");
    assert_eq!(snake_case("v4code"), "v4code");
    assert_eq!(
        snake_case("uploadedMillisecondsSinceEpoch"),
        "uploaded_milliseconds_since_epoch"
    );
    assert_eq!(snake_case("HTTPServer"), "http_server");
    assert_eq!(snake_case("R2ListRequest"), "r2_list_request");
    assert_eq!(snake_case("already_snake"), "already_snake");
}

#[test]
fn pascal_case_joins_words() {
    assert_eq!(pascal_case("createBucket"), "CreateBucket");
    assert_eq!(pascal_case("foo_bar"), "FooBar");
    assert_eq!(pascal_case("R2Etag"), "R2Etag");
}

#[test]
fn screaming_snake_case_shouts() {
    assert_eq!(
        screaming_snake_case("versionPublicBeta"),
        "VERSION_PUBLIC_BETA"
    );
}

#[test]
fn keywords_become_identifiers() {
    assert_eq!(identifier("type"), "r#type");
    assert_eq!(identifier("self"), "self_");
    assert_eq!(identifier("Self"), "Self_");
    assert_eq!(identifier("value"), "value");
    assert_eq!(unraw("r#type"), "type");
    assert_eq!(unraw("value"), "value");
}
