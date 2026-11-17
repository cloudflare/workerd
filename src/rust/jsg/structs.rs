// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! The V8 handles that `#[jsg_struct]` conversions reuse, kept per isolate in the [`Realm`], as
//! C++ JSG's `StructWrapper` keeps them: an internalized name per field, and, with the
//! `fast_jsg_struct` flag, a `v8::DictionaryTemplate` declaring the fields, from which each
//! conversion to JavaScript creates its object in one step.

#![allow(
    unsafe_code,
    reason = "calls the shims that create and use the templates and names, and points into the realm's shapes"
)]

use std::any::TypeId;
use std::collections::HashMap;
use std::hash::BuildHasherDefault;
use std::marker::PhantomData;
use std::ptr::NonNull;

use crate::Lock;
use crate::resource::TypeIdHasher;
use crate::v8;
use crate::v8::call_into_js;
use crate::v8::ffi;

/// The handles of one struct type.
struct Shape {
    /// The internalized name of each field, in the order the struct declares its fields.
    names: Box<[v8::Global<v8::Name>]>,
    /// Declares the fields in the same order, if the realm has `fast_jsg_struct`.
    template: Option<v8::Global<v8::DictionaryTemplate>>,
}

/// The [`Shape`] of each struct type converted in a realm.
///
/// A shape is boxed and never moves or changes until it is dropped, which happens only when the
/// realm drops it or the startup snapshot pipeline takes it (see [`StructShapes::clear`]), never
/// while a [`Lock`] is in use. A [`StructShape`] can therefore point into it for as long as its
/// lock's scope.
#[derive(Default)]
pub struct StructShapes {
    shapes: HashMap<TypeId, NonNull<Shape>, BuildHasherDefault<TypeIdHasher>>,
}

impl Shape {
    /// Creates the handles of a struct whose fields' JavaScript names are `keys`.
    fn new(lock: &mut Lock<'_>, keys: &[&str]) -> Self {
        let names = keys
            .iter()
            .map(|key| {
                let Some(name) = v8::String::new_internalized_from_str(lock, key) else {
                    unreachable!("a field name is far shorter than V8's string length limit");
                };
                v8::Global::new(lock, name.into())
            })
            .collect();
        let template = lock.feature_flags().get_fast_jsg_struct().then(|| {
            // SAFETY: the isolate is valid and locked (guaranteed by Lock); the handle is a new,
            // strong `Global` of it that only the `Global` made of it resets.
            unsafe {
                let handle = ffi::dictionary_template_new(lock.isolate().as_ffi(), keys);
                v8::Global::from_ffi(lock, handle)
            }
        });
        Self { names, template }
    }
}

impl StructShapes {
    pub fn len(&self) -> usize {
        self.shapes.len()
    }

    /// Drops every shape, resetting its handles.
    ///
    /// # Safety
    ///
    /// The isolate must be locked, with no [`Lock`] in use, since a [`StructShape`] of the
    /// lock's scope may point into a shape.
    pub unsafe fn clear(&mut self) {
        for (_, shape) in self.shapes.drain() {
            // SAFETY: each pointer comes from `Box::leak` in `StructShape::of` and is dropped
            // once, here, since it was removed from the map.
            drop(unsafe { Box::from_raw(shape.as_ptr()) });
        }
    }
}

impl Drop for StructShapes {
    fn drop(&mut self) {
        // SAFETY: the realm, which owns the shapes, is dropped under the isolate lock with no
        // `Lock` in use; `Realm::drop` has already cleared them.
        unsafe { self.clear() };
    }
}

/// The cached handles of struct type `S`, for one conversion in the scope `'s`.
///
/// Used by the code `#[jsg_struct]` generates; not meant to be used directly.
#[doc(hidden)]
#[derive(Clone, Copy)]
pub struct StructShape<'s> {
    shape: NonNull<Shape>,
    _scope: PhantomData<&'s ()>,
}

