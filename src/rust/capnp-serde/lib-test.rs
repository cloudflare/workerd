// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use serde::Deserialize;
use serde::Serialize;
use serde_json::json;

use super::*;

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Numbers {
    #[serde(with = "As::<Integer>")]
    small: i32,
    #[serde(with = "As::<Int64>")]
    big: u64,
    #[serde(with = "As::<Float>")]
    float: f64,
    #[serde(with = "As::<Float>")]
    float32: f32,
}

#[test]
fn numbers_serialize_as_the_cpp_codec_does() {
    let value = Numbers {
        small: -5,
        big: u64::MAX,
        float: f64::NEG_INFINITY,
        float32: 1.5,
    };
    assert_eq!(
        serde_json::to_value(&value).unwrap(),
        json!({"small": -5, "big": "18446744073709551615", "float": "-Infinity", "float32": 1.5})
    );
}

#[test]
fn numbers_deserialize_leniently() {
    let value: Numbers = serde_json::from_value(
        json!({"small": "-5", "big": 7, "float": "Infinity", "float32": 2.0}),
    )
    .unwrap();
    assert_eq!(
        value,
        Numbers {
            small: -5,
            big: 7,
            float: f64::INFINITY,
            float32: 2.0
        }
    );

    let value: Numbers =
        serde_json::from_value(json!({"small": 3.0, "big": "8", "float": null, "float32": "NaN"}))
            .unwrap();
    assert_eq!(value.small, 3);
    assert_eq!(value.big, 8);
    assert!(value.float.is_nan());
    assert!(value.float32.is_nan());
}

#[test]
fn integers_are_range_checked() {
    assert!(
        serde_json::from_value::<Numbers>(
            json!({"small": 1.5, "big": "1", "float": 0, "float32": 0})
        )
        .is_err()
    );
    assert!(
        serde_json::from_value::<Numbers>(
            json!({"small": 4_294_967_296_u64, "big": "1", "float": 0, "float32": 0})
        )
        .is_err()
    );
    assert!(
        serde_json::from_value::<Numbers>(json!({"small": 1, "big": -1, "float": 0, "float32": 0}))
            .is_err()
    );
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Datas {
    #[serde(with = "As::<Bytes>")]
    bytes: Vec<u8>,
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
        bytes: vec![1, 255],
        base64: b"abc".to_vec(),
        hex: vec![0xab, 0x01],
        list: Some(vec![-1, 2]),
    };
    let json = json!({"bytes": [1, 255], "base64": "YWJj", "hex": "ab01", "list": ["-1", "2"]});
    assert_eq!(serde_json::to_value(&value).unwrap(), json);
    assert_eq!(serde_json::from_value::<Datas>(json).unwrap(), value);

    let value: Datas =
        serde_json::from_value(json!({"bytes": [], "base64": "YWI", "hex": "AB", "list": null}))
            .unwrap();
    assert_eq!(value.base64, b"ab");
    assert_eq!(value.hex, vec![0xab]);
    assert_eq!(value.list, None);

    assert!(
        serde_json::from_value::<Datas>(json!({"bytes": [256], "base64": "", "hex": ""})).is_err()
    );
    assert!(
        serde_json::from_value::<Datas>(json!({"bytes": [], "base64": "", "hex": "x"})).is_err()
    );
}

#[test]
fn data_encodings_decode_strings_from_documents() {
    // Strings without escapes are decoded from the document, those with escapes from serde_json's
    // copy.
    for json in [
        r#"{"bytes": [], "base64": "YWJj", "hex": "ab01"}"#,
        r#"{"bytes": [], "base64": "YW\u004aj", "hex": "\u0061b01"}"#,
    ] {
        let value: Datas = serde_json::from_str(json).unwrap();
        assert_eq!(value.base64, b"abc");
        assert_eq!(value.hex, vec![0xab, 0x01]);
    }

    let error =
        serde_json::from_str::<Datas>(r#"{"bytes": [], "base64": "", "hex": "x"}"#).unwrap_err();
    assert_eq!(
        error.to_string(),
        "invalid value: string \"x\", expected hex digits at line 1 column 38"
    );
    let error =
        serde_json::from_str::<Datas>(r#"{"bytes": [], "base64": 5, "hex": ""}"#).unwrap_err();
    assert_eq!(
        error.to_string(),
        "invalid type: integer `5`, expected base64 at line 1 column 25"
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

#[derive(Clone, Copy, Debug, PartialEq)]
enum Color {
    Red,
    DarkBlue,
}

impl Enum for Color {
    const ENUMERANTS: &'static [(&'static str, Self)] =
        &[("red", Self::Red), ("blue", Self::DarkBlue)];

    fn ordinal(self) -> u16 {
        self as u16
    }
}

impl Serialize for Color {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        serialize_enum(*self, serializer)
    }
}

impl<'de> Deserialize<'de> for Color {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        deserialize_enum(deserializer)
    }
}

#[test]
fn enums() {
    assert_eq!(
        serde_json::to_value(Color::DarkBlue).unwrap(),
        json!("blue")
    );
    assert_eq!(
        serde_json::from_value::<Color>(json!("red")).unwrap(),
        Color::Red
    );
    assert_eq!(
        serde_json::from_value::<Color>(json!(1)).unwrap(),
        Color::DarkBlue
    );
    let error = serde_json::from_value::<Color>(json!("green")).unwrap_err();
    assert_eq!(
        error.to_string(),
        "invalid value: string \"green\", expected one of \"red\", \"blue\" or an ordinal"
    );
    assert!(serde_json::from_value::<Color>(json!(2)).is_err());
    assert!(serde_json::from_value::<Color>(json!(-1)).is_err());
}

#[test]
fn default_predicates() {
    assert!(is_default(&0_u32));
    assert!(!is_default(&1_u32));
    assert!(eq_u64::<{ u64::MAX }>(&u64::MAX));
    assert!(eq_bool::<true>(&true));
    assert!(eq_i8::<-3>(&-3));
    assert!(f64_bits_eq::<0>(&0.0));
    assert!(!f64_bits_eq::<0>(&-0.0));
    assert!(f32_bits_eq::<{ 1.5_f32.to_bits() }>(&1.5));
    assert!(enum_ordinal_eq::<_, 1>(&Color::DarkBlue));
}
