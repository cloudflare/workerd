// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use crate::proto;
use crate::proto::any_value::Value;

/// Span or resource attributes, in the order they were added.
///
/// Lookups are linear: a span carries a few dozen attributes at most, and the order is what a
/// collector shows.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Attributes(Vec<(String, Value)>);

impl Attributes {
    /// Appends without looking for an existing entry, for callers whose keys are already distinct.
    pub fn push(&mut self, key: impl Into<String>, value: impl IntoValue) {
        self.0.push((key.into(), value.into_value()));
    }

    /// Replaces the value in place when the key is present, and appends otherwise.
    pub fn set(&mut self, key: &str, value: impl IntoValue) {
        match self.0.iter_mut().find(|(k, _)| k == key) {
            Some((_, existing)) => *existing = value.into_value(),
            None => self.push(key, value),
        }
    }

    /// Appends unless the key is present.
    pub fn set_default(&mut self, key: &str, value: impl IntoValue) {
        if self.get(key).is_none() {
            self.push(key, value);
        }
    }

    /// Applies `set_default` for every entry of `defaults`.
    pub fn set_defaults(&mut self, defaults: &Self) {
        for (key, value) in &defaults.0 {
            self.set_default(key, value.clone());
        }
    }

    pub fn get(&self, key: &str) -> Option<&Value> {
        self.0.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    /// The value when it is a string.
    pub fn string(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Value::String(s)) => Some(s),
            _ => None,
        }
    }

    /// The value when it is an integer.
    pub fn int(&self, key: &str) -> Option<i64> {
        match self.get(key) {
            Some(Value::Int(i)) => Some(*i),
            _ => None,
        }
    }

    pub fn into_proto(self) -> Vec<proto::KeyValue> {
        self.0
            .into_iter()
            .map(|(key, value)| key_value(key, value))
            .collect()
    }
}

pub fn key_value(key: impl Into<String>, value: impl IntoValue) -> proto::KeyValue {
    proto::KeyValue {
        key: key.into(),
        value: Some(proto::AnyValue {
            value: Some(value.into_value()),
        }),
    }
}

/// The Rust types an attribute value can be built from.
pub trait IntoValue {
    fn into_value(self) -> Value;
}

impl IntoValue for Value {
    fn into_value(self) -> Value {
        self
    }
}

impl IntoValue for String {
    fn into_value(self) -> Value {
        Value::String(self)
    }
}

impl IntoValue for &str {
    fn into_value(self) -> Value {
        Value::String(self.to_owned())
    }
}

impl IntoValue for bool {
    fn into_value(self) -> Value {
        Value::Bool(self)
    }
}

impl IntoValue for i64 {
    fn into_value(self) -> Value {
        Value::Int(self)
    }
}

impl IntoValue for f64 {
    fn into_value(self) -> Value {
        Value::Double(self)
    }
}
