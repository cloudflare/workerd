// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use r2_api_serde::R2BindingRequest;
use r2_api_serde::R2BindingRequestPayload;
use r2_api_serde::R2DeleteRequestWhich;

fn parse(json: &str) -> serde_json::Result<R2BindingRequest> {
    serde_json::from_str(json)
}

fn assert_upload_part(request: &R2BindingRequest) {
    assert_eq!(request.version, 1);
    let R2BindingRequestPayload::UploadPart(part) = &request.payload else {
        panic!("{request:?}");
    };
    assert_eq!(part.object.as_deref(), Some("key"));
    assert_eq!(part.upload_id.as_deref(), Some("id"));
    assert_eq!(part.part_number, 3);
}

#[test]
fn fields_in_any_order() {
    for json in [
        // As capnp's JsonCodec writes them.
        r#"{"version":1,"method":"uploadPart","object":"key","uploadId":"id","partNumber":3}"#,
        r#"{"method":"uploadPart","version":1,"object":"key","uploadId":"id","partNumber":3}"#,
        r#"{"method":"uploadPart","object":"key","version":1,"uploadId":"id","partNumber":3}"#,
        r#"{"method":"uploadPart","object":"key","uploadId":"id","partNumber":3,"version":1}"#,
        r#"{"object":"key","version":1,"uploadId":"id","method":"uploadPart","partNumber":3}"#,
        r#"{"object":"key","uploadId":"id","partNumber":3,"method":"uploadPart","version":1}"#,
    ] {
        assert_upload_part(&parse(json).unwrap());
    }
}

#[test]
fn escaped_keys() {
    assert_upload_part(
        &parse(
            r#"{"vers\u0069on":1,"meth\u006fd":"upload\u0050art","\u006fbject":"key","uploadId":"id","partNumber":3}"#,
        )
        .unwrap(),
    );
}

#[test]
fn unknown_fields_are_ignored() {
    assert_upload_part(
        &parse(
            r#"{"version":1,"extra":[1,{"a":2}],"method":"uploadPart","object":"key","more":null,"uploadId":"id","partNumber":3}"#,
        )
        .unwrap(),
    );
}

#[test]
fn missing_version() {
    let request = parse(r#"{"method":"head","object":"key"}"#).unwrap();
    assert_eq!(request.version, 0);
    let R2BindingRequestPayload::Head(head) = request.payload else {
        panic!("{:?}", request.payload);
    };
    assert_eq!(head.object.as_deref(), Some("key"));
}

#[test]
fn delete_union() {
    let request = parse(r#"{"version":1,"method":"delete","object":"key"}"#).unwrap();
    let R2BindingRequestPayload::Delete(delete) = request.payload else {
        panic!("{:?}", request.payload);
    };
    assert_eq!(delete.which, R2DeleteRequestWhich::Object("key".to_owned()));

    let request = parse(r#"{"version":1,"method":"delete","objects":["a","b"],"x":1}"#).unwrap();
    let R2BindingRequestPayload::Delete(delete) = request.payload else {
        panic!("{:?}", request.payload);
    };
    assert_eq!(
        delete.which,
        R2DeleteRequestWhich::Objects(vec!["a".to_owned(), "b".to_owned()])
    );
}

#[test]
fn method_index() {
    // The tag is a member's name, as in capnp's JsonCodec, not its index.
    assert!(parse(r#"{"version":1,"method":0,"object":"key"}"#).is_err());
}

#[test]
fn duplicate_keys() {
    // The last value wins, as in capnp's JsonCodec.
    let request =
        parse(r#"{"version":1,"method":"head","object":"a","object":"key","version":2}"#).unwrap();
    assert_eq!(request.version, 2);
    let R2BindingRequestPayload::Head(head) = request.payload else {
        panic!("{:?}", request.payload);
    };
    assert_eq!(head.object.as_deref(), Some("key"));

    assert!(parse(r#"{"version":1,"method":"head","method":"head"}"#).is_err());
}

#[test]
fn invalid_requests() {
    for json in [
        r#"{"version":1}"#,
        r#"{"version":1,"object":"key"}"#,
        r#"{"version":1,"method":"unknown","object":"key"}"#,
        r#"{"version":1,"method":12}"#,
        r#"{"version":"x","method":"head"}"#,
        r#"{"version":1,"method":"head","object":1}"#,
        r#"{"version":1,"method":"head","object":"key""#,
        "[]",
    ] {
        assert!(parse(json).is_err(), "{json}");
    }
}
