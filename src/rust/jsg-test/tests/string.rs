// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use jsg::v8::Local;
use jsg::v8::MaybeLocal;
use jsg::v8::String as JsString;
use jsg::v8::ToLocalValue;
use jsg::v8::Utf8Value;
use jsg::v8::Value;
use jsg::v8::WriteFlags;

// Convenience: create a Local<String> from a UTF-8 str literal.
fn from_utf8<'a>(lock: &mut jsg::Lock, s: &str) -> Local<'a, JsString> {
    JsString::new_from_utf8(lock, s.as_bytes()).unwrap(lock)
}

// =============================================================================
// Local<String> — static constructors
// =============================================================================

#[test]
fn string_empty_has_zero_length() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = JsString::empty(lock);
        assert_eq!(s.length(), 0);
        assert!(s.is_one_byte());
        Ok(())
    });
}

#[test]
fn string_new_from_utf8_roundtrips_ascii() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let maybe = JsString::new_from_utf8(lock, b"hello");
        assert!(!maybe.is_empty());
        let s = maybe.unwrap(lock);
        assert_eq!(s.length(), 5);
        assert_eq!(s.to_string(lock), "hello");
        Ok(())
    });
}

#[test]
fn string_new_from_utf8_roundtrips_unicode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "こんにちは";
        let s = from_utf8(lock, input);
        assert_eq!(s.to_string(lock), input);
        Ok(())
    });
}

#[test]
fn string_new_from_utf8_empty_slice() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let maybe = JsString::new_from_utf8(lock, b"");
        assert!(!maybe.is_empty());
        let s = maybe.unwrap(lock);
        assert_eq!(s.length(), 0);
        Ok(())
    });
}

#[test]
fn string_new_from_one_byte_roundtrips() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let maybe = JsString::new_from_one_byte(lock, b"latin1");
        assert!(!maybe.is_empty());
        let s = maybe.unwrap(lock);
        assert_eq!(s.length(), 6);
        assert!(s.is_one_byte());
        assert_eq!(s.to_string(lock), "latin1");
        Ok(())
    });
}

#[test]
fn string_new_from_two_byte_roundtrips() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let utf16: Vec<u16> = "hello".encode_utf16().collect();
        let maybe = JsString::new_from_two_byte(lock, &utf16);
        assert!(!maybe.is_empty());
        let s = maybe.unwrap(lock);
        assert_eq!(s.length(), 5);
        assert_eq!(s.to_string(lock), "hello");
        Ok(())
    });
}

#[test]
fn string_new_from_two_byte_unicode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "日本語";
        let utf16: Vec<u16> = input.encode_utf16().collect();
        let s = JsString::new_from_two_byte(lock, &utf16).unwrap(lock);
        assert_eq!(s.to_string(lock), input);
        Ok(())
    });
}

#[test]
fn string_new_from_utf8_internalized_roundtrips() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s1 = JsString::new_internalized_from_utf8(lock, b"intern_me").unwrap(lock);
        let s2 = JsString::new_internalized_from_utf8(lock, b"intern_me").unwrap(lock);
        // Both strings should have the same content.
        assert_eq!(s1.to_string(lock), "intern_me");
        assert_eq!(s2.to_string(lock), "intern_me");
        // Internalized strings with equal content compare equal.
        assert_eq!(s1, s2);
        Ok(())
    });
}

#[test]
fn string_new_from_one_byte_internalized_roundtrips() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = JsString::new_internalized_from_one_byte(lock, b"latin1_intern").unwrap(lock);
        assert_eq!(s.to_string(lock), "latin1_intern");
        assert!(s.is_one_byte());
        Ok(())
    });
}

#[test]
fn string_new_from_two_byte_internalized_roundtrips() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let utf16: Vec<u16> = "two_byte_intern".encode_utf16().collect();
        let s = JsString::new_internalized_from_two_byte(lock, &utf16).unwrap(lock);
        assert_eq!(s.to_string(lock), "two_byte_intern");
        Ok(())
    });
}

// =============================================================================
// Local<String> — instance methods
// =============================================================================

#[test]
fn string_length_matches_utf16_code_units() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        // ASCII: 1 byte per char = 1 UTF-16 code unit
        assert_eq!(from_utf8(lock, "hello").length(), 5);
        // Each BMP kanji = 1 UTF-16 code unit
        assert_eq!(from_utf8(lock, "日本語").length(), 3);
        Ok(())
    });
}

#[test]
fn string_is_one_byte_for_ascii() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "ascii only");
        assert!(s.is_one_byte());
        assert!(s.contains_only_one_byte());
        Ok(())
    });
}

