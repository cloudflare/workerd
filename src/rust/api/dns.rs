// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg_macros::jsg_method;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;
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

impl From<DnsParserError> for jsg::Error {
    fn from(val: DnsParserError) -> Self {
        match val {
            DnsParserError::InvalidHexString(msg) | DnsParserError::InvalidDnsResponse(msg) => {
                Self::new_error(&msg)
            }
            DnsParserError::ParseIntError(msg) => Self::new_range_error(msg.to_string()),
            DnsParserError::Unknown => Self::new_error("Unknown dns parser error"),
        }
    }
}

/// CAA record representation
#[jsg_struct]
#[derive(Debug)]
pub struct CaaRecord {
    pub critical: u8,
    pub field: String,
    pub value: String,
}

/// NAPTR record representation
#[jsg_struct]
#[derive(Debug)]
pub struct NaptrRecord {
    pub flags: String,
    pub service: String,
    pub regexp: String,
    pub replacement: String,
    pub order: u32,
    pub preference: u32,
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

/// Replacement values needs to be parsed accordingly.
///
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
pub fn parse_replacement(input: &[&str]) -> jsg::Result<String, DnsParserError> {
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
        if length + offset_index > input.len() {
            return Err(DnsParserError::InvalidDnsResponse(
                "replacement data too short for declared frame length".to_owned(),
            ));
        }
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


/// Splits the first whitespace-delimited token from `s`.
/// Returns `(token, rest)` where `rest` is trimmed of leading whitespace.
fn split_first_token(s: &str) -> Option<(&str, &str)> {
    let s = s.trim_start();
    if s.is_empty() { return None; }
    let end = s.find(|c: char| c.is_ascii_whitespace()).unwrap_or(s.len());
    Some((&s[..end], s[end..].trim_start()))
}

/// Extracts the next double-quoted token from `s`.
/// Returns `(content_without_quotes, rest)` trimmed of leading whitespace.
/// The quoted string may contain internal whitespace.
fn next_quoted(s: &str) -> Option<(&str, &str)> {
    let s = s.trim_start();
    if !s.starts_with('"') { return None; }
    let close = s[1..].find('"')? + 1;
    let content = &s[1..close];
    let rest = s[close + 1..].trim_start();
    Some((content, rest))
}

#[jsg_resource]
pub struct DnsUtil;

#[jsg_resource]
impl DnsUtil {
    pub fn new() -> jsg::Rc<Self> {
        jsg::Rc::new(Self {})
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
    /// Parses a CAA record. Supports both the RFC 3597 generic hex encoding
    /// (old format: `\# <length> <hex bytes>`) and the presentation format
    /// introduced in the 1.1.1.1 DoH JSON API rollout of 2026-07-28
    /// (new format: `0 issue "pki.goog"`).
    ///
    /// CAA values can contain whitespace (e.g. `"digicert.com; params=yes"`),
    /// so the value is extracted with surrounding quotes stripped rather than
    /// whitespace-split.
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        let trimmed = record.trim();

        if trimmed.starts_with("\\#") {
            // RFC 3597 generic hex: \# <length> <hex bytes...>
            let parts: Vec<_> = trimmed.split_ascii_whitespace().collect();
            if parts.len() < 3 {
                return Err(DnsParserError::InvalidDnsResponse(
                    "CAA record too short: expected at least 3 fields".to_owned(),
                ));
            }
            let data = parts[2..].to_vec();
            if data.len() < 2 {
                return Err(DnsParserError::InvalidDnsResponse(
                    "CAA record data too short: expected critical and prefix length fields".to_owned(),
                ));
            }
            let critical = data[0].parse::<u8>()?;
            let prefix_length = data[1].parse::<usize>()?;
            if data.len() < 2 + prefix_length {
                return Err(DnsParserError::InvalidDnsResponse(format!(
                    "CAA record data too short for prefix_length {prefix_length}"
                )));
            }
            let field = decode_hex(&data[2..prefix_length + 2])?.join("");
            let value = decode_hex(&data[(prefix_length + 2)..])?.join("");
            if field != "issuewild" && field != "issue" && field != "iodef" {
                return Err(DnsParserError::InvalidDnsResponse(format!(
                    "Received unknown field '{field}'"
                )));
            }
            Ok(CaaRecord { critical, field, value })
        } else {
            // Presentation format: <critical> <tag> "<value>"
            // Value may contain whitespace, so parse tokens manually.
            let (critical_str, rest) = split_first_token(trimmed).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse(
                    "CAA presentation format: missing critical field".to_owned(),
                )
            })?;
            let critical = critical_str.parse::<u8>()?;

            let (field, rest) = split_first_token(rest).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse(
                    "CAA presentation format: missing tag field".to_owned(),
                )
            })?;
            let field = field.to_owned();