impl<'s> StructShape<'s> {
    /// Gets, or on its first conversion in the realm creates, the shape of struct type `S`, whose
    /// fields' JavaScript names are `names`. Every call for `S` passes the same names.
    pub fn of<S: 'static>(lock: &mut Lock<'s>, names: &'static [&'static str]) -> Self {
        let type_id = TypeId::of::<S>();
        let shape = if let Some(&shape) = lock.realm().structs.shapes.get(&type_id) {
            shape
        } else {
            let shape = NonNull::from(Box::leak(Box::new(Shape::new(lock, names))));
            lock.realm().structs.shapes.insert(type_id, shape);
            shape
        };
        Self {
            shape,
            _scope: PhantomData,
        }
    }

    fn shape(&self) -> &Shape {
        // SAFETY: the shape outlives the lock of `'s` (see `StructShapes`).
        unsafe { self.shape.as_ref() }
    }

    /// Whether the realm has the `fast_jsg_struct` flag, so that [`new_instance`] applies.
    ///
    /// [`new_instance`]: Self::new_instance
    #[must_use]
    pub fn is_fast(&self) -> bool {
        self.shape().template.is_some()
    }

    /// Reads field `index` of `object`, like [`Local::<Object>::get`](v8::Local::get).
    ///
    /// # Errors
    ///
    /// As for [`Local::<Object>::get`](v8::Local::get).
    pub fn get(
        &self,
        lock: &mut Lock<'s>,
        object: v8::Local<'s, v8::Object>,
        index: usize,
    ) -> crate::Result<v8::Local<'s, v8::Value>> {
        let name = self.shape().names[index].as_ffi();
        call_into_js(lock, |lock| {
            // SAFETY: the isolate is valid and locked (guaranteed by Lock), `object` is a valid
            // handle, and `name` a handle of the realm of the isolate. The result was created in
            // the current HandleScope.
            Ok(unsafe {
                v8::Local::from_ffi_maybe(ffi::local_object_get_property_by_global_name(
                    lock.isolate().as_ffi(),
                    object.raw(),
                    &name,
                ))
            })
        })
    }

    /// Sets field `index` of `object` to `value`, like [`Local::<Object>::set`](v8::Local::set).
    ///
    /// # Errors
    ///
    /// As for [`Local::<Object>::set`](v8::Local::set).
    pub fn set(
        &self,
        lock: &mut Lock<'s>,
        object: v8::Local<'s, v8::Object>,
        index: usize,
        value: v8::Local<'s, v8::Value>,
    ) -> crate::Result<()> {
        let name = self.shape().names[index].as_ffi();
        call_into_js(lock, |lock| {
            // SAFETY: the isolate is valid and locked (guaranteed by Lock), `object` and `value`
            // are valid handles, and `name` a handle of the realm of the isolate.
            let ok = unsafe {
                ffi::local_object_set_property_by_global_name(
                    lock.isolate().as_ffi(),
                    object.raw(),
                    &name,
                    value.into_ffi(),
                )
            };
            Ok(ok.then_some(()))
        })
    }

    /// Creates an object with `values[i]` the value of field `i`, from the cached template. No
    /// JavaScript runs.
    ///
    /// # Panics
    ///
    /// Panics if the realm does not have `fast_jsg_struct` (see [`is_fast`](Self::is_fast)).
    /// There must be a value for each field.
    ///
    /// # Errors
    ///
    /// If an exception or a termination is pending, it stays pending and the error is one for
    /// which [`Error::is_pending`](crate::Error::is_pending) is true.
    pub fn new_instance<const N: usize>(
        &self,
        lock: &mut Lock<'s>,
        values: [v8::Local<'s, v8::Value>; N],
    ) -> crate::Result<v8::Local<'s, v8::Object>> {
        let shape = self.shape();
        // V8 CHECK-fails on a mismatch.
        assert_eq!(N, shape.names.len(), "a value for each field");
        // V8 handles may only be stored on the stack, as this array is.
        let mut values = values.map(|value| ffi::MaybeLocal {
            ptr: value.raw().ptr,
        });
        let Some(template) = shape.template.as_ref().map(v8::Global::as_ffi) else {
            unreachable!("new_instance without fast_jsg_struct");
        };
        call_into_js(lock, |lock| {
            // SAFETY: the isolate is valid and locked, a context is entered and a HandleScope is
            // active (guaranteed by Lock); the values are valid handles in it, on the stack, as
            // many as the template declares. The result belongs to that scope.
            Ok(unsafe {
                v8::Local::from_ffi_maybe(ffi::dictionary_template_new_instance(
                    lock.isolate().as_ffi(),
                    &template,
                    &mut values,
                ))
            })
        })
    }
}
