use r2_api_serde::R2BindingRequest;
use r2_api_serde::R2BindingRequestPayload;
use r2_api_serde::R2CreateMultipartUploadResponse;
use r2_api_serde::R2DeleteRequestWhich;
use r2_api_serde::R2ErrorResponse;
use r2_api_serde::R2EtagType;
use r2_api_serde::R2HeadResponse;
use r2_api_serde::R2ListResponse;
use r2_api_serde::R2UploadPartResponse;

use super::*;
use crate::parse;

fn parse_fixture<T: serde::de::DeserializeOwned>(f: Fixture) -> T {
    *parse(fixture(f).as_bytes()).unwrap()
}

fn parse_request(f: Fixture) -> R2BindingRequestPayload {
    let request = parse_fixture::<R2BindingRequest>(f);
    assert_eq!(request.version, 1);
    request.payload
}

#[test]
fn head_response_minimal() {
    let head = parse_fixture::<R2HeadResponse>(Fixture::HeadResponseMinimal);
    assert_eq!(head.name.as_deref(), Some("index.html"));
    assert_eq!(head.size, 5123);
    assert_eq!(head.uploaded_milliseconds_since_epoch, 1_720_000_000_000);
    let http = head.http_fields.unwrap();
    assert_eq!(
        http.content_type.as_deref(),
        Some("text/html; charset=utf-8")
    );
    assert!(http.cache_control.is_none());
    assert_eq!(http.cache_expiry, u64::MAX);
    assert!(head.custom_fields.unwrap().is_empty());
    assert!(head.range.is_none());
    assert_eq!(head.checksums.unwrap().md5.unwrap().len(), 16);
    assert!(head.ssec.is_none());
}

#[test]
fn head_response_full() {
    let head = parse_fixture::<R2HeadResponse>(Fixture::HeadResponseFull);
    let http = head.http_fields.unwrap();
    assert_eq!(
        http.content_disposition.as_deref(),
        Some(r#"attachment; filename="IMG_0001.jpg""#)
    );
    assert_eq!(http.cache_expiry, 1_751_536_000_000);
    let custom = head.custom_fields.unwrap();
    assert_eq!(custom.len(), 4);
    assert_eq!(custom[3].v.as_deref(), Some("Summer \u{2600} 2024"));
    let range = head.range.unwrap();
    assert_eq!(
        (range.offset, range.length, range.suffix),
        (0, 1_048_576, u64::MAX)
    );
    let checksums = head.checksums.unwrap();
    assert_eq!(checksums.md5.unwrap().len(), 16);
    assert_eq!(checksums.sha1.unwrap().len(), 20);
    assert_eq!(checksums.sha256.unwrap().len(), 32);
    assert_eq!(checksums.sha384.unwrap().len(), 48);
    assert_eq!(checksums.sha512.unwrap()[..2], [0x30, 0x9e]);
    assert_eq!(head.storage_class.as_deref(), Some("InfrequentAccess"));
    assert_eq!(head.ssec.unwrap().algorithm.as_deref(), Some("AES256"));
}

#[test]
fn list_responses() {
    for (f, objects) in [
        (Fixture::ListResponseSmall, OBJECTS_IN_SMALL_LIST),
        (Fixture::ListResponseLarge, OBJECTS_IN_LARGE_LIST),
    ] {
        let list = parse_fixture::<R2ListResponse>(f);
        let listed = list.objects.unwrap();
        assert_eq!(listed.len(), objects);
        let final_object = listed.last().unwrap();
        assert_eq!(
            final_object.name.as_deref(),
            Some(format!("photos/2024/07/IMG_{:04}.jpg", objects - 1).as_str())
        );
        assert_eq!(
            final_object
                .checksums
                .as_ref()
                .unwrap()
                .md5
                .as_ref()
                .unwrap()
                .len(),
            16
        );
        assert!(list.truncated);
        assert_eq!(list.delimited_prefixes.unwrap().len(), 2);
    }
}

#[test]
fn multipart_responses() {
    let create =
        parse_fixture::<R2CreateMultipartUploadResponse>(Fixture::CreateMultipartUploadResponse);
    assert!(create.upload_id.unwrap().starts_with("AJ9x"));
    let part = parse_fixture::<R2UploadPartResponse>(Fixture::UploadPartResponse);
    assert_eq!(part.etag.unwrap().len(), 32);
}

#[test]
fn error_response() {
    let error = parse_fixture::<R2ErrorResponse>(Fixture::ErrorResponse);
    assert_eq!((error.version, error.v4code), (1, 10007));
    assert_eq!(
        error.message.as_deref(),
        Some("The specified key does not exist.")
    );
}

#[test]
fn get_request() {
    let R2BindingRequestPayload::Get(get) = parse_request(Fixture::GetRequest) else {
        panic!();
    };
    let range = get.range.unwrap();
    assert_eq!(
        (range.offset, range.length, range.suffix),
        (0, 1_048_576, u64::MAX)
    );
    let only_if = get.only_if.unwrap();
    let etags = only_if.etag_matches.unwrap();
    assert_eq!(etags.len(), 1);
    assert_eq!(etags[0].r#type, R2EtagType::Strong);
    assert_eq!(only_if.uploaded_after, 1_719_999_999_000);
    assert_eq!(only_if.uploaded_before, u64::MAX);
}

#[test]
fn put_request() {
    let R2BindingRequestPayload::Put(put) = parse_request(Fixture::PutRequest) else {
        panic!();
    };
    assert_eq!(put.custom_fields.unwrap().len(), 2);
    assert_eq!(put.md5.unwrap()[..2], [0x5e, 0xb6]);
    assert_eq!(put.sha256.unwrap()[..2], [0xb9, 0x4d]);
    assert!(put.sha1.is_none());
}

#[test]
fn list_request() {
    let R2BindingRequestPayload::List(list) = parse_request(Fixture::ListRequest) else {
        panic!();
    };
    assert_eq!(list.limit, 1000);
    assert_eq!(list.delimiter.as_deref(), Some("/"));
    assert_eq!(list.include.unwrap(), [0, 1]);
    assert!(list.new_runtime);
    assert!(list.start_after.is_none());
}

#[test]
fn delete_request() {
    let R2BindingRequestPayload::Delete(delete) = parse_request(Fixture::DeleteRequest) else {
        panic!();
    };
    let R2DeleteRequestWhich::Objects(objects) = delete.which else {
        panic!();
    };
    assert_eq!(objects.len(), OBJECTS_IN_DELETE);
}

#[test]
fn complete_multipart_upload_request() {
    let R2BindingRequestPayload::CompleteMultipartUpload(complete) =
        parse_request(Fixture::CompleteMultipartUploadRequest)
    else {
        panic!();
    };
    let parts = complete.parts.unwrap();
    assert_eq!(parts.len(), PARTS_IN_COMPLETE);
    assert_eq!(
        parts.last().unwrap().part,
        u32::try_from(PARTS_IN_COMPLETE).unwrap()
    );
}
