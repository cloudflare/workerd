#[cxx::bridge(namespace = "workerd::rust::dns")]
mod ffi {
    /// CAA record representation
    struct CaaRecord {
        critical: u8,
        field: String,
        value: String,
    }
    extern "Rust" {
        fn parse_caa_record(record: &str) -> CaaRecord;
    }
}

/// Given a vector of strings, converts each slice to UTF-8 from HEX.
pub fn decode_hex(s: Vec<&str>) -> Vec<String> {
    s.into_iter()
        .map(|s| {
            u8::from_str_radix(s, 16)
                .ok()
                .map(|b| String::from_utf8(vec![b]).unwrap_or_default())
                .unwrap()
        })
        .collect()
}

/// Parses an unknown RR format returned from Cloudflare DNS.
/// Specification is available at
/// https://datatracker.ietf.org/doc/html/rfc3597
///
/// The format of the record is as follows:
///   \# <length-in-bytes> <bytes-in-hex>
///   \\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67
///       |  |  |  |
///       |  |  |  - Starting point of the actual data
///       |  |  - Length of the field.
///       |  - Number representation of "is_critical"
///       - Length of the data
///
/// Note: Field can be "issuewild", "issue" or "iodef".
///
/// ```
/// let record = parse_caa_record("\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67");
/// assert_eq!(record.critical, false);
/// assert_eq!(record.field, "issue")
/// assert_eq!(record.value, "pki.goog")
/// ```
fn parse_caa_record(record: &str) -> ffi::CaaRecord {
    // Let's remove "\\#" and the length of data from the beginning of the record
    let data = record.split_ascii_whitespace().collect::<Vec<_>>()[2..].to_vec();
    let critical = u8::from_str_radix(data[0], 10).unwrap();
    let prefix_length = usize::from_str_radix(data[1], 10).unwrap();

    let field = decode_hex(data[2..prefix_length + 2].to_vec()).join("");
    let value = decode_hex(data[(prefix_length + 2)..].to_vec()).join("");

    // Field can be "issuewild", "issue" or "iodef"
    assert!(
        field == "issuewild" || field == "issue" || field == "iodef",
        "received unsupported field {}",
        field
    );

    ffi::CaaRecord {
        critical,
        field,
        value,
    }
}
