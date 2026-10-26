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
/// - To JavaScript, it is a new plain object with a property for each entry, set in order, so
///   that a later entry with the same key overwrites an earlier one.
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
    pub entries: Vec<(String, V)>,
}

impl<V> Dict<V> {
    /// Returns the value of the first entry with `key`.
    pub fn get(&self, key: &str) -> Option<&V> {
        self.entries.iter().find(|(k, _)| k == key).map(|(_, v)| v)
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
