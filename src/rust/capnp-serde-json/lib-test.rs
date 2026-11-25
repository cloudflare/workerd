// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use serde::Deserialize;
use serde::Serialize;
use serde_json::json;

use super::*;

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Numbers {
    #[serde(with = "As::<Int64>")]
    signed: i64,
    #[serde(with = "As::<Int64>")]
    unsigned: u64,
}

#[test]
fn int64_serializes_as_a_string() {
    let value = Numbers {
        signed: i64::MIN,
        unsigned: u64::MAX,
    };
    let json = json!({"signed": "-9223372036854775808", "unsigned": "18446744073709551615"});
    assert_eq!(serde_json::to_value(&value).unwrap(), json);
    assert_eq!(serde_json::from_value::<Numbers>(json).unwrap(), value);
}

#[test]
fn int64_deserializes_from_a_number() {
    let value: Numbers = serde_json::from_str(r#"{"signed": -5, "unsigned": 7}"#).unwrap();
    assert_eq!(
        value,
        Numbers {
            signed: -5,
            unsigned: 7
        }
    );
}

#[test]
fn int64_rejects_other_values() {
    for json in [
        // Out of range.
        r#"{"signed": 0, "unsigned": -1}"#,
        r#"{"signed": 0, "unsigned": "-1"}"#,
        r#"{"signed": 0, "unsigned": "18446744073709551616"}"#,
        r#"{"signed": "9223372036854775808", "unsigned": 0}"#,
        // Not decimal integers.
        r#"{"signed": 1.0, "unsigned": 0}"#,
        r#"{"signed": " 1", "unsigned": 0}"#,
        r#"{"signed": "0x10", "unsigned": 0}"#,
        r#"{"signed": "", "unsigned": 0}"#,
        r#"{"signed": null, "unsigned": 0}"#,
    ] {
        assert!(serde_json::from_str::<Numbers>(json).is_err(), "{json}");
    }
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Datas {
    #[serde(with = "As::<Base64>")]
    base64: Vec<u8>,
    #[serde(with = "As::<Hex>")]
    hex: Vec<u8>,
    #[serde(default, with = "As::<Option<Vec<Int64>>>")]
    list: Option<Vec<i64>>,
}

#[test]
fn data_encodings() {
    let value = Datas {
        base64: b"abc".to_vec(),
        hex: vec![0xab, 0x01],
        list: Some(vec![-1, 2]),
    };
    let json = json!({"base64": "YWJj", "hex": "ab01", "list": ["-1", "2"]});
    assert_eq!(serde_json::to_value(&value).unwrap(), json);
    assert_eq!(serde_json::from_value::<Datas>(json).unwrap(), value);

    let value: Datas =
        serde_json::from_value(json!({"base64": "YWI=", "hex": "09aFAf", "list": null})).unwrap();
    assert_eq!(value.base64, b"ab");
    assert_eq!(value.hex, vec![0x09, 0xaf, 0xaf]);
    assert_eq!(value.list, None);
}

#[test]
fn data_encodings_are_strict() {
    for (base64, hex) in [
        ("YWI", ""),
        ("YW Jj", ""),
        ("YWJj\n", ""),
        ("YW-j", ""),
        ("", "a"),
        ("", "1g"),
        ("", "a b"),
        ("", "zz"),
        ("", "\u{e9}"),
    ] {
        assert!(
            serde_json::from_value::<Datas>(json!({"base64": base64, "hex": hex})).is_err(),
            "{base64:?} {hex:?}"
        );
    }
    assert!(serde_json::from_value::<Datas>(json!({"base64": 5, "hex": ""})).is_err());
}

#[test]
fn data_encodings_decode_strings_from_documents() {
    // Strings without escapes are decoded from the document, those with escapes from serde_json's
    // copy.
    for json in [
        r#"{"base64": "YWJj", "hex": "ab01"}"#,
        r#"{"base64": "YW\u004aj", "hex": "\u0061b01"}"#,
    ] {
        let value: Datas = serde_json::from_str(json).unwrap();
        assert_eq!(value.base64, b"abc");
        assert_eq!(value.hex, vec![0xab, 0x01]);
    }

    let error = serde_json::from_str::<Datas>(r#"{"base64": "", "hex": []}"#).unwrap_err();
    assert_eq!(
        error.to_string(),
        "invalid type: sequence, expected hex digits at line 1 column 22"
    );
    let error = serde_json::from_str::<Datas>(r#"{"base64": "YWJ", "hex": ""}"#).unwrap_err();
    assert_eq!(
        error.to_string(),
        "invalid value: string \"YWJ\", expected standard, padded base64 at line 1 column 16"
    );
}

#[derive(Debug, PartialEq, Deserialize)]
struct Text {
    text: String,
}

#[test]
fn from_slice_matches_serde_json() {
    let documents: [(&[u8], bool); 4] = [
        (b"{\"text\": \"caf\xc3\xa9\"}", true),
        // Invalid UTF-8 in a string that is decoded, in one that is ignored, and outside strings.
        (b"{\"text\": \"\xff\"}", false),
        (b"{\"text\": \"a\", \"other\": \"\xff\"}", true),
        (b"{\"text\": \"a\"}\xff", false),
    ];
    for (json, valid) in documents {
        let value = from_slice::<Text>(json).map_err(|error| error.to_string());
        assert_eq!(
            value,
            serde_json::from_slice::<Text>(json).map_err(|error| error.to_string())
        );
        assert_eq!(value.is_ok(), valid, "{value:?}");
    }
}

#[test]
fn codec_input_ends_at_nul() {
    let text = |json: &[u8]| from_codec_input::<Text>(json).map(|value| value.text);
    assert_eq!(text(b"{\"text\": \"a\"}").unwrap(), "a");
    assert_eq!(text(b"{\"text\": \"a\"}\0").unwrap(), "a");
    assert_eq!(text(b"{\"text\": \"a\"}\0garbage").unwrap(), "a");
    assert!(text(b"{\"text\": \"a\0\"}").is_err());
    assert!(text(b"\0{\"text\": \"a\"}").is_err());
}

#[test]
fn default_predicates() {
    assert!(is_default(&0_u32));
    assert!(!is_default(&1_u32));
    assert!(eq_u64::<{ u64::MAX }>(&u64::MAX));
    assert!(eq_bool::<true>(&true));
    assert!(eq_i8::<-3>(&-3));
    assert!(!eq_i8::<-3>(&3));
}
