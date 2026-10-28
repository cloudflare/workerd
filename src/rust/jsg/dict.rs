// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

/// A JavaScript object used as a map from strings to values of type `V`: Web IDL's
/// `record<DOMString, V>`, and C++ JSG's `jsg::Dict<V>`.
///
/// Like C++ JSG's, it keeps the entries in a `Vec`, in order.
///
/// - From JavaScript, it takes any object other than an array, and reads the object's own
///   enumerable properties that are not symbols, in property order, as `Object.keys()` lists
///   them. Each value is read and converted before the next one is read. A value of the wrong
///   type is a `TypeError` that names its key.
/// - To JavaScript, it is a new plain object with an own data property for each entry, defined
///   in order, so that a later entry with the same key overwrites an earlier one. As in Web IDL,
///   and unlike C++ JSG, which sets the properties, no setter on `Object.prototype` runs and a
///   `__proto__` entry is an own property.
///
/// # Examples
///
/// ```ignore
/// use jsg::Dict;
///
/// #[jsg_method]
/// pub fn set_headers(&self, headers: Dict<String>) {
///     for (name, value) in headers {
///         // ...
///     }
/// }
///
/// #[jsg_method]
/// pub fn get_counts(&self) -> Dict<jsg::Number> {
///     Dict::from(vec![("hits".to_owned(), jsg::Number::new(3.0))])
/// }
/// ```
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct Dict<V> {
    /// The entries, in order.
    entries: Vec<(String, V)>,
}

impl<V> Dict<V> {
    /// Returns the value of the first entry with `key`, searching the entries in order.
    pub fn find(&self, key: &str) -> Option<&V> {
        self.entries.iter().find(|(k, _)| k == key).map(|(_, v)| v)
    }

    /// Returns an iterator over the entries' keys and values, in order.
    pub fn iter(&self) -> impl Iterator<Item = (&str, &V)> {
        self.entries.iter().map(|(k, v)| (k.as_str(), v))
    }

    /// Returns the number of entries, counting each entry of a duplicate key.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Returns whether there are no entries.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

impl<V> Default for Dict<V> {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
        }
    }
}

impl<V> From<Vec<(String, V)>> for Dict<V> {
    fn from(entries: Vec<(String, V)>) -> Self {
        Self { entries }
    }
}

impl<V> FromIterator<(String, V)> for Dict<V> {
    fn from_iter<I: IntoIterator<Item = (String, V)>>(iter: I) -> Self {
        Self {
            entries: iter.into_iter().collect(),
        }
    }
}

impl<V> IntoIterator for Dict<V> {
    type Item = (String, V);
    type IntoIter = std::vec::IntoIter<(String, V)>;

    fn into_iter(self) -> Self::IntoIter {
        self.entries.into_iter()
    }
}

#[cfg(test)]
#[path = "dict-test.rs"]
mod tests;
