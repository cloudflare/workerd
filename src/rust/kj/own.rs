use std::ops::Deref;
use std::pin::Pin;

use kj_rs::KjOwn;

/// Wrapper for C++ objects.
///
/// C++ objects are either passed by references or are owned by rust using `kj::Own`.
/// `OwnOrRef` gets uniform access to all cases.
///
/// Instances of this class are not expected to be exposed to the user directly, but as a building
/// block for ffi wrappers.
///
/// Usage Guidelines:
///
/// To achieve maximum compatibility with Rust's type system, C++ objects should be represented
/// on the Rust side as some `struct Wrapper(OwnOrRef<T>)` with inaccessible tuple value.
///
/// C++ can pass an object to Rust three ways:
///
/// - as `kj::Own<T>` - these should be represented as `Wrapper` objects passed by-value on Rust
///   side: `kj::Own<Foo> createFoo()` on C++ should be translated to `createFoo() -> FooWrapper`
///   on Rust.
///
/// - as `const T&` - these should be represented as non-mutable `&Wrapper` references on Rust
///   side: `bar(const Foo& foo)` on C++ side should be translated to `bar(foo: &FooWrapper)` on
///   Rust.
///
/// - as `T&` - these should be represented as mutable `&Wrapper` references on Rust side:
///   `bar(T& foo)` on C++ side should be translated to `bar(foo: &mut FooWrapper)` on Rust.
///
/// When passing object from rust to C++ do similarly:
///
/// - if C++ side accepts `kj::Own<T>` - accept `Wrapper` objects by-value
///
/// - if C++ side accepts `const T&` - accept `&Wrapper` references
///
/// - if C++ side accepts `T&` - accept `&mut Wrapper` references
pub enum OwnOrRef<'a, T> {
    Own(KjOwn<T>),
    Ref(&'a T),
    MutRef(Pin<&'a mut T>),
}

impl<T> AsRef<T> for OwnOrRef<'_, T> {
    fn as_ref(&self) -> &T {
        match self {
            OwnOrRef::Own(own) => own.as_ref(),
            OwnOrRef::Ref(ref_) => ref_,
            OwnOrRef::MutRef(ref_) => ref_,
        }
    }
}

impl<T> OwnOrRef<'_, T> {
    /// Obtain mut reference to the underlying object.
    ///
    /// # Safety
    ///
    /// - self should not be `Ref` variant.
    ///
    /// C++ mutable references are represented by `Pin<&mut T>`, otherwise we'd implement `AsMut`.
    pub unsafe fn as_mut(&mut self) -> Pin<&mut T> {
        match self {
            OwnOrRef::Own(own) => own.as_mut(),
            OwnOrRef::Ref(_) => unreachable!("mut reference to borrowed object"),
            OwnOrRef::MutRef(ref_) => ref_.as_mut(),
        }
    }
}

impl<T> Deref for OwnOrRef<'_, T> {
    type Target = T;

    fn deref(&self) -> &Self::Target {
        self.as_ref()
    }
}

impl<'a, T> From<&'a T> for OwnOrRef<'a, T> {
    fn from(value: &'a T) -> Self {
        Self::Ref(value)
    }
}

impl<T> From<KjOwn<T>> for OwnOrRef<'_, T> {
    fn from(value: KjOwn<T>) -> Self {
        Self::Own(value)
    }
}

impl<'a, T> From<Pin<&'a mut T>> for OwnOrRef<'a, T> {
    fn from(value: Pin<&'a mut T>) -> Self {
        Self::MutRef(value)
    }
}
