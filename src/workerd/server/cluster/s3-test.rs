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
