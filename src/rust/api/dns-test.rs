// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
            "\\# 21 00 09 69 73 73 75 65 77 69 6c 64 6c 65 74 73 65 6e 63 72 79 70 74".to_owned(),
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
    let result = dns_util
        .parse_caa_record("\\# 15 00 05 69 6e 76 61 6c 69 64 70 6b 69 2e 67 6f 6f 67".to_owned());

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
        .parse_caa_record("0 issuewild \"letsencrypt.org; validationmethods=dns-01\"".to_owned())
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
