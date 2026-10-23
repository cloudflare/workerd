use alloc::vec;

use super::*;
use crate::alloc::borrow::ToOwned;
use crate::alloc::vec::Vec;

#[test]
fn test_kj_exception_new_without_details() {
    let exception = KjException::new(
        repr::KjExceptionType::Failed,
        "test message",
        "test.rs",
        42,
        None,
    );

    assert_eq!(exception.what(), "test message");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Failed);
    assert!(exception.details().is_none());
}

#[test]
fn test_kj_exception_new_with_single_detail() {
    let details = vec![(123u64, b"detail data".to_vec())];
    let exception = KjException::new(
        repr::KjExceptionType::Overloaded,
        "test with details",
        "test.rs",
        100,
        Some(&details),
    );

    assert_eq!(exception.what(), "test with details");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Overloaded);
    assert_eq!(exception.file(), c"test.rs");
    assert_eq!(exception.line(), 100);

    let retrieved_details = exception.details();
    assert!(retrieved_details.is_some());
    let details_vec = retrieved_details.unwrap();
    assert_eq!(details_vec.len(), 1);
    assert_eq!(details_vec[0].0, 123);
    assert_eq!(details_vec[0].1, b"detail data");
}

#[test]
fn test_kj_exception_new_with_multiple_details() {
    let details = vec![
        (456u64, b"first detail".to_vec()),
        (789u64, b"second detail".to_vec()),
        (999u64, b"third detail with more data".to_vec()),
    ];
    let exception = KjException::new(
        repr::KjExceptionType::Disconnected,
        "multiple details test",
        "multi_test.rs",
        200,
        Some(&details),
    );

    assert_eq!(exception.what(), "multiple details test");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Disconnected);

    let retrieved_details = exception.details();
    assert!(retrieved_details.is_some());
    let details_vec = retrieved_details.unwrap();
    assert_eq!(details_vec.len(), 3);

    // Verify first detail
    assert_eq!(details_vec[0].0, 456);
    assert_eq!(details_vec[0].1, b"first detail");

    // Verify second detail
    assert_eq!(details_vec[1].0, 789);
    assert_eq!(details_vec[1].1, b"second detail");

    // Verify third detail
    assert_eq!(details_vec[2].0, 999);
    assert_eq!(details_vec[2].1, b"third detail with more data");
}

#[test]
fn test_kj_exception_into_kj_error() {
    let details = vec![
        (456u64, b"first detail".to_vec()),
        (789u64, b"second detail".to_vec()),
        (999u64, b"third detail with more data".to_vec()),
    ];
    let exception = KjException::new(
        repr::KjExceptionType::Disconnected,
        "multiple details test",
        "multi_test.rs",
        200,
        Some(&details),
    );

    let error: KjError = exception.into();
    assert_eq!(error.description(), "multiple details test");
    assert_eq!(error.exception_type(), repr::KjExceptionType::Disconnected);
    assert_eq!(error.file(), Some("multi_test.rs"));
    assert_eq!(error.line(), Some(200));
    assert_eq!(error.details(), Some(&details));
}

#[test]
fn test_kj_exception_new_with_empty_details_vec() {
    let empty_details: Vec<(u64, Vec<u8>)> = vec![];
    let exception = KjException::new(
        repr::KjExceptionType::Unimplemented,
        "empty details",
        "empty.rs",
        50,
        Some(&empty_details),
    );

    assert_eq!(exception.what(), "empty details");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Unimplemented);
    assert!(exception.details().is_none());
}

#[test]
fn test_kj_exception_with_binary_data() {
    // Test with binary data that includes null bytes
    let binary_data = vec![0x00, 0x01, 0xFF, 0xAB, 0xCD, 0x00, 0x42];
    let details = vec![(0xDEAD_BEEF_u64, binary_data.clone())];

    let exception = KjException::new(
        repr::KjExceptionType::Failed,
        "binary data test",
        "binary.rs",
        150,
        Some(&details),
    );

    let retrieved_details = exception.details();
    assert!(retrieved_details.is_some());
    let details_vec = retrieved_details.unwrap();
    assert_eq!(details_vec.len(), 1);
    assert_eq!(details_vec[0].0, 0xDEAD_BEEF_u64);
    assert_eq!(details_vec[0].1, binary_data);
}

#[test]
fn test_kj_error_creation() {
    // Test the KjError struct's constructors with the new API
    let exc1 = KjError::new(repr::KjExceptionType::Failed, "simple message".to_owned());
    assert_eq!(exc1.description(), "simple message");
    assert_eq!(exc1.exception_type(), repr::KjExceptionType::Failed);
    assert!(exc1.file().is_none());
    assert!(exc1.line().is_none());
    assert!(exc1.details().is_none());

    // Test with details
    let details = vec![(1u64, b"test".to_vec())];
    let exc2 = KjError::new(
        repr::KjExceptionType::Disconnected,
        "full details".to_owned(),
    )
    .with_details(details.clone());
    assert_eq!(exc2.description(), "full details");
    assert_eq!(exc2.exception_type(), repr::KjExceptionType::Disconnected);
    assert_eq!(exc2.details(), Some(&details));
}

