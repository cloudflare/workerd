use thiserror::Error;

#[derive(Debug, Error)]
pub enum DnsParserError {
    #[error("Invalid hex string: {0}")]
    InvalidHexString(String),
    #[error("ParseInt error: {0}")]
    ParseIntError(#[from] std::num::ParseIntError),
    #[error("Invalid DNS response: {0}")]
    InvalidDnsResponse(String),
    #[error("unknown dns parser error")]
    Unknown,
}

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
        fn parse_caa_record(record: &str) -> Result<CaaRecord>;
        fn parse_naptr_record(record: &str) -> Result<NaptrRecord>;
    }
}

/// Given a vector of strings, converts each slice to UTF-8 from HEX.
///
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn decode_hex(input: &[&str]) -> Result<Vec<String>, DnsParserError> {
    let mut v = Vec::with_capacity(input.len());

    for slice in input {
        let num = u16::from_str_radix(slice, 16)?;
        let ch = String::from_utf16(&[num])
            .map_err(|_| DnsParserError::InvalidHexString("Invalid UTF-16 sequence".to_owned()))?;
        v.push(ch);
    }

    Ok(v)
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
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn parse_caa_record(record: &str) -> Result<ffi::CaaRecord, DnsParserError> {
    // Let's remove "\\#" and the length of data from the beginning of the record
    let data = record.split_ascii_whitespace().collect::<Vec<_>>()[2..].to_vec();
    let critical = data[0].parse::<u8>()?;
    let prefix_length = data[1].parse::<usize>()?;

    let field = decode_hex(&data[2..prefix_length + 2])?.join("");
    let value = decode_hex(&data[(prefix_length + 2)..])?.join("");

    // Field can be "issuewild", "issue" or "iodef"
    if field != "issuewild" && field != "issue" && field != "iodef" {
        return Err(DnsParserError::InvalidDnsResponse(format!(
            "Received unknown field '{field}'"
        )));
    }

    Ok(ffi::CaaRecord {
        critical,
        field,
        value,
    })
}

/// Parses an unknown RR format returned from Cloudflare DNS.
/// Specification is available at
/// `<https://datatracker.ietf.org/doc/html/rfc3597>`
///
/// The format of the record is as follows:
/// \# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00
///       |--|  |--|  |  |  |  |--------------------------|  |  |--------------|  |  |--------------------------------|
///       |     |     |  |  |  |                             |  |                 |  - Replacement
///       |     |     |  |  |  |                             |  |                 - Length of first part of the replacement
///       |     |     |  |  |  |                             |  - Regexp
///       |     |     |  |  |  |                             - Regexp length
///       |     |     |  |  |  - Service
///       |     |     |  |  - Length of service
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
///
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn parse_naptr_record(record: &str) -> Result<ffi::NaptrRecord, DnsParserError> {
    let data = record.split_ascii_whitespace().collect::<Vec<_>>()[1..].to_vec();

    let order_str = data[1..3].to_vec();
    let order = u32::from_str_radix(&order_str.join(""), 16)?;
    let preference_str = data[3..5].to_vec();
    let preference = u32::from_str_radix(&preference_str.join(""), 16)?;

    let flag_length = usize::from_str_radix(data[5], 16)?;
    let flag_offset = 6;
    let flags = decode_hex(&data[flag_offset..flag_length + flag_offset])?.join("");

    let service_length = usize::from_str_radix(data[flag_offset + flag_length], 16)?;
    let service_offset = flag_offset + flag_length + 1;
    let service = decode_hex(&data[service_offset..service_length + service_offset])?.join("");

    let regexp_length = usize::from_str_radix(data[service_offset + service_length], 16)?;
    let regexp_offset = service_offset + service_length + 1;
    let regexp = decode_hex(&data[regexp_offset..regexp_length + regexp_offset])?.join("");

    let replacement = parse_replacement(&data[regexp_offset + regexp_length..])?;

    Ok(ffi::NaptrRecord {
        flags,
        service,
        regexp,
        replacement,
        order,
        preference,
    })
}

/// Replacement values needs to be parsed accordingly.
/// It has a similar characteristic to CAA and NAPTR records whereas
/// first character contains the length of the input, and the second character
/// is the starting index of the substring. We need to continue parsing until there
/// are no input left, and later join them using "."
///
/// It is important that the returning value doesn't end with dot (".") character.
///
/// # Errors
/// `DnsParserError::InvalidHexString`
/// `DnsParserError::ParseIntError`
pub fn parse_replacement(input: &[&str]) -> Result<String, DnsParserError> {
    if input.is_empty() {
        return Ok(String::new());
    }

    let mut output: Vec<String> = vec![];
    let mut length_index = 0;
    let mut offset_index = 1;

    // Iterate through each character to parse different frames.
    // Each frame starts with the length of the remaining frame.
    while length_index < input.len() {
        let length = usize::from_str_radix(input[length_index], 16)?;
        let subset = input[offset_index..length + offset_index].to_vec();
        let decoded = decode_hex(&subset)?.join("");

        // We omit the trailing "." from replacements.
        // Cloudflare DNS returns "_sip._udp.sip2sip.info." whereas Node.js removes trailing dot
        if !decoded.is_empty() {
            output.push(decoded);
        }

        length_index += subset.len() + 1;
        offset_index = length_index + 1;
    }

    Ok(output.join("."))
}
