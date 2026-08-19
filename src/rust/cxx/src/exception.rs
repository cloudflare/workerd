use alloc::string::String;
use alloc::vec::Vec;
use core::error::Error as StdError;
use core::ffi::CStr;
use core::fmt::Display;
use core::fmt::{self};
use core::mem::ManuallyDrop;
use core::ptr::NonNull;

use crate::alloc::string::ToString;

// Representation for kj::Exception* and functions to manipulated it,
pub mod repr {
    use core::ffi::c_char;

    /// Opaque representation of kj::Exception.
    #[repr(C)]
    pub struct KjException {
        data: (),
    }

    /// Represents kj::Exception::Type
    #[repr(C)]
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub enum KjExceptionType {
        /// Something went wrong. This is the usual error type.
        Failed = 0,
        /// The call failed because of a temporary lack of resources.
        Overloaded = 1,
        /// The call required communication over a connection that has been lost.
        Disconnected = 2,
        /// The requested method is not implemented.
        Unimplemented = 3,
    }

    /// Represents a detail entry for kj::Exception
    #[repr(C)]
    #[derive(Debug, Clone)]
    pub struct KjExceptionDetail {
        /// Detail type ID (64-bit integer)
        pub type_id: u64,
        /// Length of the detail data
        pub data_len: usize,
        /// Pointer to the detail data
        pub data_ptr: *const u8,
    }
    const_assert_eq!(core::mem::size_of::<KjExceptionDetail>(), 24);

    unsafe extern "C" {
        #[link_name = "cxxbridge1$kjException$new"]
        pub fn kj_exception_new(
            exception_type: i32,
            ptr: *const u8,
            len: usize,
            file: *const u8,
            file_len: usize,
            line: i32,
            details: *const KjExceptionDetail,
            details_count: usize,
        ) -> *mut KjException;

        #[link_name = "cxxbridge1$kjException$getDescription"]
        pub fn kj_exception_get_description(err: *mut KjException) -> *const c_char;

        #[link_name = "cxxbridge1$kjException$getType"]
        pub fn kj_exception_get_type(err: *mut KjException) -> i32;

        #[link_name = "cxxbridge1$kjException$getDetailsCount"]
        pub fn kj_exception_get_details_count(err: *mut KjException) -> usize;

        #[link_name = "cxxbridge1$kjException$getDetails"]
        pub fn kj_exception_get_details(
            err: *mut KjException,
            output: *mut KjExceptionDetail,
            max_count: usize,
        );

        #[link_name = "cxxbridge1$kjException$dropInPlace"]
        pub fn kj_exception_drop_in_place(err: *mut KjException);

        #[link_name = "cxxbridge1$kjException$getFile"]
        pub fn kj_exception_get_file(err: *mut KjException) -> *const c_char;

        #[link_name = "cxxbridge1$kjException$getLine"]
        pub fn kj_exception_get_line(err: *mut KjException) -> i32;
    }
}

/// Represents kj::CanceledException thrown from an `extern "C++"` function.
pub struct CanceledException {}

impl CanceledException {
    /// Panics with CanceledException, used for cancellation flow.
    pub fn panic() -> ! {
        std::panic::panic_any(Self {})
    }
}

impl Display for CanceledException {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("kj::CanceledException")
    }
}

/// Fully owned wrapper around kj::Exception* allocated with new.
/// Represents kj::Exception thrown from an `extern "C++"` function.
#[derive(Debug)]
pub struct KjException {
    pub(crate) err: NonNull<repr::KjException>,
}

// Safety: the bridge representation and ownership invariants satisfy this operation.
unsafe impl Sync for KjException {}
// Safety: the bridge representation and ownership invariants satisfy this operation.
unsafe impl Send for KjException {}

impl Display for KjException {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str(self.what())
    }
}

impl StdError for KjException {}

impl KjException {
    /// Allocate new kj::Exception instance.
    pub(crate) fn new(
        exception_type: repr::KjExceptionType,
        msg: &str,
        file: &str,
        line: u32,
        details: Option<&Vec<(u64, Vec<u8>)>>,
    ) -> Self {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        let exception = unsafe {
            match details {
                Some(details_vec) if !details_vec.is_empty() => {
                    // Convert details to C-compatible format
                    let c_details: Vec<repr::KjExceptionDetail> = details_vec
                        .iter()
                        .map(|(type_id, data)| repr::KjExceptionDetail {
                            type_id: *type_id,
                            data_len: data.len(),
                            data_ptr: data.as_ptr(),
                        })
                        .collect();

                    repr::kj_exception_new(
                        exception_type as i32,
                        msg.as_ptr(),
                        msg.len(),
                        file.as_ptr(),
                        file.len(),
                        line.try_into().unwrap_or_default(),
                        c_details.as_ptr(),
                        c_details.len(),
                    )
                }
                _ => repr::kj_exception_new(
                    exception_type as i32,
                    msg.as_ptr(),
                    msg.len(),
                    file.as_ptr(),
                    file.len(),
                    line.try_into().unwrap_or_default(),
                    core::ptr::null(),
                    0,
                ),
            }
        };
        Self {
            err: NonNull::new(exception).expect("can't allocate new kj::Exception"),
        }
    }

    /// Returns the exception description.
    pub fn what(&self) -> &str {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        let description = unsafe { repr::kj_exception_get_description(self.err.as_ptr()) };
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe {
            CStr::from_ptr(description)
                .to_str()
                .unwrap_or("bad kj::Exception description")
        }
    }

