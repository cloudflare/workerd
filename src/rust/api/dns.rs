// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::iter::Peekable;
use std::str::Chars;

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
        // `length` is parsed from the hex token, so it can be as large as
        // usize::MAX. Use saturating_add so a length near the top of the
        // usize range can't wrap and silently bypass the bounds check.
        if length.saturating_add(offset_index) > input.len() {
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

/// Marks RDATA in the RFC 3597 generic format: `\# <length> <hex octets>`.
/// Anything else is in presentation format.
const GENERIC_RDATA_PREFIX: &str = "\\#";

fn unterminated_rdata() -> DnsParserError {
    DnsParserError::InvalidDnsResponse("unterminated escape or quote in RDATA".to_owned())
}

/// Decodes the escape sequence following a backslash, leaving the iterator
/// positioned after it. RFC 1035 §5.1 defines two forms: `\DDD`, exactly three
/// decimal digits naming an octet, and `\X`, a literal X.
///
/// An octet becomes the character with the same numeric value, matching how
/// `decode_hex` maps the octets of generic-format RDATA.
fn decode_escape(chars: &mut Peekable<Chars<'_>>) -> Result<char, DnsParserError> {
    let digits: Vec<char> = chars.clone().take(3).collect();
    if digits.len() == 3 && digits.iter().all(char::is_ascii_digit) {
        let octet: u8 = digits.iter().collect::<String>().parse()?;
        chars.nth(2);
        return Ok(char::from(octet));
    }

    chars.next().ok_or_else(unterminated_rdata)
}

/// Splits presentation-format RDATA into its fields. A field is either a bare
/// whitespace-delimited token or a `"`-quoted character-string; escape
/// sequences are decoded in both, so escaped whitespace does not end a field.
///
/// The master-file metacharacters `;`, `@` and `( )` are deliberately not
/// implemented: the input is a single RDATA field rather than a zone file, so
/// there is no comment or origin context. Treating `;` as a comment would
/// truncate CAA values carrying RFC 8657 parameters.
fn split_rdata_fields(input: &str) -> Result<Vec<String>, DnsParserError> {
    let mut fields = Vec::new();
    let mut chars = input.chars().peekable();
    loop {
        while chars.next_if(char::is_ascii_whitespace).is_some() {}

        let quoted = match chars.peek() {
            None => return Ok(fields),
            Some('"') => {
                chars.next();
                true
            }
            Some(_) => false,
        };

        let mut field = String::new();
        loop {
            let Some(c) = chars.next() else {
                if quoted {
                    return Err(unterminated_rdata());
                }
                break;
            };
            match c {
                '"' if quoted => break,
                '\\' => field.push(decode_escape(&mut chars)?),
                c if !quoted && c.is_ascii_whitespace() => break,
                c => field.push(c),
            }
        }
        fields.push(field);
    }
}

/// A CAA property tag must be one of "issue", "issuewild" or "iodef".
fn validate_caa_field(field: &str) -> Result<(), DnsParserError> {
    if field == "issuewild" || field == "issue" || field == "iodef" {
        return Ok(());
    }
    Err(DnsParserError::InvalidDnsResponse(format!(
        "Received unknown field '{field}'"
    )))
}

/// Parses CAA RDATA in presentation format: `<flags> <tag> <value>`,
/// e.g. `0 issue "pki.goog"`.
fn parse_presentation_caa_record(record: &str) -> Result<CaaRecord, DnsParserError> {
    let fields = split_rdata_fields(record)?;
    let [critical, field, value] = fields.as_slice() else {
        return Err(DnsParserError::InvalidDnsResponse(format!(
            "CAA record expected 3 fields, got {}",
            fields.len()
        )));
    };
    validate_caa_field(field)?;

    Ok(CaaRecord {
        critical: critical.parse()?,
        field: field.clone(),
        value: value.clone(),
    })
}

/// Parses NAPTR RDATA in presentation format:
/// `<order> <preference> "<flags>" "<service>" "<regexp>" <replacement>`,
/// e.g. `20 100 "s" "SIP+D2U" "" _sip._udp.sip2sip.info.`.
fn parse_presentation_naptr_record(record: &str) -> Result<NaptrRecord, DnsParserError> {
    let fields = split_rdata_fields(record)?;
    let [order, preference, flags, service, regexp, replacement] = fields.as_slice() else {
        return Err(DnsParserError::InvalidDnsResponse(format!(
            "NAPTR record expected 6 fields, got {}",
            fields.len()
        )));
    };

    Ok(NaptrRecord {
        flags: flags.clone(),
        service: service.clone(),
        regexp: regexp.clone(),
        // Presentation-format names are fully qualified, but Node.js reports
        // them without the trailing dot (and the root name as an empty string).
        replacement: replacement
            .strip_suffix('.')
            .unwrap_or(replacement)
            .to_owned(),
        order: order.parse()?,
        preference: preference.parse()?,
    })
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
    /// assert_eq!(record.critical, 0);
    /// assert_eq!(record.field, "issue");
    /// assert_eq!(record.value, "pki.goog");
    /// ```
    /// # Errors
    /// `DnsParserError::InvalidHexString`
    /// `DnsParserError::ParseIntError`
    #[jsg_method]
    pub fn parse_caa_record(&self, record: String) -> Result<CaaRecord, DnsParserError> {
        // Let's remove "\\#" and the length of data from the beginning of the record
        let parts: Vec<_> = record.split_ascii_whitespace().collect();
        if parts.first() != Some(&GENERIC_RDATA_PREFIX) {
            return parse_presentation_caa_record(&record);
        }
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
        let critical = u8::from_str_radix(data[0], 16)?;
        let prefix_length = usize::from_str_radix(data[1], 16)?;

        // `prefix_length` is attacker-supplied; saturate the addition so a
        // value near usize::MAX can't wrap past the bounds check.
        if data.len() < 2usize.saturating_add(prefix_length) {
            return Err(DnsParserError::InvalidDnsResponse(format!(
                "CAA record data too short for prefix_length {prefix_length}"
            )));
        }
        let field = decode_hex(&data[2..prefix_length + 2])?.join("");
        let value = decode_hex(&data[(prefix_length + 2)..])?.join("");

        validate_caa_field(&field)?;

        Ok(CaaRecord {
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
    #[jsg_method]
    pub fn parse_naptr_record(&self, record: String) -> jsg::Result<NaptrRecord, DnsParserError> {
        let parts: Vec<_> = record.split_ascii_whitespace().collect();
        if parts.first() != Some(&GENERIC_RDATA_PREFIX) {
            return parse_presentation_naptr_record(&record);
        }
        if parts.len() < 2 {
            return Err(DnsParserError::InvalidDnsResponse(
                "NAPTR record too short".to_owned(),
            ));
        }
        let data = parts[1..].to_vec();

        // Need at least: length(1) + order(2) + preference(2) + flag_length(1) = 6 fields
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
        let flag_offset = 6usize;
        // Length fields are parsed from attacker-controlled hex tokens, so
        // saturate every addition that could otherwise wrap past
        // `data.len()` and let the slice/index that follows panic.
        if data.len() < flag_offset.saturating_add(flag_length).saturating_add(1) {
            return Err(DnsParserError::InvalidDnsResponse(
                "NAPTR record too short for flags field".to_owned(),
            ));
        }
        let flags = decode_hex(&data[flag_offset..flag_length + flag_offset])?.join("");

        let service_length = usize::from_str_radix(data[flag_offset + flag_length], 16)?;
        let service_offset = flag_offset + flag_length + 1;
        if data.len() < service_offset.saturating_add(service_length).saturating_add(1) {
            return Err(DnsParserError::InvalidDnsResponse(
                "NAPTR record too short for service field".to_owned(),
            ));
        }
        let service = decode_hex(&data[service_offset..service_length + service_offset])?.join("");

        let regexp_length = usize::from_str_radix(data[service_offset + service_length], 16)?;
        let regexp_offset = service_offset + service_length + 1;
        if data.len() < regexp_offset.saturating_add(regexp_length) {
            return Err(DnsParserError::InvalidDnsResponse(
                "NAPTR record too short for regexp field".to_owned(),
            ));
        }
        let regexp = decode_hex(&data[regexp_offset..regexp_length + regexp_offset])?.join("");

        let replacement = parse_replacement(&data[regexp_offset + regexp_length..])?;

        Ok(NaptrRecord {
            flags,
            service,
            regexp,
            replacement,
            order,
            preference,
        })
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
    fn test_parse_caa_record_issuer_critical() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record("\\# 15 80 05 69 73 73 75 65 70 6b 69 2e 67 6f 6f 67".to_owned())
            .unwrap();

        // The issuer critical bit is the high bit of the flags octet, so the
        // hex octet `80` is 128. Reading it as decimal would give 80.
        assert_eq!(record.critical, 128);
        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "pki.goog");
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
    // Presentation-format RDATA. Cloudflare DNS serves CAA and NAPTR either as
    // RFC 3597 generic hex RDATA or in presentation format; both must parse.
    // =========================================================================

    #[test]
    fn test_parse_caa_record_presentation() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record("0 issue \"pki.goog\"".to_owned())
            .unwrap();

        assert_eq!(record.critical, 0);
        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "pki.goog");
    }

    #[test]
    fn test_parse_caa_record_presentation_critical() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record("128 iodef \"mailto:security@example.com\"".to_owned())
            .unwrap();

        assert_eq!(record.critical, 128);
        assert_eq!(record.field, "iodef");
        assert_eq!(record.value, "mailto:security@example.com");
    }

    #[test]
    fn test_parse_caa_record_presentation_value_with_space() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record(
                "0 issuewild \"letsencrypt.org; validationmethods=dns-01\"".to_owned(),
            )
            .unwrap();

        assert_eq!(record.field, "issuewild");
        assert_eq!(record.value, "letsencrypt.org; validationmethods=dns-01");
    }

    #[test]
    fn test_parse_caa_record_presentation_invalid_field() {
        let dns_util = DnsUtil {};
        assert!(
            dns_util
                .parse_caa_record("0 contactemail \"admin@example.com\"".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_caa_record_presentation_wrong_field_count() {
        let dns_util = DnsUtil {};
        assert!(dns_util.parse_caa_record("0 issue".to_owned()).is_err());
        assert!(
            dns_util
                .parse_caa_record("0 issue \"pki.goog\" extra".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_caa_record_presentation_unterminated_quote() {
        let dns_util = DnsUtil {};
        assert!(
            dns_util
                .parse_caa_record("0 issue \"pki.goog".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_naptr_record_presentation() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record("20 100 \"s\" \"SIP+D2U\" \"\" _sip._udp.sip2sip.info.".to_owned())
            .unwrap();

        assert_eq!(record.order, 20);
        assert_eq!(record.preference, 100);
        assert_eq!(record.flags, "s");
        assert_eq!(record.service, "SIP+D2U");
        assert_eq!(record.regexp, "");
        assert_eq!(record.replacement, "_sip._udp.sip2sip.info");
    }

    #[test]
    fn test_parse_naptr_record_presentation_regexp_and_root() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record(
                "100 10 \"u\" \"E2U+sip\" \"!^.*$!sip:info@example.com !\" .".to_owned(),
            )
            .unwrap();

        assert_eq!(record.order, 100);
        assert_eq!(record.preference, 10);
        assert_eq!(record.flags, "u");
        assert_eq!(record.service, "E2U+sip");
        assert_eq!(record.regexp, "!^.*$!sip:info@example.com !");
        assert_eq!(record.replacement, "");
    }

    #[test]
    fn test_parse_naptr_record_presentation_escaped_quote() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_naptr_record("1 2 \"u\" \"E2U+sip\" \"a\\\"b\" .".to_owned())
            .unwrap();

        assert_eq!(record.regexp, "a\"b");
    }

    #[test]
    fn test_parse_naptr_record_presentation_wrong_field_count() {
        let dns_util = DnsUtil {};
        assert!(
            dns_util
                .parse_naptr_record("20 100 \"s\" \"SIP+D2U\" \"\"".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_split_rdata_fields() {
        assert!(split_rdata_fields("").unwrap().is_empty());
        assert_eq!(
            split_rdata_fields("  a  \"b c\" \"\" d ").unwrap(),
            vec!["a", "b c", "", "d"]
        );
        assert!(split_rdata_fields("\"unterminated").is_err());
        assert!(split_rdata_fields("trailing\\").is_err());
    }

    #[test]
    fn test_split_rdata_fields_decimal_escapes() {
        // `\DDD` names an octet: \065 is 'A', \032 is a space.
        assert_eq!(split_rdata_fields("a\\065b").unwrap(), vec!["aAb"]);
        assert_eq!(split_rdata_fields("\"a\\065b\"").unwrap(), vec!["aAb"]);

        // Escaped whitespace does not terminate an unquoted field.
        assert_eq!(split_rdata_fields("x;\\032y z").unwrap(), vec!["x; y", "z"]);

        // The full octet range maps to the character with the same value, as
        // decode_hex does for generic-format RDATA.
        assert_eq!(
            split_rdata_fields("\\000\\255").unwrap(),
            vec!["\u{0}\u{ff}"]
        );

        // Out of octet range.
        assert!(split_rdata_fields("\\256").is_err());
    }

    #[test]
    fn test_split_rdata_fields_literal_escapes() {
        // Fewer than three digits is the literal form, not an octet.
        assert_eq!(split_rdata_fields("a\\6b").unwrap(), vec!["a6b"]);
        assert_eq!(split_rdata_fields("a\\65").unwrap(), vec!["a65"]);

        assert_eq!(split_rdata_fields("a\\\\b").unwrap(), vec!["a\\b"]);
        assert_eq!(split_rdata_fields("\"a\\\"b\"").unwrap(), vec!["a\"b"]);

        // An escaped space is literal, so it does not split the field.
        assert_eq!(split_rdata_fields("a\\ b").unwrap(), vec!["a b"]);
    }

    #[test]
    fn test_parse_caa_record_presentation_escaped_space() {
        let dns_util = DnsUtil {};
        let record = dns_util
            .parse_caa_record("0 issue ca.example.net;\\032account=1".to_owned())
            .unwrap();

        assert_eq!(record.field, "issue");
        assert_eq!(record.value, "ca.example.net; account=1");
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

    // The cases below set a hex length token to FFFFFFFFFFFFFFFF (usize::MAX
    // on 64-bit). Without saturating arithmetic in the bounds checks, the
    // addition wraps to a small value, the check passes, and the slice/index
    // that follows panics — they should return Err instead.

    #[test]
    fn test_parse_replacement_length_overflow() {
        // length token is usize::MAX on 64-bit; raw `length + offset_index`
        // wraps to 0, the bounds check passes, and `input[1..0]` panics.
        let input = vec!["FFFFFFFFFFFFFFFF", "01"];
        assert!(parse_replacement(&input).is_err());
    }

    #[test]
    fn test_parse_caa_record_prefix_length_overflow() {
        let dns_util = DnsUtil {};
        // critical=00, prefix_length=18446744073709551615 (usize::MAX as decimal)
        assert!(
            dns_util
                .parse_caa_record("\\# 02 00 18446744073709551615".to_owned())
                .is_err()
        );
    }

    #[test]
    fn test_parse_naptr_record_flag_length_overflow() {
        let dns_util = DnsUtil {};
        // 6 fields total; flag_length token = FFFFFFFFFFFFFFFF makes the
        // `flag_offset + flag_length + 1` check overflow.
        assert!(
            dns_util
                .parse_naptr_record(
                    "\\# 06 15 b3 08 ae FFFFFFFFFFFFFFFF".to_owned()
                )
                .is_err()
        );
    }

    #[test]
    fn test_parse_naptr_record_service_length_overflow() {
        let dns_util = DnsUtil {};
        // flag_length=01, then service_length token = FFFFFFFFFFFFFFFF.
        assert!(
            dns_util
                .parse_naptr_record(
                    "\\# 09 15 b3 08 ae 01 73 FFFFFFFFFFFFFFFF 00".to_owned()
                )
                .is_err()
        );
    }

    #[test]
    fn test_parse_naptr_record_regexp_length_overflow() {
        let dns_util = DnsUtil {};
        // flag_length=01, service_length=01, regexp_length=FFFFFFFFFFFFFFFF.
        assert!(
            dns_util
                .parse_naptr_record(
                    "\\# 0b 15 b3 08 ae 01 73 01 73 FFFFFFFFFFFFFFFF 00".to_owned()
                )
                .is_err()
        );
    }
}