#[test]
fn string_is_not_one_byte_for_multibyte() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "こんにちは");
        assert!(!s.is_one_byte());
        assert!(!s.contains_only_one_byte());
        Ok(())
    });
}

#[test]
fn string_utf8_length_matches_byte_count() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let ascii = "hello";
        assert_eq!(from_utf8(lock, ascii).utf8_length(lock), ascii.len());

        let unicode = "こんにちは";
        assert_eq!(from_utf8(lock, unicode).utf8_length(lock), unicode.len());
        Ok(())
    });
}

// =============================================================================
// write / write_one_byte / write_utf8
// =============================================================================

#[test]
fn string_write_utf16_into_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "hi");
        let mut buf = vec![0u16; 2];
        s.write(lock, 0, 2, &mut buf, WriteFlags::None);
        assert_eq!(buf, vec!['h' as u16, 'i' as u16]);
        Ok(())
    });
}

#[test]
fn string_write_utf16_with_offset() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "hello");
        let mut buf = vec![0u16; 3];
        s.write(lock, 2, 3, &mut buf, WriteFlags::None);
        assert_eq!(buf, vec!['l' as u16, 'l' as u16, 'o' as u16]);
        Ok(())
    });
}

#[test]
fn string_write_one_byte_into_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "abc");
        let mut buf = vec![0u8; 3];
        s.write_one_byte(lock, 0, 3, &mut buf, WriteFlags::None);
        assert_eq!(buf, b"abc");
        Ok(())
    });
}

#[test]
fn string_write_utf8_into_buffer() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "hello";
        let s = from_utf8(lock, input);
        let mut buf = vec![0u8; input.len()];
        let written = s.write_utf8(lock, &mut buf, WriteFlags::None);
        assert_eq!(written, input.len());
        assert_eq!(&buf, input.as_bytes());
        Ok(())
    });
}

#[test]
fn string_write_utf8_null_terminate() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "hi";
        let s = from_utf8(lock, input);
        let mut buf = vec![0u8; input.len() + 1];
        let written = s.write_utf8(lock, &mut buf, WriteFlags::NullTerminate);
        assert_eq!(written, input.len() + 1);
        assert_eq!(buf[input.len()], 0);
        Ok(())
    });
}

#[test]
fn string_to_string_roundtrips_ascii() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "Hello, world!";
        assert_eq!(from_utf8(lock, input).to_string(lock), input);
        Ok(())
    });
}

#[test]
fn string_to_string_roundtrips_unicode() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "🦀 Rust";
        assert_eq!(from_utf8(lock, input).to_string(lock), input);
        Ok(())
    });
}

// =============================================================================
// PartialEq / Eq
// =============================================================================

#[test]
fn string_eq_same_content() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let a = from_utf8(lock, "equal");
        let b = from_utf8(lock, "equal");
        assert_eq!(a, b);
        Ok(())
    });
}

#[test]
fn string_ne_different_content() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let a = from_utf8(lock, "foo");
        let b = from_utf8(lock, "bar");
        assert_ne!(a, b);
        Ok(())
    });
}

#[test]
fn string_empty_eq_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let a = JsString::empty(lock);
        let b = JsString::empty(lock);
        assert_eq!(a, b);
        Ok(())
    });
}

// =============================================================================
// MaybeLocal
// =============================================================================

#[test]
fn maybe_local_is_not_empty_on_success() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let maybe = JsString::new_from_utf8(lock, b"hello");
        assert!(!maybe.is_empty());
        Ok(())
    });
}

#[test]
fn maybe_local_into_option_some() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let maybe = JsString::new_from_utf8(lock, b"test");
        let opt = maybe.into_option(lock);
        assert!(opt.is_some());
        Ok(())
    });
}

#[test]
fn maybe_local_from_none_is_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let _ = lock; // ensure we're inside the context
        let maybe: MaybeLocal<'_, JsString> = None::<Local<'_, JsString>>.into();
        assert!(maybe.is_empty());
        Ok(())
    });
}

#[test]
fn maybe_local_unwrap_or_returns_default_when_empty() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let default = JsString::empty(lock);
        let maybe: MaybeLocal<'_, JsString> = None::<Local<'_, JsString>>.into();
        let result = maybe.unwrap_or(lock, default);
        assert_eq!(result.length(), 0);
        Ok(())
    });
}

// =============================================================================
// WriteFlags
// =============================================================================

#[test]
fn write_flags_none_is_zero() {
    assert_eq!(WriteFlags::None.bits(), 0);
}

#[test]
fn write_flags_null_terminate_is_one() {
    assert_eq!(WriteFlags::NullTerminate.bits(), 1);
}

#[test]
fn write_flags_replace_invalid_utf8_is_two() {
    assert_eq!(WriteFlags::ReplaceInvalidUtf8.bits(), 2);
}

