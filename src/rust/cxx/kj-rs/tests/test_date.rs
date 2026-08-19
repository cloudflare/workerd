use kj_rs::KjDate;
#[cxx::bridge(namespace = "kj_rs_demo")]
pub mod ffi {

    unsafe extern "C++" {
        include!("kj-rs-demo/test-date.h");

        pub fn c_create_date_epoch() -> KjDate;
        pub fn c_create_date_from_nanos(nanoseconds: i64) -> KjDate;
        pub fn c_return_date_epoch() -> KjDate;
        pub fn c_return_date_from_nanos(nanoseconds: i64) -> KjDate;
        pub fn c_return_5_sec_after_epoch() -> KjDate;
        pub fn c_take_date_epoch(date: KjDate);
        pub fn c_take_date_7777777777(date: KjDate);
        pub fn c_take_date_and_verify_nanos(date: KjDate, expected_nanos: i64);
        pub fn c_roundtrip_date(date: KjDate) -> KjDate;
        pub fn c_verify_date_equality(date1: KjDate, date2: KjDate) -> bool;
        pub fn c_verify_date_ordering(earlier: KjDate, later: KjDate) -> bool;
        pub fn c_extract_nanoseconds_from_date(date: KjDate) -> i64;
    }

    extern "Rust" {
        // Rust functions that C++ can call for KjDate testing
        pub fn r_return_date_epoch() -> KjDate;
        pub fn r_return_date_from_nanos(nanoseconds: i64) -> KjDate;
        pub fn r_return_date_specific() -> KjDate;
        pub fn r_take_date_epoch(date: KjDate);
        pub fn r_take_date_and_verify_nanos(date: KjDate, expected_nanos: i64);
        pub fn r_roundtrip_date(date: KjDate) -> KjDate;
        pub fn r_verify_date_equality(date1: KjDate, date2: KjDate) -> bool;
        pub fn r_verify_date_ordering(earlier: KjDate, later: KjDate) -> bool;
        pub fn r_extract_nanoseconds(date: KjDate) -> i64;
        pub fn r_create_date_from_seconds(seconds: i64) -> KjDate;
        pub fn r_verify_date_sequence(dates: &[KjDate]) -> bool;
    }
}

pub fn r_take_date_epoch(date: KjDate) {
    assert_eq!(date.nanoseconds(), 0, "Expected Unix epoch date");
}

pub fn r_take_date_and_verify_nanos(date: KjDate, expected_nanos: i64) {
    assert_eq!(
        date.nanoseconds(),
        expected_nanos,
        "Date nanoseconds don't match expected value: expected {}, got {}",
        expected_nanos,
        date.nanoseconds()
    );
}

pub fn r_return_date_epoch() -> KjDate {
    KjDate::unix_epoch()
}

pub fn r_return_date_from_nanos(nanoseconds: i64) -> KjDate {
    KjDate::from(nanoseconds)
}

pub fn r_return_date_specific() -> KjDate {
    KjDate::from(5_000_000_000i64)
}

pub fn r_roundtrip_date(date: KjDate) -> KjDate {
    date
}

pub fn r_verify_date_equality(date1: KjDate, date2: KjDate) -> bool {
    date1 == date2
}

pub fn r_verify_date_ordering(earlier: KjDate, later: KjDate) -> bool {
    earlier < later
}

pub fn r_extract_nanoseconds(date: KjDate) -> i64 {
    i64::from(date)
}

pub fn r_verify_date_sequence(dates: &[KjDate]) -> bool {
    if dates.len() < 2 {
        return true;
    }

    for i in 0..dates.len() - 1 {
        if dates[i] >= dates[i + 1] {
            return false;
        }
    }
    true
}