            let value_raw = rest.trim();
            let value = if value_raw.len() >= 2 && value_raw.starts_with('"') && value_raw.ends_with('"') {
                value_raw[1..value_raw.len() - 1].to_owned()
            } else {
                value_raw.to_owned()
            };

            if field != "issuewild" && field != "issue" && field != "iodef" {
                return Err(DnsParserError::InvalidDnsResponse(format!(
                    "Received unknown field '{field}'"
                )));
            }
            Ok(CaaRecord { critical, field, value })
        }
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
    /// Parses a NAPTR record. Supports both the RFC 3597 generic hex encoding
    /// and the presentation format introduced in the 1.1.1.1 DoH JSON API
    /// rollout of 2026-07-28 (e.g. `100 10 "s" "SIP+D2U" "" _sip._udp.example.com.`).
    ///
    /// The replacement trailing dot is stripped to match Node.js behaviour.
    #[jsg_method]
    pub fn parse_naptr_record(&self, record: String) -> jsg::Result<NaptrRecord, DnsParserError> {
        let trimmed = record.trim();

        if trimmed.starts_with("\\#") {
            // RFC 3597 generic hex encoding.
            let parts: Vec<_> = trimmed.split_ascii_whitespace().collect();
            if parts.len() < 2 {
                return Err(DnsParserError::InvalidDnsResponse("NAPTR record too short".to_owned()));
            }
            let data = parts[1..].to_vec();
            if data.len() < 6 {
                return Err(DnsParserError::InvalidDnsResponse(
                    "NAPTR record data too short: expected at least 6 fields".to_owned(),
                ));
            }
            let order_str = data[1..3].to_vec();
            let order = u32::from_str_radix(&order_str.join(""), 16)?;
            let preference_str = data[3..5].to_vec();
            let preference = u32::from_str_radix(&preference_str.join(""), 16)?;
            let flag_length = usize::from_str_radix(data[5], 16)?;
            let flag_offset = 6;
            if data.len() < flag_offset + flag_length + 1 {
                return Err(DnsParserError::InvalidDnsResponse(
                    "NAPTR record too short for flags field".to_owned(),
                ));
            }
            let flags = decode_hex(&data[flag_offset..flag_length + flag_offset])?.join("");
            let service_length = usize::from_str_radix(data[flag_offset + flag_length], 16)?;
            let service_offset = flag_offset + flag_length + 1;
            if data.len() < service_offset + service_length + 1 {
                return Err(DnsParserError::InvalidDnsResponse(
                    "NAPTR record too short for service field".to_owned(),
                ));
            }
            let service = decode_hex(&data[service_offset..service_length + service_offset])?.join("");
            let regexp_length = usize::from_str_radix(data[service_offset + service_length], 16)?;
            let regexp_offset = service_offset + service_length + 1;
            if data.len() < regexp_offset + regexp_length {
                return Err(DnsParserError::InvalidDnsResponse(
                    "NAPTR record too short for regexp field".to_owned(),
                ));
            }
            let regexp = decode_hex(&data[regexp_offset..regexp_length + regexp_offset])?.join("");
            let replacement = parse_replacement(&data[regexp_offset + regexp_length..])?;
            Ok(NaptrRecord { flags, service, regexp, replacement, order, preference })
        } else {
            // Presentation format: <order> <preference> "<flags>" "<service>" "<regexp>" <replacement>
            let (order_str, rest) = split_first_token(trimmed).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse("NAPTR presentation format: missing order".to_owned())
            })?;
            let order = order_str.parse::<u32>()?;

            let (pref_str, rest) = split_first_token(rest).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse("NAPTR presentation format: missing preference".to_owned())
            })?;
            let preference = pref_str.parse::<u32>()?;

            let (flags, rest) = next_quoted(rest).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse("NAPTR presentation format: missing or malformed flags field".to_owned())
            })?;
            let (service, rest) = next_quoted(rest).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse("NAPTR presentation format: missing or malformed service field".to_owned())
            })?;
            let (regexp, rest) = next_quoted(rest).ok_or_else(|| {
                DnsParserError::InvalidDnsResponse("NAPTR presentation format: missing or malformed regexp field".to_owned())
            })?;

            // Strip trailing dot from replacement (DNS presentation convention).
            let replacement_raw = rest.trim();
            let replacement = if replacement_raw == "." {
                String::new()
            } else if replacement_raw.ends_with('.') {
                replacement_raw[..replacement_raw.len() - 1].to_owned()
            } else {
                replacement_raw.to_owned()
            };

            Ok(NaptrRecord {
                flags: flags.to_owned(),
                service: service.to_owned(),
                regexp: regexp.to_owned(),
                replacement,
                order,
                preference,
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_decode() {
        let input = vec!["69", "73", "73", "75", "65"];
        assert_eq!(decode_hex(&input).unwrap().join(""), "issue");

        let empty_input: Vec<&str> = vec![];
        assert!(decode_hex(&empty_input).unwrap().is_empty());
    }

    #[test]
    fn test_decode_hex_invalid() {
        let input = vec!["ZZ"];
        let result = decode_hex(&input);
        assert!(result.is_err());
    }

    #[test]
    fn test_parse_replacement_empty() {
        let input: Vec<&str> = vec![];
        assert_eq!(parse_replacement(&input).unwrap(), "");

        let multiple_parts_input = vec!["03", "73", "69", "70", "04", "74", "65", "73", "74", "00"];
        assert_eq!(
            parse_replacement(&multiple_parts_input).unwrap(),
            "sip.test"
        );
    }

    #[test]
    fn test_parse_caa_record_issue() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record("\\# 15 00 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67".to_owned())
            .unwrap();

        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "pki.goog");
    }

    #[test]
    fn test_parse_caa_record_issuewild() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record(
                "\\# 21 00 09 69 73 73 75 65 77 69 6c 64 6c 65 74 73 65 6e 63 72 79 70 74"
                    .to_owned(),
            )
            .unwrap();

        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issuewild");
        assert_eq!(record.value, "letsencrypt");
    }

    #[test]
    fn test_parse_caa_record_invalid_field() {
        let dns_util = DnsUtil {};
        let result = dns_util.parse_caa_record(
            "\\# 15 00 05 69 6e 76 61 6c 69 64 70 6b 69 2e 67 6f 6f 67".to_owned(),
        );

        assert!(result.is_err());
    }

    #[test]
    fn test_parse_naptr_record() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record("\\# 37 15 b3 08 ae 01 73 0a 6d 79 2d 73 65 72 76 69 63 65 06 72 65 67 65 78 70 0b 72 65 70 6c 61 63 65 6d 65 6e 74 00".to_owned())
            .unwrap();

        assert_eq!(record.flags, "s");
        assert_eq!(record.service, "my-service");
        assert_eq!(record.regexp, "regexp");
        assert_eq!(record.replacement, "replacement");
        assert_eq!(record.order, 5555);
        assert_eq!(record.preference, 2222);
    }

    // =========================================================================
    // Malformed input tests — these previously caused panics (index out of bounds)
    // which would abort the process via CXX. They must return Err, not panic.
    // =========================================================================

    #[test]
    fn test_parse_caa_record_empty_string() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_caa_record(String::new()).is_err());
    }

    #[test]
    fn test_parse_caa_record_single_token() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_caa_record("\\#".to_owned()).is_err());
    }

    #[test]
    fn test_parse_caa_record_two_tokens() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_caa_record("\\# 15".to_owned()).is_err());
    }

    #[test]
    fn test_parse_caa_record_data_too_short_for_prefix() {
        let dns_util = DnsUtil {};
        // critical=00, prefix_length=FF (255) but no data follows
        assert!(
            dns_util
                .parse_caa_record("\\# 02 00 FF".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_naptr_record_empty_string() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_naptr_record(String::new()).is_err());
    }

    #[test]
    fn test_parse_naptr_record_single_token() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_naptr_record("\\#".to_owned()).is_err());
    }

    #[test]
    fn test_parse_naptr_record_too_few_fields() {
        let dns_util = DnsUtil {};
        assert!(
            dns_util
                .parse_naptr_record("\\# 37 15 b3".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_replacement_length_exceeds_input() {
        // First element says frame is FF (255) bytes but only 2 bytes follow
        let input = vec!["FF", "73", "69"];
        assert!(parse_replacement(&input).is_err());
    }

    #[test]
    fn test_parse_naptr_record_truncated_at_flags() {
        let dns_util = DnsUtil {};
        // Has order+preference+flag_length but no flag data
        assert!(
            dns_util
                .parse_naptr_record("\\# 06 15 b3 08 ae 05".to_owned())
                .is_err()
        );
    }
    // --- CAA presentation format (post 2026-07-28) ---

    #[test]
    fn test_parse_caa_record_presentation_simple() {
        let dns_util = DnsUtil {};
        let record = dns_util.parse_caa_record(r#"0 issue "pki.goog""#.to_owned()).unwrap();
        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "pki.goog");
    }

    #[test]
    fn test_parse_caa_record_presentation_value_with_whitespace() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record(r#"0 issue "digicert.com; cansignhttpexchanges=yes""#.to_owned())
            .unwrap();
        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "digicert.com; cansignhttpexchanges=yes");
    }

    #[test]
    fn test_parse_caa_record_presentation_issuewild() {
        let dns_util = DnsUtil {};
        let record = dns_util.parse_caa_record(r#"0 issuewild "letsencrypt.org""#.to_owned()).unwrap();
        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issuewild");
        assert_eq!(record.value, "letsencrypt.org");
    }

    #[test]
    fn test_parse_caa_record_presentation_invalid_tag() {
        assert!(DnsUtil {}.parse_caa_record(r#"0 badtag "example.com""#.to_owned()).is_err());
    }

    // --- NAPTR presentation format (post 2026-07-28) ---

    #[test]
    fn test_parse_naptr_record_presentation_basic() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record(r#"100 10 "s" "SIP+D2U" "" _sip._udp.example.com."#.to_owned())
            .unwrap();
        assert_eq!(record.order, 100);
        assert_eq!(record.preference, 10);
        assert_eq!(record.flags, "s");
        assert_eq!(record.service, "SIP+D2U");
        assert_eq!(record.regexp, "");
        assert_eq!(record.replacement, "_sip._udp.example.com");
    }

    #[test]
    fn test_parse_naptr_record_presentation_with_regexp() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record(r#"100 10 "u" "E2U+sip" "!^.*$!sip:info@example.com!i" ."#.to_owned())
            .unwrap();
        assert_eq!(record.order, 100);
        assert_eq!(record.flags, "u");
        assert_eq!(record.regexp, "!^.*$!sip:info@example.com!i");
        assert_eq!(record.replacement, "");
    }

}
