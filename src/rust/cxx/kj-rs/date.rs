use core::fmt;
use std::time::Duration;
use std::time::SystemTime;

/// Represents a `kj::Date` from the KJ library.
///
/// Like C++ represents a point in time as nanoseconds since the Unix epoch (January 1, 1970 UTC).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct KjDate {
    /// Nanoseconds since Unix epoch (January 1, 1970 UTC)
    nanoseconds: i64,
}

impl KjDate {
    /// Creates a new `KjDate` representing the Unix epoch (January 1, 1970 UTC).
    #[inline]
    #[must_use]
    pub const fn unix_epoch() -> Self {
        Self { nanoseconds: 0 }
    }

    /// Returns the nanoseconds since Unix epoch.
    /// This is the only public accessor method needed.
    #[inline]
    #[must_use]
    pub const fn nanoseconds(&self) -> i64 {
        self.nanoseconds
    }
}

impl Default for KjDate {
    /// Returns the Unix epoch as the default date.
    fn default() -> Self {
        Self::unix_epoch()
    }
}

impl fmt::Debug for KjDate {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "KjDate({:?})",
            std::convert::Into::<SystemTime>::into(*self)
        )
    }
}

impl From<i64> for KjDate {
    #[inline]
    fn from(nanoseconds: i64) -> Self {
        Self { nanoseconds }
    }
}

impl From<KjDate> for i64 {
    #[inline]
    fn from(date: KjDate) -> Self {
        date.nanoseconds
    }
}

impl From<SystemTime> for KjDate {
    fn from(time: SystemTime) -> Self {
        match time.duration_since(std::time::UNIX_EPOCH) {
            Ok(duration) => {
                let nanoseconds = i64::try_from(duration.as_nanos()).unwrap_or(i64::MAX);
                Self { nanoseconds }
            }
            Err(err) => {
                let duration_before = err.duration();
                let nanoseconds = -(i64::try_from(duration_before.as_nanos()).unwrap_or(i64::MAX));
                Self { nanoseconds }
            }
        }
    }
}

impl From<KjDate> for SystemTime {
    fn from(date: KjDate) -> Self {
        let duration =
            Duration::from_nanos(u64::try_from(date.nanoseconds.abs()).unwrap_or(u64::MAX));

        if date.nanoseconds >= 0 {
            std::time::UNIX_EPOCH + duration
        } else {
            std::time::UNIX_EPOCH - duration
        }
    }
}

#[cfg(test)]
mod tests {
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
}
