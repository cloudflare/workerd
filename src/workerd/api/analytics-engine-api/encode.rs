// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The limits of a data point, and its `AnalyticsEngineEvent` message
//! (`src/workerd/api/analytics-engine.capnp`), as `analytics-engine-impl.h` checks and builds
//! them.

use analytics_engine_capnp::analytics_engine_event;
use jsg::Nullable;

use crate::data_point::Bytes;
use crate::data_point::DataPoint;

pub const MAX_INDEXES_LENGTH: usize = 1;
pub const MAX_INDEX_SIZE_IN_BYTES: usize = 96;
pub const MAX_ARRAY_MEMBERS: usize = 20;
pub const MAX_CUMULATIVE_BYTES_IN_BLOBS: usize = 800 * MAX_ARRAY_MEMBERS;

/// Fails as C++ `validateDoubles()` does. Each error message starts with `prefix`.
pub fn validate_doubles(doubles: &[f64], prefix: &str) -> jsg::Result<()> {
    if doubles.len() > MAX_ARRAY_MEMBERS {
        return Err(limit_error(
            prefix,
            format_args!("Maximum of {MAX_ARRAY_MEMBERS} doubles supported."),
        ));
    }
    Ok(())
}

/// Fails as C++ `validateBlobs()` does: a null blob counts towards the number of blobs, but not
/// their size.
pub fn validate_blobs(blobs: &[Nullable<Bytes>], prefix: &str) -> jsg::Result<()> {
    if blobs.len() > MAX_ARRAY_MEMBERS {
        return Err(limit_error(
            prefix,
            format_args!("Maximum of {MAX_ARRAY_MEMBERS} blobs supported."),
        ));
    }
    let size: usize = blobs.iter().filter_map(bytes).map(<[u8]>::len).sum();
    if size > MAX_CUMULATIVE_BYTES_IN_BLOBS {
        return Err(limit_error(
            prefix,
            format_args!(
                "Cumulative size of blobs exceeds {MAX_CUMULATIVE_BYTES_IN_BLOBS} bytes)."
            ),
        ));
    }
    Ok(())
}

/// Fails as C++ `validateIndexes()` does.
pub fn validate_indexes(indexes: &[Nullable<Bytes>], prefix: &str) -> jsg::Result<()> {
    if indexes.len() > MAX_INDEXES_LENGTH {
        return Err(limit_error(
            prefix,
            format_args!("Maximum of {MAX_INDEXES_LENGTH} indexes supported."),
        ));
    }
    if indexes
        .first()
        .and_then(bytes)
        .is_some_and(|index| index.len() > MAX_INDEX_SIZE_IN_BYTES)
    {
        return Err(limit_error(
            prefix,
            format_args!("Size of indexes[0] exceeds {MAX_INDEX_SIZE_IN_BYTES} bytes)."),
        ));
    }
    Ok(())
}

/// Fails as `AnalyticsEngine::writeDataPoint()` does for a data point over a limit: for the
/// indexes, then the blobs, then the doubles.
pub fn validate(point: &DataPoint, prefix: &str) -> jsg::Result<()> {
    if let Some(indexes) = &point.indexes {
        validate_indexes(indexes, prefix)?;
    }
    if let Some(blobs) = &point.blobs {
        validate_blobs(blobs, prefix)?;
    }
    if let Some(doubles) = &point.doubles {
        validate_doubles(doubles, prefix)?;
    }
    Ok(())
}

fn limit_error(prefix: &str, message: std::fmt::Arguments<'_>) -> jsg::Error {
    jsg::Error::new_type_error(format!("{prefix}{message}"))
}

fn bytes(value: &Nullable<Bytes>) -> Option<&[u8]> {
    match value {
        Nullable::Some(Bytes(bytes)) => Some(bytes),
        Nullable::Null => None,
    }
}

/// What a binding puts in each of its messages.
#[jsg_macros::jsg_traced]
#[derive(Debug, Clone)]
pub struct Header {
    pub account_id: i64,
    pub dataset: String,
    pub schema_version: i64,
}

/// The serialized `AnalyticsEngineEvent` for `point`, which [`validate`] accepts, written at
/// `timestamp`, in nanoseconds since the Unix epoch.
#[must_use]
pub fn encode(header: &Header, point: &DataPoint, timestamp: i64) -> Vec<u8> {
    let mut message = capnp::message::Builder::new_default();
    let mut event = message.init_root::<analytics_engine_event::Builder<'_>>();
    event.set_account_id(header.account_id);
    event.set_timestamp(timestamp);
    event.set_dataset(header.dataset.as_bytes());
    event.set_schema_version(header.schema_version);
    // index1 is empty, rather than null, unless the data point sets it.
    event.set_index1(&[]);
    if let Some(index) = point
        .indexes
        .as_deref()
        .and_then(<[_]>::first)
        .and_then(bytes)
    {
        event.set_index1(index);
    }
    for (i, blob) in point.blobs.iter().flatten().enumerate() {
        if let Some(blob) = bytes(blob) {
            set_blob(&mut event, i + 1, blob);
        }
    }
    for (i, &double) in point.doubles.iter().flatten().enumerate() {
        set_double(&mut event, i + 1, double);
    }
    capnp::serialize::write_message_to_words(&message)
}

/// Defines `$name(event, n, value)`, which calls the setter of field `n` of the numbered fields
/// that `$setter` lists.
macro_rules! numbered_setter {
    ($name:ident($value:ty) { $($n:literal => $setter:ident),+ $(,)? }) => {
        fn $name(event: &mut analytics_engine_event::Builder<'_>, n: usize, value: $value) {
            match n {
                $($n => event.$setter(value),)+
                _ => unreachable!("validate() allows at most {MAX_ARRAY_MEMBERS}"),
            }
        }
    };
}

numbered_setter!(set_double(f64) {
    1 => set_double1, 2 => set_double2, 3 => set_double3, 4 => set_double4,
    5 => set_double5, 6 => set_double6, 7 => set_double7, 8 => set_double8,
    9 => set_double9, 10 => set_double10, 11 => set_double11, 12 => set_double12,
    13 => set_double13, 14 => set_double14, 15 => set_double15, 16 => set_double16,
    17 => set_double17, 18 => set_double18, 19 => set_double19, 20 => set_double20,
});

numbered_setter!(set_blob(&[u8]) {
    1 => set_blob1, 2 => set_blob2, 3 => set_blob3, 4 => set_blob4,
    5 => set_blob5, 6 => set_blob6, 7 => set_blob7, 8 => set_blob8,
    9 => set_blob9, 10 => set_blob10, 11 => set_blob11, 12 => set_blob12,
    13 => set_blob13, 14 => set_blob14, 15 => set_blob15, 16 => set_blob16,
    17 => set_blob17, 18 => set_blob18, 19 => set_blob19, 20 => set_blob20,
});

#[cfg(test)]
#[path = "encode-test.rs"]
mod tests;
