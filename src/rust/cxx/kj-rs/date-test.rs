use super::*;

#[test]
fn test_unix_epoch() {
    let epoch = KjDate::unix_epoch();
    assert_eq!(epoch.nanoseconds(), 0);
}

#[test]
fn test_from() {
    let date = KjDate::from(1_000_000_000);
    assert_eq!(date.nanoseconds(), 1_000_000_000);
}

#[test]
fn test_ordering() {
    let earlier = KjDate::from(1000);
    let later = KjDate::from(2000);
    assert!(earlier < later);
}

#[test]
fn test_default() {
    let default_date = KjDate::default();
    assert_eq!(default_date, KjDate::unix_epoch());
}

#[test]
fn test_system_time_conversion() {
    let date = KjDate::from(1_000_000_000);
    let system_time: SystemTime = date.into();
    let converted_back = KjDate::from(system_time);
    assert_eq!(date, converted_back);
}
