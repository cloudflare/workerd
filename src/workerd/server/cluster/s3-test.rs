// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#![allow(clippy::unwrap_used)]

use std::time::Duration;

use super::*;

#[test]
fn dates_round_trip() {
    let t = UNIX_EPOCH + Duration::from_millis(1_790_348_414_500);
    assert_eq!(
        format_rfc3339_nano(1_790_348_414_500),
        "2026-09-25T15:00:14.5Z"
    );
    assert_eq!(
        format_rfc3339_nano(1_790_348_414_000),
        "2026-09-25T15:00:14Z"
    );
    assert_eq!(amz_date(t).1, "20260925T150014Z");
    assert_eq!(civil(11_016), (2000, 2, 29));
}

#[test]
fn credentials_from_an_env_file() {
    let c = Credentials::parse(
        "# r2\nexport AWS_ACCESS_KEY_ID='k'\nAWS_SECRET_ACCESS_KEY=\"s\"\n\
         AWS_SESSION_TOKEN=t=\nS3_ENDPOINT=https://x\n",
        None,
    )
    .unwrap();
    assert_eq!(
        (&*c.key_id, &*c.secret, c.token.as_deref()),
        ("k", "s", Some("t="))
    );
    assert!(Credentials::parse("AWS_ACCESS_KEY_ID=k\n", None).is_none());
}

#[test]
fn a_missing_credential_names_its_variable() {
    // Unless the environment supplies it.
    if ["AWS_SECRET_ACCESS_KEY", "AWS_CREDENTIALS_FILE"]
        .iter()
        .any(|var| std::env::var_os(var).is_some())
    {
        return;
    }
    let options = [("aws_access_key_id".to_owned(), "k".to_owned())];
    let Err(Error::Config(message)) = S3::new("s3://bucket/prefix", &options) else {
        panic!("expected a config error");
    };
    assert_eq!(
        message,
        "set aws_secret_access_key or AWS_SECRET_ACCESS_KEY"
    );
}

#[test]
fn every_amz_header_is_signed() {
    // The signature botocore's S3SigV4Auth makes of the same request,
    // with payload signing off.
    let mut request = Request {
        method: "PUT",
        host: "s3.us-east-1.amazonaws.com",
        path: "/b/k",
        query: "partNumber=1&uploadId=u",
        headers: vec![
            ("x-amz-meta-m".into(), "  a  b ".into()),
            ("x-amz-meta-workerd-r2".into(), "{\"x\":1}".into()),
        ],
    };
    let credentials = Credentials {
        key_id: "AKIDEXAMPLE".into(),
        secret: "wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY".into(),
        token: None,
        modified: None,
    };
    let now = UNIX_EPOCH + Duration::from_secs(1_794_832_496);
    sign(&mut request, &credentials, "us-east-1", now);
    let authorization = request.headers.iter().find(|(n, _)| n == "authorization");
    assert_eq!(
        authorization.unwrap().1,
        "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20261116/us-east-1/s3/aws4_request, \
         SignedHeaders=host;x-amz-content-sha256;x-amz-date;x-amz-meta-m;\
         x-amz-meta-workerd-r2, \
         Signature=5ecc9bb05cd026951412503c39fc434ea74a9d952f1b5f7973c8fd625ddfa7a7"
    );
}