pub fn r_create_date_from_seconds(seconds: i64) -> KjDate {
    KjDate::from(seconds * 1_000_000_000)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    pub fn test_kjdate_creation() {
        let epoch = KjDate::unix_epoch();
        assert_eq!(epoch.nanoseconds(), 0);

        let date = KjDate::from(1_000_000_000i64);
        assert_eq!(date.nanoseconds(), 1_000_000_000);
    }

    #[test]
    pub fn test_kjdate_ordering() {
        let earlier = KjDate::from(1000i64);
        let later = KjDate::from(2000i64);

        assert!(earlier < later);
        assert!(later > earlier);
        assert_ne!(earlier, later);
        assert_eq!(earlier, KjDate::from(1000i64));
    }

    #[test]
    pub fn test_kjdate_default() {
        let default_date = KjDate::default();
        assert_eq!(default_date, KjDate::unix_epoch());
    }

    #[test]
    pub fn test_rust_calls_cpp_ffi_functions() {
        // Test Rust calling C++ functions directly via FFI

        // Test creation functions - Rust calls C++, C++ returns KjDate
        let cpp_epoch = ffi::c_create_date_epoch();
        assert_eq!(cpp_epoch.nanoseconds(), 0);

        let cpp_from_nanos = ffi::c_create_date_from_nanos(1_000_000_000);
        assert_eq!(cpp_from_nanos.nanoseconds(), 1_000_000_000);

        // Test C++ -> Rust passing
        let cpp_returned_epoch = ffi::c_return_date_epoch();
        assert_eq!(cpp_returned_epoch.nanoseconds(), 0);

        let cpp_returned_from_nanos = ffi::c_return_date_from_nanos(2_500_000_000);
        assert_eq!(cpp_returned_from_nanos.nanoseconds(), 2_500_000_000);

        let cpp_returned_specific = ffi::c_return_5_sec_after_epoch();
        assert_eq!(cpp_returned_specific.nanoseconds(), 5_000_000_000); // 5 seconds

        let test_nanos2 = 5_000_000_000i64;
        let cpp_return_from_nanos = ffi::c_return_date_from_nanos(test_nanos2);
        assert_eq!(cpp_return_from_nanos.nanoseconds(), test_nanos2);
    }

    #[test]
    pub fn test_rust_sends_dates_to_cpp_ffi() {
        // Test take functions - Rust passes KjDate to C++ for verification
        let test_epoch = KjDate::unix_epoch();
        ffi::c_take_date_epoch(test_epoch); // Should not panic if correct

        let test_specific = KjDate::from(7_777_777_777i64);
        ffi::c_take_date_7777777777(test_specific); // Should not panic

        let test_verify = KjDate::from(9_999_999_999i64);
        ffi::c_take_date_and_verify_nanos(test_verify, 9_999_999_999); // Should not panic

        let test_nanos = 7_500_000_000i64;
        let test_date = KjDate::from(test_nanos);
        ffi::c_take_date_and_verify_nanos(test_date, test_nanos); // Should not panic
    }

    #[test]
    pub fn test_rust_cpp_ffi_round_trip_and_verification() {
        // Test complex interactions between Rust and C++ via FFI

        // Round-trip test: Rust -> C++ -> Rust
        let original = KjDate::from(777_888_999i64);
        let cpp_round_tripped = ffi::c_roundtrip_date(original);
        assert!(ffi::c_verify_date_equality(original, cpp_round_tripped));

        // Verification functions: Rust asks C++ to verify dates
        let date1 = KjDate::from(1_234_567_890i64);
        let date2 = KjDate::from(1_234_567_890i64);
        let date3 = KjDate::from(9_876_543_210i64);

        assert!(ffi::c_verify_date_equality(date1, date2));
        assert!(!ffi::c_verify_date_equality(date1, date3));

        // Ordering verification: Rust asks C++ to verify ordering
        let earlier = KjDate::from(1_000_000_000i64);
        let later = KjDate::from(2_000_000_000i64);
        assert!(ffi::c_verify_date_ordering(earlier, later));
        assert!(!ffi::c_verify_date_ordering(later, earlier));

        // Extraction test: Rust asks C++ to extract nanoseconds
        let expected_nanos = 3_333_333_333i64;
        let test_date = KjDate::from(expected_nanos);
        let cpp_extracted = ffi::c_extract_nanoseconds_from_date(test_date);
        assert_eq!(cpp_extracted, expected_nanos);
    }

    #[test]
    pub fn test_kjdate_system_time_conversion() {
        use std::time::SystemTime;
        use std::time::UNIX_EPOCH;

        // Test conversion from SystemTime
        let sys_time = UNIX_EPOCH + std::time::Duration::from_millis(1500);
        let date = KjDate::from(sys_time);
        assert_eq!(date.nanoseconds(), 1_500_000_000);

        // Test conversion to SystemTime
        let date = KjDate::from(2_000_000_000i64);
        let converted_sys_time = SystemTime::from(date);
        let expected_sys_time = UNIX_EPOCH + std::time::Duration::from_secs(2);
        assert_eq!(converted_sys_time, expected_sys_time);

        // Test round trip conversion
        let original_time = UNIX_EPOCH + std::time::Duration::from_secs(3);
        let date = KjDate::from(original_time);
        let round_trip_time = SystemTime::from(date);
        assert_eq!(original_time, round_trip_time);
    }

    #[test]
    pub fn test_kjdate_debug() {
        let date = KjDate::from(123_456_789i64);
        assert!(format!("{date:?}").starts_with("KjDate(SystemTime"),);
    }
}