#[test]
fn write_flags_bitor_combines_values() {
    let combined = WriteFlags::NullTerminate | WriteFlags::ReplaceInvalidUtf8;
    assert_eq!(combined.bits(), 3);
    assert_eq!(combined, WriteFlags::NullTerminateAndReplaceInvalidUtf8);
}

#[test]
fn write_flags_combined_variant_equals_bitor() {
    assert_eq!(
        WriteFlags::NullTerminateAndReplaceInvalidUtf8.bits(),
        WriteFlags::NullTerminate.bits() | WriteFlags::ReplaceInvalidUtf8.bits()
    );
}

// =============================================================================
// Utf8Value
// =============================================================================

#[test]
fn utf8_value_length_matches_byte_count() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = "hello".to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.length(), 5);
        Ok(())
    });
}

#[test]
fn utf8_value_as_str_returns_correct_content() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = "hello world".to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.as_str(), Some("hello world"));
        Ok(())
    });
}

#[test]
fn utf8_value_as_bytes_matches_utf8_encoding() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "Rust 🦀";
        let val: Local<'_, Value> = input.to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.as_bytes(), input.as_bytes());
        Ok(())
    });
}

#[test]
fn utf8_value_from_unicode_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let input = "こんにちは";
        let val: Local<'_, Value> = input.to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.length(), input.len());
        assert_eq!(utf8.as_str(), Some(input));
        Ok(())
    });
}

#[test]
fn utf8_value_from_empty_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = "".to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.length(), 0);
        assert_eq!(utf8.as_str(), Some(""));
        Ok(())
    });
}

#[test]
fn utf8_value_as_ptr_is_non_null() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = "test".to_local(lock);
        let utf8 = Utf8Value::new(lock, &val);
        assert!(!utf8.as_ptr().is_null());
        Ok(())
    });
}

#[test]
fn utf8_value_coerces_number_to_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, ctx| {
        let val = ctx.eval_raw("42").map_err(|e| e.unwrap_jsg_err(lock))?;
        let utf8 = Utf8Value::new(lock, &val);
        assert_eq!(utf8.as_str(), Some("42"));
        Ok(())
    });
}

// =============================================================================
// Local<String> cast from/to Local<Value>
// =============================================================================

#[test]
fn string_try_as_from_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = "cast me".to_local(lock);
        let s = val.try_as::<JsString>().expect("should cast to String");
        assert_eq!(s.to_string(lock), "cast me");
        Ok(())
    });
}

#[test]
fn string_try_as_fails_for_non_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let val: Local<'_, Value> = jsg::Number::new(42.0).to_local(lock);
        assert!(val.try_as::<JsString>().is_none());
        Ok(())
    });
}

#[test]
fn string_upcast_to_value() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "upcast");
        let val: Local<'_, Value> = s.into();
        assert!(val.is_string());
        Ok(())
    });
}

// =============================================================================
// String::concat, internalize, get_identity_hash, is_flat, K_MAX_LENGTH
// =============================================================================

#[test]
fn string_concat_produces_concatenated_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let left = from_utf8(lock, "hello, ");
        let right = from_utf8(lock, "world");
        let result = JsString::concat(lock, left, right);
        assert_eq!(result.to_string(lock), "hello, world");
        Ok(())
    });
}

#[test]
fn string_concat_with_empty_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "abc");
        let empty = JsString::empty(lock);
        let result = JsString::concat(lock, s, empty);
        assert_eq!(result.to_string(lock), "abc");
        Ok(())
    });
}

#[test]
fn string_internalize_returns_equal_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "intern-me");
        let interned = s.internalize(lock);
        assert_eq!(interned.to_string(lock), "intern-me");
        Ok(())
    });
}

#[test]
fn string_internalize_twice_returns_equal_content() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "dedup");
        let a = s.internalize(lock);
        let b = s.internalize(lock);
        assert_eq!(a.to_string(lock), b.to_string(lock));
        Ok(())
    });
}

#[test]
fn string_is_flat_for_simple_string() {
    let harness = crate::Harness::new();
    harness.run_in_context(|lock, _ctx| {
        let s = from_utf8(lock, "flat");
        assert!(s.is_flat());
        Ok(())
    });
}

#[test]
fn string_max_length_matches_v8() {
    // V8's kMaxLength is (1<<28)-16 on 32-bit and (1<<29)-24 on 64-bit.
    #[cfg(target_pointer_width = "32")]
    assert_eq!(JsString::MAX_LENGTH, (1 << 28) - 16);
    #[cfg(target_pointer_width = "64")]
    assert_eq!(JsString::MAX_LENGTH, (1 << 29) - 24);
}