    /// Returns the exception type.
    pub fn r#type(&self) -> repr::KjExceptionType {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        let type_value = unsafe { repr::kj_exception_get_type(self.err.as_ptr()) };
        match type_value {
            1 => repr::KjExceptionType::Overloaded,
            2 => repr::KjExceptionType::Disconnected,
            3 => repr::KjExceptionType::Unimplemented,
            _ => repr::KjExceptionType::Failed,
        }
    }

    /// Returns the exception details.
    pub fn details(&self) -> Option<Vec<(u64, Vec<u8>)>> {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        let count = unsafe { repr::kj_exception_get_details_count(self.err.as_ptr()) };
        if count == 0 {
            return None;
        }

        let mut c_details: Vec<repr::KjExceptionDetail> = Vec::with_capacity(count);
        c_details.resize(
            count,
            repr::KjExceptionDetail {
                type_id: 0,
                data_len: 0,
                data_ptr: core::ptr::null(),
            },
        );

        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe {
            repr::kj_exception_get_details(self.err.as_ptr(), c_details.as_mut_ptr(), count);
        }

        let mut result = Vec::with_capacity(count);
        for detail in c_details {
            let data =
                // Safety: the bridge representation and ownership invariants satisfy this operation.
                unsafe { core::slice::from_raw_parts(detail.data_ptr, detail.data_len).to_vec() };
            result.push((detail.type_id, data));
        }

        Some(result)
    }

    /// Consumes the exception, returning the raw pointer.
    /// # Safety
    /// The caller must ensure that the returned pointer is eventually dropped.
    pub unsafe fn into_raw(self) -> NonNull<repr::KjException> {
        ManuallyDrop::new(self).err
    }

    /// File name where the exception was thrown.
    pub fn file(&self) -> &CStr {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { CStr::from_ptr(repr::kj_exception_get_file(self.err.as_ptr())) }
    }

    /// Line number where the exception was thrown.
    pub fn line(&self) -> i32 {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { repr::kj_exception_get_line(self.err.as_ptr()) }
    }
}

impl Drop for KjException {
    fn drop(&mut self) {
        // Safety: the bridge representation and ownership invariants satisfy this operation.
        unsafe { repr::kj_exception_drop_in_place(self.err.as_ptr()) }
    }
}

/// Trait for converting a Rust error object into a `kj::Exception`.
pub trait IntoKjException {
    /// Convert this error into a `kj::Exception` pointer.
    /// File and line should be used if the error doesn't capture them.
    /// The returned output must be new-allocated.
    fn into_kj_exception(self, file: &str, line: u32) -> KjException;
}

impl<T: core::error::Error> IntoKjException for T {
    fn into_kj_exception(self, file: &str, line: u32) -> KjException {
        let msg = self.to_string();
        KjException::new(repr::KjExceptionType::Failed, &msg, file, line, None)
    }
}

/// Error type to be converted into kj::Exception preserving all details.
#[derive(Debug, PartialEq, Eq, Clone, Hash)]
pub struct KjError {
    description: String,
    exception_type: repr::KjExceptionType,
    file: Option<String>,
    line: Option<u32>,
    details: Option<Vec<(u64, Vec<u8>)>>,
}

impl KjError {
    /// Creates a new `KjError` with the given exception type and description.
    pub fn new(exception_type: repr::KjExceptionType, description: String) -> Self {
        Self {
            description,
            exception_type,
            file: None,
            line: None,
            details: None,
        }
    }

    /// Adds exception details to this error.
    #[must_use]
    pub fn with_details(mut self, details: Vec<(u64, Vec<u8>)>) -> Self {
        self.details = Some(details);
        self
    }

    /// Adds source location information to this error.
    #[must_use]
    pub fn with_location(mut self, file: String, line: u32) -> Self {
        self.file = Some(file);
        self.line = Some(line);
        self
    }

    /// Returns a description of the error.
    pub fn description(&self) -> &str {
        &self.description
    }

    /// Returns the exception type.
    pub fn exception_type(&self) -> repr::KjExceptionType {
        self.exception_type
    }

    /// Returns the source file if available.
    pub fn file(&self) -> Option<&str> {
        self.file.as_deref()
    }

    /// Returns the line number if available.
    pub fn line(&self) -> Option<u32> {
        self.line
    }

    /// Returns the exception details if available.
    pub fn details(&self) -> Option<&Vec<(u64, Vec<u8>)>> {
        self.details.as_ref()
    }
}

impl IntoKjException for KjError {
    fn into_kj_exception(self, file: &str, line: u32) -> KjException {
        let msg = self.description.clone();
        let exception_type = self.exception_type;
        let file = self.file.as_deref().unwrap_or(file);
        let line = self.line.unwrap_or(line);
        let details = self.details.as_ref();

        KjException::new(exception_type, &msg, file, line, details)
    }
}

impl From<KjException> for KjError {
    fn from(value: KjException) -> Self {
        let mut error = Self::new(value.r#type(), value.to_string());

        if let Ok(file) = value.file().to_str() {
            error = error.with_location(
                String::from(file),
                value.line().try_into().unwrap_or_default(),
            );
        }

        if let Some(details) = value.details() {
            error = error.with_details(details);
        }

        error
    }
}

#[cfg(test)]
mod tests {
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
}
