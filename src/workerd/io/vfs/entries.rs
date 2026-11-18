// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::collections::HashMap;

/// A map from names to values that iterates in the order of `kj::HashMap`: entries are kept in
/// insertion order, and removing an entry moves the last entry into its place.
///
/// `readdir()` reports a directory's entries in this order, so Workers can observe it.
pub struct EntryMap<V> {
    entries: Vec<(Box<[u8]>, V)>,
    index: HashMap<Box<[u8]>, usize>,
}

impl<V> Default for EntryMap<V> {
    fn default() -> Self {
        Self {
            entries: Vec::new(),
            index: HashMap::new(),
        }
    }
}

impl<V> EntryMap<V> {
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn get(&self, name: &[u8]) -> Option<&V> {
        self.index.get(name).map(|&i| &self.entries[i].1)
    }

    pub fn get_mut(&mut self, name: &[u8]) -> Option<&mut V> {
        self.index.get(name).map(|&i| &mut self.entries[i].1)
    }

    pub fn contains(&self, name: &[u8]) -> bool {
        self.index.contains_key(name)
    }

    /// Adds an entry at the end. The caller has checked that `name` is absent.
    pub fn push(&mut self, name: &[u8], value: V) {
        debug_assert!(!self.contains(name));
        let name: Box<[u8]> = name.into();
        self.index.insert(name.clone(), self.entries.len());
        self.entries.push((name, value));
    }

    /// Removes the entry named `name`, moving the last entry into its place, and returns whether
    /// there was one.
    pub fn remove(&mut self, name: &[u8]) -> bool {
        let Some(pos) = self.index.remove(name) else {
            return false;
        };
        self.entries.swap_remove(pos);
        if let Some((moved, _)) = self.entries.get(pos) {
            self.index.insert(moved.clone(), pos);
        }
        true
    }

    pub fn iter(&self) -> impl Iterator<Item = (&[u8], &V)> {
        self.entries.iter().map(|(name, value)| (&**name, value))
    }

    pub fn into_iter(self) -> impl Iterator<Item = (Box<[u8]>, V)> {
        self.entries.into_iter()
    }
}

#[cfg(test)]
#[path = "entries-test.rs"]
mod tests;
