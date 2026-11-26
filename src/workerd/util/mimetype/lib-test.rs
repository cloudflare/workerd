use super::*;

#[test]
fn token_chars() {
    for c in 0..=255u8 {
        let expected = c.is_ascii_alphanumeric() || b"!#$%&'*+\\-.^_`|~".contains(&c);
        assert_eq!(is_token_char(c), expected, "{c:#04x}");
    }
}

#[test]
fn param_value_chars() {
    for c in 0..=255u8 {
        let expected = c == b'\t' || (0x20..=0x7e).contains(&c) || (0x80..=0xfe).contains(&c);
        assert_eq!(is_param_value_char(c), expected, "{c:#04x}");
    }
}

#[test]
fn tokens_and_values() {
    assert!(is_token(b"text"));
    assert!(!is_token(b""));
    assert!(!is_token(b"te xt"));
    assert!(!is_token(b"t\xe9"));
    assert!(is_param_value(b""));
    assert!(is_param_value(b"a b\t\"\xfe"));
    assert!(!is_param_value(b"\xff"));
    assert!(!is_param_value(b"a\nb"));
    assert!(!is_param_value(b"\x7f"));
}

#[test]
fn whitespace() {
    assert_eq!(skip_leading_whitespace(b" \t\r\n a "), b"a ");
    assert_eq!(skip_leading_whitespace(b"  "), b"");
    assert_eq!(trim_trailing_whitespace(b" a \t\r\n"), b" a");
    assert_eq!(trim_trailing_whitespace(b" \x0c"), b" \x0c");
    assert_eq!(trim_trailing_whitespace(b"  "), b"");
}