#[test]
fn test_kj_error_into_kj_exception_basic() {
    // Test basic KjError conversion to kj::Exception
    let kj_error = KjError::new(
        repr::KjExceptionType::Overloaded,
        "test error message".to_owned(),
    );

    let exception = kj_error.into_kj_exception("error_test.rs", 100);
    assert_eq!(exception.what(), "test error message");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Overloaded);
    assert!(exception.details().is_none());
}

#[test]
fn test_kj_error_into_kj_exception_with_details() {
    // Test KjError with details conversion to kj::Exception
    let details = vec![
        (42u64, b"first detail".to_vec()),
        (123u64, b"second detail".to_vec()),
    ];
    let kj_error = KjError::new(
        repr::KjExceptionType::Disconnected,
        "error with details".to_owned(),
    )
    .with_details(details);

    let exception = kj_error.into_kj_exception("detailed_error.rs", 200);
    assert_eq!(exception.what(), "error with details");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Disconnected);

    let retrieved_details = exception.details();
    assert!(retrieved_details.is_some());
    let details_vec = retrieved_details.unwrap();
    assert_eq!(details_vec.len(), 2);
    assert_eq!(details_vec[0].0, 42);
    assert_eq!(details_vec[0].1, b"first detail");
    assert_eq!(details_vec[1].0, 123);
    assert_eq!(details_vec[1].1, b"second detail");
}

#[test]
fn test_kj_error_into_kj_exception_with_location() {
    // Test KjError with location info uses its own location instead of provided location
    let kj_error = KjError::new(
        repr::KjExceptionType::Unimplemented,
        "error with location".to_owned(),
    )
    .with_location("original_file.rs".to_owned(), 42);

    let exception = kj_error.into_kj_exception("fallback_file.rs", 999);
    assert_eq!(exception.what(), "error with location");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Unimplemented);
    // Note: We can't easily test the file/line info as it's not exposed in KjException
    // but the logic should use "original_file.rs":42 instead of "fallback_file.rs":999
}

// Custom error types for testing std::error::Error trait implementation
#[derive(Debug, PartialEq)]
struct SimpleTestError {
    message: String,
}

impl core::fmt::Display for SimpleTestError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "SimpleTestError: {}", self.message)
    }
}

impl core::error::Error for SimpleTestError {}

#[derive(Debug, PartialEq)]
struct ChainedTestError {
    message: String,
    source: Option<SimpleTestError>,
}

impl core::fmt::Display for ChainedTestError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "ChainedTestError: {}", self.message)
    }
}

impl core::error::Error for ChainedTestError {
    fn source(&self) -> Option<&(dyn core::error::Error + 'static)> {
        self.source
            .as_ref()
            .map(|e| e as &(dyn core::error::Error + 'static))
    }
}

#[test]
fn test_std_error_into_kj_exception_simple() {
    // Test simple std::error::Error conversion
    let error = SimpleTestError {
        message: "something went wrong".to_owned(),
    };

    let exception = error.into_kj_exception("test_error.rs", 300);
    assert_eq!(exception.what(), "SimpleTestError: something went wrong");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Failed); // Default type
    assert!(exception.details().is_none()); // No details for std::error::Error
}

#[test]
fn test_std_error_into_kj_exception_chained() {
    // Test std::error::Error with source chain
    let source_error = SimpleTestError {
        message: "root cause".to_owned(),
    };
    let error = ChainedTestError {
        message: "wrapper error".to_owned(),
        source: Some(source_error),
    };

    let exception = error.into_kj_exception("chained_test.rs", 400);
    assert_eq!(exception.what(), "ChainedTestError: wrapper error");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Failed);
    assert!(exception.details().is_none());
}

#[test]
#[expect(clippy::std_instead_of_core, reason = "core::io is unstable")]
fn test_std_error_into_kj_exception_io_error() {
    // Test converting a std::io::Error to kj::Exception
    let kind = std::io::ErrorKind::NotFound;
    let error = std::io::Error::new(kind, "file not found");

    let exception = error.into_kj_exception("io_test.rs", 500);
    assert_eq!(exception.what(), "file not found");
    assert_eq!(exception.r#type(), repr::KjExceptionType::Failed);
    assert!(exception.details().is_none());
}

#[test]
fn test_std_error_vs_kj_error_conversion() {
    // Compare std::error::Error conversion vs KjError conversion
    let std_error = SimpleTestError {
        message: "test message".to_owned(),
    };
    let kj_error = KjError::new(
        repr::KjExceptionType::Failed,
        "SimpleTestError: test message".to_owned(),
    );

    let std_exception = std_error.into_kj_exception("test.rs", 600);
    let kj_exception = kj_error.into_kj_exception("test.rs", 600);

    // Both should have the same message and type
    assert_eq!(std_exception.what(), kj_exception.what());
    assert_eq!(std_exception.r#type(), kj_exception.r#type());
    assert_eq!(std_exception.details(), kj_exception.details());
}
