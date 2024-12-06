#[cxx::bridge(namespace = "workerd::rust::dns")]
mod ffi {
    /// CAA record representation
    struct CaaRecord {
        critical: u8,
        field: String,
        value: String,
    }
    /// NAPTR record representation
    struct NaptrRecord {
        flags: String,
        service: String,
        regexp: String,
        replacement: String,
        order: u32,
        preference: u32,
    }
    extern "Rust" {
        fn parse_caa_record(record: &str) -> CaaRecord;
        fn parse_naptr_record(record: &str) -> NaptrRecord;
    }
}

/// Given a vector of strings, converts each slice to UTF-8 from HEX.
///
/// # Panics
/// Will panic if any substring is not a valid hex.
#[must_use]
pub fn decode_hex(s: &[&str]) -> Vec<String> {
    s.iter()
        .map(|s| {
            u16::from_str_radix(s, 16)
                .ok()
                .map(|b| String::from_utf16(&[b]).unwrap())
                .unwrap()
        })
        .collect()
}

/// Parses an unknown RR format returned from Cloudflare DNS.
/// Specification is available at
/// `<https://datatracker.ietf.org/doc/html/rfc3597>`
///
/// The format of the record is as follows:
///   \# <length-in-bytes> <bytes-in-hex>
///   \\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67
///       |  |  |  |
///       |  |  |  - Starting point of the actual data
///       |  |  - Length of the field.
///       |  - Number representation of "`is_critical`"
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
    let critical = data[0].parse::<u8>().unwrap();
    let prefix_length = data[1].parse::<usize>().unwrap();

    let field = decode_hex(&data[2..prefix_length + 2]).join("");
    let value = decode_hex(&data[(prefix_length + 2)..]).join("");

    // Field can be "issuewild", "issue" or "iodef"
    assert!(
        field == "issuewild" || field == "issue" || field == "iodef",
        "received unsupported field {field}",
    );

    ffi::CaaRecord {
        critical,
        field,
        value,
    }
}

/// Parses an unknown RR format returned from Cloudflare DNS.
/// Specification is available at
/// `<https://datatracker.ietf.org/doc/html/rfc3597>`
///
/// The format of the record is as follows:
/// \# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00
///       |--|  |--|  |  |  |  |--------------------------|  |  |--------------|  |  |--------------------------------|
///       |     |     |  |  |  |                             |  |              |  |  - Replacement
///       |     |     |  |  |  |                             |  |              - Replacement length
///       |     |     |  |  |  |                             |  - Regexp
///       |     |     |  |  |  |                             - Regexp length
///       |     |     |  |  |  - Service
///       |     |     |  |  - The length of service
///       |     |     |  - Flag
///       |     |     - Length of flags
///       |     - Preference
///       - Order
///
/// ```
/// let record = parse_naptr_record("\\# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00");
/// assert_eq!(record.flags, "s");
/// assert_eq!(record.service, "my-service");
/// assert_eq!(record.regexp, "regexp");
/// assert_eq!(record.replacement, "replacement");
/// assert_eq!(record.order, 5555);
/// assert_eq!(record.preference, 2222);
/// ```
fn parse_naptr_record(record: &str) -> ffi::NaptrRecord {
    let data = record.split_ascii_whitespace().collect::<Vec<_>>()[1..].to_vec();

    let order_str = data[1..3].to_vec();
    let order = u32::from_str_radix(&order_str.join(""), 16).unwrap();
    let preference_str = data[3..5].to_vec();
    let preference = u32::from_str_radix(&preference_str.join(""), 16).unwrap();

    let flag_length = usize::from_str_radix(data[5], 16).unwrap();
    let flag_offset = 6;
    let flags = decode_hex(&data[flag_offset..flag_length + flag_offset]).join("");

    let service_length = usize::from_str_radix(data[flag_offset + flag_length], 16).unwrap();
    let service_offset = flag_offset + flag_length + 1;
    let service = decode_hex(&data[service_offset..service_length + service_offset]).join("");

    let regexp_length = usize::from_str_radix(data[service_offset + service_length], 16).unwrap();
    let regexp_offset = service_offset + service_length + 1;
    let regexp = decode_hex(&data[regexp_offset..regexp_length + regexp_offset]).join("");

    let replacement = parse_replacement(&data[regexp_offset + regexp_length..]);

    ffi::NaptrRecord {
        flags,
        service,
        regexp,
        replacement,
        order,
        preference,
    }
}

/// Replacement values needs to be parsed accordingly.
/// It has a similar characteristic to CAA and NAPTR records whereas
/// first character contains the length of the input, and the second character
/// is the starting index of the substring. We need to continue parsing until there
/// are no input left, and later join them using "."
///
/// It is important that the returning value doesn't end with dot (".") character.
fn parse_replacement(input: &[&str]) -> String {
    if input.is_empty() {
        return String::new();
    }

    let mut output: Vec<String> = vec![];
    let mut length_index = 0;
    let mut offset_index = 1;

    while length_index < input.len() {
        let length = usize::from_str_radix(input[length_index], 16).unwrap();
        let subset = input[offset_index..length + offset_index].to_vec();
        let decoded = decode_hex(&subset).join("");

        // We omit the trailing "." from replacements.
        // Cloudflare DNS returns "_sip._udp.sip2sip.info." whereas Node.js removes trailing dot
        if !decoded.is_empty() {
            output.push(decoded);
        }

        length_index += subset.len() + 1;
        offset_index = length_index + 1;
    }

    output.join(".")
}
