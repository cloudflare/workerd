use super::*;

#[test]
fn accept_key_is_rfc_6455s_example() {
    assert_eq!(
        accept_key(b"dGhlIHNhbXBsZSBub25jZQ=="),
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
    );
}

#[test]
fn client_keys_are_16_bytes_and_differ() {
    let a = client_key();
    let b = client_key();
    assert_eq!(a.len(), 24);
    assert!(a.ends_with("=="));
    assert_ne!(a, b);
}
