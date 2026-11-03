// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The handles that each isolate keeps for converting `#[jsg_struct]` types.

#![allow(
    unsafe_code,
    reason = "opens the persistent name handles in the scope of the lock"
)]

use std::sync::OnceLock;
use std::sync::atomic::AtomicUsize;
use std::sync::atomic::Ordering;

use crate::Error;
use crate::Lock;
use crate::v8;

/// The JavaScript names of a `#[jsg_struct]`'s public fields, in declaration order. Like C++
/// JSG's `StructWrapper`, each isolate interns them when it first converts the struct and keeps
/// the handles, so that a conversion creates no strings.
#[doc(hidden)]
pub struct StructFields<const N: usize> {
    names: [&'static str; N],
    /// Where every isolate keeps this struct's handles in its [`StructCache`], assigned on first
    /// use.
    index: OnceLock<usize>,
}

impl<const N: usize> StructFields<N> {
    #[must_use]
    pub const fn new(names: [&'static str; N]) -> Self {
        Self {
            names,
            index: OnceLock::new(),
        }
    }

    fn index(&self) -> usize {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        *self
            .index
            .get_or_init(|| NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// An isolate's handles for the structs it has converted, by [`StructFields`] index.
#[derive(Default)]
pub struct StructCache(Vec<StructHandles>);

#[derive(Default)]
struct StructHandles {
    names: Option<Box<[v8::Global<v8::String>]>>,
    template: Option<v8::Global<v8::DictionaryTemplate>>,
}

impl StructCache {
    fn get(&self, index: usize) -> Option<&StructHandles> {
        self.0.get(index)
    }

    fn entry(&mut self, index: usize) -> &mut StructHandles {
        if self.0.len() <= index {
            self.0.resize_with(index + 1, StructHandles::default);
        }
        &mut self.0[index]
    }
}

impl<'s> Lock<'s> {
    /// Returns handles of the names of `fields`, which this isolate interns the first time.
    ///
    /// # Errors
    ///
    /// Fails if V8 can't create a name's string.
    #[doc(hidden)]
    pub fn struct_field_names<const N: usize>(
        &mut self,
        fields: &'static StructFields<N>,
    ) -> Result<[v8::Local<'s, v8::String>; N], Error> {
        let index = fields.index();
        let isolate = self.isolate().as_ffi();
        let open = |names: &[v8::Global<v8::String>]| {
            std::array::from_fn(|i| {
                // SAFETY: the handles belong to this lock's isolate, which is locked, and are
                // opened in its current scope.
                unsafe {
                    v8::Local::from_ffi(v8::ffi::global_to_local(isolate, &names[i].as_ffi()))
                }
            })
        };
        if let Some(names) = self.structs().get(index).and_then(|h| h.names.as_deref()) {
            return Ok(open(names));
        }
        Ok(open(self.intern_struct_field_names(index, &fields.names)?))
    }

    fn structs(&mut self) -> &mut StructCache {
        &mut self.realm().structs
    }

    #[cold]
    fn intern_struct_field_names(
        &mut self,
        index: usize,
        names: &[&'static str],
    ) -> Result<&[v8::Global<v8::String>], Error> {
        let names = names
            .iter()
            .map(|name| {
                let name = v8::String::new_internalized_from_str(self, name)
                    .ok_or_else(Error::invalid_string_length)?;
                Ok(v8::Global::new(self, name))
            })
            .collect::<Result<Box<[_]>, Error>>()?;
        Ok(self.structs().entry(index).names.insert(names))
    }

    /// Creates an object with the names of `fields` as data properties whose values are `values`,
    /// from a template that this isolate creates the first time, as C++ JSG's `StructWrapper`
    /// does with `fast_jsg_struct`. Runs no JavaScript.
    ///
    /// # Errors
    ///
    /// While an exception or a termination is pending, it creates nothing, and the error is one
    /// for which [`Error::is_pending`] is true.
    #[doc(hidden)]
    pub fn new_struct_object<const N: usize>(
        &mut self,
        fields: &'static StructFields<N>,
        values: &[v8::Local<'s, v8::Value>; N],
    ) -> Result<v8::Local<'s, v8::Object>, Error> {
        let index = fields.index();
        let template = match self.structs().get(index).and_then(|h| h.template.as_ref()) {
            Some(template) => template.as_ffi(),
            None => self.create_struct_template(index, &fields.names)?,
        };
        // SAFETY: the template belongs to this lock's isolate, whose realm keeps it for longer
        // than this call, and has the N names of `fields`, one for each value.
        unsafe { v8::DictionaryTemplate::new_instance(self, &template, values) }
    }

    #[cold]
    fn create_struct_template(
        &mut self,
        index: usize,
        names: &[&'static str],
    ) -> Result<v8::ffi::Global, Error> {
        let template = v8::DictionaryTemplate::new(self, names)?;
        let template = self.structs().entry(index).template.insert(template);
        Ok(template.as_ffi())
    }
}
