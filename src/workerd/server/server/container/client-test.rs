// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn uri_components() {
    assert_eq!(encode_uri_component("workerd-abc_1.2"), "workerd-abc_1.2");
    assert_eq!(encode_uri_component("a b/c?d"), "a%20b%2Fc%3Fd");
    assert_eq!(encode_uri_component("!~*'()"), "!~*'()");
    assert_eq!(
        encode_uri_component(r#"{"name":["workerd-snap-"]}"#),
        "%7B%22name%22%3A%5B%22workerd-snap-%22%5D%7D"
    );
}

#[test]
fn image_aliases() {
    let resolved = resolve_image("cloudflare/debian-trixie");
    assert!(resolved.starts_with("docker.io/library/node:24.20.0-trixie-slim@sha256:"));
    assert_eq!(image_alias(resolved), "cloudflare/debian-trixie");
    assert_eq!(resolve_image("my-image"), "my-image");
    assert_eq!(image_alias("my-image"), "my-image");
}

#[test]
fn signal_names() {
    assert_eq!(signal_name(1), "SIGHUP");
    assert_eq!(signal_name(9), "SIGKILL");
    assert_eq!(signal_name(15), "SIGTERM");
    assert_eq!(signal_name(31), "SIGSYS");
    assert_eq!(signal_name(0), "SIGKILL");
    assert_eq!(signal_name(32), "SIGKILL");
}

#[test]
fn random_uuids_are_canonical_v4() {
    let id = random_uuid();
    assert_eq!(parse_snapshot_id(&id).unwrap(), id);
    assert_eq!(&id[14..15], "4");
    assert!(matches!(&id[19..20], "8" | "9" | "a" | "b"));
    assert_ne!(random_uuid(), id);
}

#[test]
fn snapshot_ids() {
    let id = "123e4567-e89b-42d3-a456-426614174000";
    assert_eq!(parse_snapshot_id(id).unwrap(), id);
    for bad in [
        "123E4567-E89B-42D3-A456-426614174000",
        "123e4567e89b42d3a456426614174000",
        "00000000-0000-0000-0000-000000000000",
        "not-a-uuid",
        "",
    ] {
        let error = parse_snapshot_id(bad).unwrap_err();
        assert_eq!(
            error.description(),
            format!("jsg.Error: Invalid snapshot ID{bad}")
        );
    }
}

#[test]
fn absolute_paths() {
    assert_eq!(parse_absolute_path("/").unwrap(), "/");
    assert_eq!(parse_absolute_path("/app/data").unwrap(), "/app/data");
    assert_eq!(
        parse_absolute_path("/app//data/./x/../").unwrap(),
        "/app/data"
    );
    let description = |path| {
        parse_absolute_path(path)
            .unwrap_err()
            .description()
            .to_owned()
    };
    assert_eq!(
        description("app"),
        "jsg.Error: Snapshot path must be absolute, got: app"
    );
    assert_eq!(
        description("/a\0b"),
        "jsg.Error: Snapshot path must not contain null bytes"
    );
    assert_eq!(
        description("/a/../.."),
        "jsg.Error: Snapshot path contains invalid components: /a/../..; expected parts.size() \
         > 0; can't use \"..\" to break out of starting directory"
    );
    assert_eq!(
        description("//a"),
        "jsg.Error: Snapshot path contains invalid components: //a; expected \
         !path.startsWith(\"/\"); expected a relative path, got absolute; path = /a"
    );
}

#[test]
fn published_host_ports() {
    let value = serde_json::json!([{"HostIp": "0.0.0.0", "HostPort": "55000"}]);
    assert_eq!(published_host_port(&value).unwrap(), Some(55000));
    assert_eq!(published_host_port(&Value::Null).unwrap(), None);
    assert_eq!(published_host_port(&serde_json::json!([])).unwrap(), None);
    assert!(published_host_port(&serde_json::json!("x")).is_err());
    assert!(published_host_port(&serde_json::json!([{"HostIp": "0.0.0.0"}])).is_err());
}

#[test]
fn snapshot_created_at_label() {
    let labels = serde_json::json!({"dev.workerd.snapshot-created-at": "1700000000"});
    assert_eq!(snapshot_created_at(Some(&labels)), Some(1_700_000_000));
    assert_eq!(snapshot_created_at(Some(&serde_json::json!({}))), None);
    assert_eq!(
        snapshot_created_at(Some(
            &serde_json::json!({"dev.workerd.snapshot-created-at": 1})
        )),
        None
    );
    assert_eq!(snapshot_created_at(None), None);
}

#[test]
fn labels_keep_their_first_order() {
    let labels = dedupe_labels(vec![
        ("b".to_owned(), "1".to_owned()),
        ("a".to_owned(), "2".to_owned()),
        ("b".to_owned(), "3".to_owned()),
    ]);
    assert_eq!(
        labels,
        [
            ("b".to_owned(), "3".to_owned()),
            ("a".to_owned(), "2".to_owned())
        ]
    );
}
