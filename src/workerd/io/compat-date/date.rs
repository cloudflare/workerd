//! Compatibility dates: `YYYY-MM-DD` strings in the years 2000 through 2999.

use std::fmt;
use std::time::SystemTime;
use std::time::UNIX_EPOCH;

/// A calendar date as a Worker's compatibility date names it.
///
/// Parsing checks only the ranges of the fields, so it accepts dates that do not exist, such as
/// `2022-02-30`. Dates order by year, then month, then day.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CompatDate {
    year: u32,
    month: u32,
    day: u32,
}

impl CompatDate {
    /// Returns the date for the given fields, which `parse` would also accept.
    #[must_use]
    pub const fn new(year: u32, month: u32, day: u32) -> Option<Self> {
        if year < 2000 || year >= 3000 || month < 1 || month > 12 || day < 1 || day > 31 {
            return None;
        }
        Some(Self { year, month, day })
    }

    /// Parses a compatibility date.
    ///
    /// The text is ten characters of digits and dashes, starting with `2`, with dashes at
    /// offsets 4 and 7. The three numbers are then read as C's `sscanf("%d-%d-%d")` reads them,
    /// since the C++ runtime has always parsed them that way: a dash directly after the day's
    /// first digit ends the text, so `2021-05-1-` is May 1st, 2021.
    #[must_use]
    pub fn parse(text: &str) -> Option<Self> {
        Self::parse_bytes(text.as_bytes())
    }

    /// Like `parse`, for text that need not be UTF-8 (and so is not a date).
    #[must_use]
    pub fn parse_bytes(bytes: &[u8]) -> Option<Self> {
        if bytes.first() != Some(&b'2') {
            return None;
        }
        if bytes.len() != 10 || bytes[4] != b'-' || bytes[7] != b'-' {
            return None;
        }
        if !bytes.iter().all(|&c| c.is_ascii_digit() || c == b'-') {
            return None;
        }

        let mut scanner = Scanner { bytes, pos: 0 };
        let year = scanner.int()?;
        scanner.literal(b'-')?;
        let month = scanner.int()?;
        scanner.literal(b'-')?;
        let day = scanner.int()?;
        // `sscanf` stores a negative number in an unsigned field as a huge one, which the range
        // checks below reject; `try_from` rejects it the same way.
        Self::new(
            u32::try_from(year).ok()?,
            u32::try_from(month).ok()?,
            u32::try_from(day).ok()?,
        )
    }

    /// Returns today's date in UTC.
    #[must_use]
    pub fn today() -> Self {
        let seconds = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |d| d.as_secs());
        let (year, month, day) = civil_from_days(seconds / 86_400);
        // Clamp into the supported range, which the clock leaves only if it is badly wrong.
        Self::new(year, month, day).unwrap_or(if year < 2000 {
            Self {
                year: 2000,
                month: 1,
                day: 1,
            }
        } else {
            Self {
                year: 2999,
                month: 12,
                day: 31,
            }
        })
    }
}

impl fmt::Display for CompatDate {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}-{:02}-{:02}", self.year, self.month, self.day)
    }
}

/// Reads the text as `sscanf` reads `%d` and literal characters.
struct Scanner<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl Scanner<'_> {
    /// Reads an optionally signed decimal number of at least one digit.
    fn int(&mut self) -> Option<i64> {
        let mut negative = false;
        if let Some(&sign @ (b'+' | b'-')) = self.bytes.get(self.pos) {
            negative = sign == b'-';
            self.pos += 1;
        }
        let start = self.pos;
        let mut value: i64 = 0;
        while let Some(&c) = self.bytes.get(self.pos) {
            if !c.is_ascii_digit() {
                break;
            }
            // At most ten digits, so this cannot overflow.
            value = value * 10 + i64::from(c - b'0');
            self.pos += 1;
        }
        if self.pos == start {
            return None;
        }
        Some(if negative { -value } else { value })
    }

    fn literal(&mut self, expected: u8) -> Option<()> {
        if self.bytes.get(self.pos) == Some(&expected) {
            self.pos += 1;
            Some(())
        } else {
            None
        }
    }
}

/// Converts days since 1970-01-01 to a proleptic Gregorian (year, month, day).
///
/// Howard Hinnant's `civil_from_days`, for non-negative day counts.
#[expect(
    clippy::cast_possible_truncation,
    reason = "the year of any `u64` day count a clock can report fits a `u32`, and month and day are at most 31"
)]
const fn civil_from_days(days: u64) -> (u32, u32, u32) {
    let z = days + 719_468;
    let era = z / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + if month <= 2 { 1 } else { 0 };
    (year as u32, month as u32, day as u32)
}

#[cfg(test)]
#[path = "date-test.rs"]
mod tests;
