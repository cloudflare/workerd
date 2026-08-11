#![allow(
    clippy::boxed_local,
    clippy::missing_errors_doc,
    clippy::missing_safety_doc,
    clippy::must_use_candidate,
    clippy::needless_lifetimes,
    clippy::needless_pass_by_value,
    clippy::unnecessary_literal_bound,
    clippy::unnecessary_wraps,
    clippy::unused_self
)]
#![allow(unknown_lints)]
#![warn(rust_2024_compatibility)]
#![forbid(unsafe_op_in_unsafe_fn)]
#![deny(warnings)] // Check that expansion of `cxx::bridge` doesn't trigger warnings.

pub mod cast;
pub mod module;

use core::fmt;
use std::fmt::Display;
use std::mem::MaybeUninit;
use std::os::raw::c_char;

use cxx::CxxString;
use cxx::CxxVector;
use cxx::ExternType;
use cxx::KjError;
use cxx::KjExceptionType;
use cxx::SharedPtr;
use cxx::UniquePtr;
use cxx::type_id;
// The bridge parser accepts the unqualified smart-pointer name, while expansion
// fully qualifies the emitted Rust field type.
use kj_rs::{KjArc, KjOwn, KjRc};

pub type ImportedKjArc = KjArc<()>;
pub type ImportedKjOwn = KjOwn<()>;
pub type ImportedKjRc = KjRc<()>;

#[cxx::bridge(namespace = "tests")]
pub mod ffi {
    #[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord)]
    struct Shared {
        z: usize,
    }

    #[derive(PartialEq, Eq, PartialOrd)]
    struct SharedString {
        msg: String,
    }

    #[derive(JsgStruct)]
    struct CaaRecord {
        critical: u8,
        field: String,
        value: String,
    }

    struct SharedWithKjOwn {
        own: KjOwn<C>,
    }

    struct SharedWithMultipleKjOwns {
        first: KjOwn<C>,
        second: KjOwn<C>,
    }

    struct SharedWithKjRc {
        rc: KjRc<RcC>,
    }

    struct SharedWithMultipleKjRcs {
        first: KjRc<RcC>,
        second: KjRc<RcC>,
    }

    struct SharedWithKjArc {
        arc: KjArc<ArcC>,
    }

    struct SharedWithMultipleKjArcs {
        first: KjArc<ArcC>,
        second: KjArc<ArcC>,
    }

    #[derive(Debug, Hash, PartialOrd, Ord)]
    enum Enum {
        AVal,
        BVal = 2020,
        #[cxx_name = "CVal"]
        LastVal,
    }

    #[namespace = "A"]
    #[derive(Copy, Clone, Default)]
    struct AShared {
        #[cxx_name = "type"]
        z: usize,
    }

    #[namespace = "A"]
    enum AEnum {
        AAVal,
        ABVal = 2020,
        ACVal,
    }

    #[namespace = "A::B"]
    enum ABEnum {
        ABAVal,
        ABBVal = 2020,
        ABCVal,
    }

    #[namespace = "A::B"]
    #[derive(Clone)]
    struct ABShared {
        z: usize,
    }

    #[namespace = "first"]
    struct First {
        second: Box<Second>,
    }

    #[namespace = "second"]
    #[derive(Hash)]
    struct Second {
        i: i32,
        e: COwnedEnum,
    }

    pub struct Array {
        a: [i32; 4],
        b: Buffer,
    }

    #[derive(Copy, Clone, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
    pub struct StructWithLifetime<'a> {
        s: &'a str,
    }

    unsafe extern "C++" {
        include!("tests/ffi/tests.h");

        type C;
        fn c_return_primitive() -> usize;
        fn c_return_shared() -> Shared;
        fn c_return_box() -> Box<R>;
        fn c_return_unique_ptr() -> UniquePtr<C>;
        fn c_return_shared_ptr() -> SharedPtr<C>;
        unsafe fn c_return_mut<'a>(shared: &'a mut Shared) -> &'a mut usize;
        unsafe fn c_return_str<'a>(shared: &'a Shared) -> &'a str;
        unsafe fn c_return_slice_char<'a>(shared: &'a Shared) -> &'a [c_char];
        unsafe fn c_return_mutsliceu8<'a>(slice: &'a mut [u8]) -> &'a mut [u8];
        unsafe fn c_return_ref<'a>(shared: &'a Shared) -> &'a usize;
        fn c_return_rust_string() -> String;
        fn c_return_rust_string_lossy() -> String;
        fn c_return_unique_ptr_string() -> UniquePtr<CxxString>;
        fn c_return_unique_ptr_vector_u8() -> UniquePtr<CxxVector<u8>>;
        fn c_return_unique_ptr_vector_f64() -> UniquePtr<CxxVector<f64>>;
        fn c_return_unique_ptr_vector_string() -> UniquePtr<CxxVector<CxxString>>;
        fn c_return_unique_ptr_vector_shared() -> UniquePtr<CxxVector<Shared>>;
        fn c_return_unique_ptr_vector_opaque() -> UniquePtr<CxxVector<C>>;
        unsafe fn c_return_ref_vector<'a>(c: &'a C) -> &'a CxxVector<u8>;
        unsafe fn c_return_mut_vector<'a>(c: Pin<&'a mut C>) -> Pin<&'a mut CxxVector<u8>>;
        fn c_return_rust_vec_u8() -> Vec<u8>;
        unsafe fn c_return_ref_rust_vec<'a>(c: &'a C) -> &'a Vec<u8>;
        unsafe fn c_return_mut_rust_vec<'a>(c: Pin<&'a mut C>) -> &'a mut Vec<u8>;
        fn c_return_rust_vec_string() -> Vec<String>;
        fn c_return_rust_vec_bool() -> Vec<bool>;
        fn c_return_identity(_: usize) -> usize;
        fn c_return_sum(_: usize, _: usize) -> usize;
        fn c_return_enum(n: u16) -> Enum;
        unsafe fn c_return_ns_ref<'a>(shared: &'a AShared) -> &'a usize;
        unsafe fn c_return_nested_ns_ref<'a>(shared: &'a ABShared) -> &'a usize;
        fn c_return_ns_enum(n: u16) -> AEnum;
        fn c_return_nested_ns_enum(n: u16) -> ABEnum;
        fn c_return_const_ptr(n: usize) -> *const C;
        fn c_return_mut_ptr(n: usize) -> *mut C;
        fn c_return_kj_own(n: usize) -> KjOwn<C>;
        fn c_sizeof_shared_with_kj_own() -> usize;
        fn c_alignof_shared_with_kj_own() -> usize;
        fn c_sizeof_shared_with_multiple_kj_owns() -> usize;
        fn c_alignof_shared_with_multiple_kj_owns() -> usize;
        fn c_return_shared_with_kj_own(n: usize) -> SharedWithKjOwn;
        fn c_return_shared_with_multiple_kj_owns(
            first: usize,
            second: usize,
        ) -> SharedWithMultipleKjOwns;
        fn c_take_shared_with_kj_own_by_value(shared: SharedWithKjOwn) -> usize;
        fn c_take_shared_with_kj_own_by_ref(shared: &SharedWithKjOwn) -> usize;
        fn c_take_shared_with_multiple_kj_owns_by_value(shared: SharedWithMultipleKjOwns) -> usize;
        fn c_take_shared_with_multiple_kj_owns_by_ref(shared: &SharedWithMultipleKjOwns) -> usize;
        fn c_roundtrip_shared_with_kj_own(shared: SharedWithKjOwn) -> SharedWithKjOwn;
        fn c_roundtrip_shared_with_multiple_kj_owns(
            shared: SharedWithMultipleKjOwns,
        ) -> SharedWithMultipleKjOwns;
        fn c_return_kj_rc(n: usize) -> KjRc<RcC>;
        fn c_return_non_refcounted_kj_rc(n: usize) -> KjRc<NonRefcountedRcC>;
        fn c_take_non_refcounted_kj_rc_by_ref(rc: &KjRc<NonRefcountedRcC>) -> usize;
        fn c_return_non_atomic_kj_arc(n: usize) -> KjArc<NonAtomicArcC>;
        fn c_take_non_atomic_kj_arc_by_ref(arc: &KjArc<NonAtomicArcC>) -> usize;
        fn c_sizeof_shared_with_kj_rc() -> usize;
        fn c_alignof_shared_with_kj_rc() -> usize;
        fn c_sizeof_shared_with_multiple_kj_rcs() -> usize;
        fn c_alignof_shared_with_multiple_kj_rcs() -> usize;
        fn c_return_shared_with_kj_rc(n: usize) -> SharedWithKjRc;
        fn c_return_shared_with_multiple_kj_rcs(
            first: usize,
            second: usize,
        ) -> SharedWithMultipleKjRcs;
        fn c_take_shared_with_kj_rc_by_value(shared: SharedWithKjRc) -> usize;
        fn c_take_shared_with_kj_rc_by_ref(shared: &SharedWithKjRc) -> usize;
        fn c_take_shared_with_multiple_kj_rcs_by_value(shared: SharedWithMultipleKjRcs) -> usize;
        fn c_take_shared_with_multiple_kj_rcs_by_ref(shared: &SharedWithMultipleKjRcs) -> usize;
        fn c_roundtrip_shared_with_kj_rc(shared: SharedWithKjRc) -> SharedWithKjRc;
        fn c_roundtrip_shared_with_multiple_kj_rcs(
            shared: SharedWithMultipleKjRcs,
        ) -> SharedWithMultipleKjRcs;
        fn c_return_kj_arc(n: usize) -> KjArc<ArcC>;
        fn c_sizeof_shared_with_kj_arc() -> usize;
        fn c_alignof_shared_with_kj_arc() -> usize;
        fn c_sizeof_shared_with_multiple_kj_arcs() -> usize;
        fn c_alignof_shared_with_multiple_kj_arcs() -> usize;
        fn c_return_shared_with_kj_arc(n: usize) -> SharedWithKjArc;
        fn c_return_shared_with_multiple_kj_arcs(
            first: usize,
            second: usize,
        ) -> SharedWithMultipleKjArcs;
        fn c_take_shared_with_kj_arc_by_value(shared: SharedWithKjArc) -> usize;
        fn c_take_shared_with_kj_arc_by_ref(shared: &SharedWithKjArc) -> usize;
        fn c_take_shared_with_multiple_kj_arcs_by_value(shared: SharedWithMultipleKjArcs) -> usize;
        fn c_take_shared_with_multiple_kj_arcs_by_ref(shared: &SharedWithMultipleKjArcs) -> usize;
        fn c_roundtrip_shared_with_kj_arc(shared: SharedWithKjArc) -> SharedWithKjArc;
        fn c_roundtrip_shared_with_multiple_kj_arcs(
            shared: SharedWithMultipleKjArcs,
        ) -> SharedWithMultipleKjArcs;

        fn c_take_primitive(n: usize);
        fn c_take_shared(shared: Shared);
        fn c_take_box(r: Box<R>);
        fn c_take_ref_r(r: &R);
        fn c_take_ref_c(c: &C);
        fn c_take_str(s: &str);
        fn c_take_slice_char(s: &[c_char]);
        fn c_take_slice_shared(s: &[Shared]);
        fn c_take_slice_shared_sort(s: &mut [Shared]);
        fn c_take_slice_r(s: &[R]);
        fn c_take_slice_r_sort(s: &mut [R]);
        fn c_take_rust_string(s: String);
        fn c_take_unique_ptr_string(s: UniquePtr<CxxString>);
        fn c_take_unique_ptr_vector_u8(v: UniquePtr<CxxVector<u8>>);
        fn c_take_unique_ptr_vector_f64(v: UniquePtr<CxxVector<f64>>);
        fn c_take_unique_ptr_vector_string(v: UniquePtr<CxxVector<CxxString>>);
        fn c_take_unique_ptr_vector_shared(v: UniquePtr<CxxVector<Shared>>);
        fn c_take_ref_vector(v: &CxxVector<u8>);
        fn c_take_rust_vec(v: Vec<u8>);
        fn c_take_rust_vec_shared(v: Vec<Shared>);
        fn c_take_rust_vec_string(v: Vec<String>);
        fn c_take_rust_vec_index(v: Vec<u8>);
        fn c_take_rust_vec_shared_index(v: Vec<Shared>);
        fn c_take_rust_vec_shared_push(v: Vec<Shared>);
        fn c_take_rust_vec_shared_truncate(v: Vec<Shared>);
        fn c_take_rust_vec_shared_clear(v: Vec<Shared>);
        fn c_take_rust_vec_shared_forward_iterator(v: Vec<Shared>);
        fn c_take_rust_vec_shared_sort(v: Vec<Shared>);
        fn c_take_ref_rust_vec(v: &Vec<u8>);
        fn c_take_ref_rust_vec_string(v: &Vec<String>);
        fn c_take_ref_rust_vec_index(v: &Vec<u8>);
        fn c_take_ref_rust_vec_copy(v: &Vec<u8>);
        unsafe fn c_take_ref_shared_string<'a>(s: &'a SharedString) -> &'a SharedString;
        fn c_take_callback(callback: fn(String) -> usize);
        fn c_take_callback_ref(callback: fn(&String));
        #[cxx_name = "c_take_callback_ref"]
        fn c_take_callback_ref_lifetime<'a>(callback: fn(&'a String));
        fn c_take_callback_mut(callback: fn(&mut String));
        fn c_take_enum(e: Enum);
        fn c_take_ns_enum(e: AEnum);
        fn c_take_nested_ns_enum(e: ABEnum);
        fn c_take_ns_shared(shared: AShared);
        fn c_take_nested_ns_shared(shared: ABShared);
        fn c_take_rust_vec_ns_shared(v: Vec<AShared>);
        fn c_take_rust_vec_nested_ns_shared(v: Vec<ABShared>);
        unsafe fn c_take_const_ptr(c: *const C) -> usize;
        unsafe fn c_take_mut_ptr(c: *mut C) -> usize;

        fn c_try_return_void() -> Result<()>;
        fn c_try_return_primitive() -> Result<usize>;
        fn c_fail_return_primitive() -> Result<usize>;
        fn c_fail_kj_exception_return_primitive() -> Result<usize>;
        fn c_fail_kj_exception_disconnected_return_primitive() -> Result<usize>;
        fn c_fail_kj_exception_with_details_return_primitive() -> Result<usize>;
        fn c_cancel_return_primitive() -> Result<usize>;
        fn c_cancel_via_rust_return_primitive() -> Result<usize>;
        fn c_cancel_roundtrip_return_primitive() -> Result<usize>;

        // These signatures are infallible, but the C++ implementations throw.
        // The exception has to become a panic; it must not abort the process.
        fn c_infallible_fail_void();
        fn c_infallible_fail_primitive() -> usize;
        fn c_infallible_fail_kj_exception_disconnected() -> usize;
        fn c_infallible_fail_rust_string() -> String;
        fn c_infallible_fail_foreign_exception() -> usize;
        fn c_infallible_cancel() -> usize;
        fn c_infallible_fail_roundtrip() -> usize;

        fn c_try_return_box() -> Result<Box<R>>;
        unsafe fn c_try_return_ref<'a>(s: &'a String) -> Result<&'a String>;
        unsafe fn c_try_return_str<'a>(s: &'a str) -> Result<&'a str>;
        unsafe fn c_try_return_sliceu8<'a>(s: &'a [u8]) -> Result<&'a [u8]>;
        unsafe fn c_try_return_mutsliceu8<'a>(s: &'a mut [u8]) -> Result<&'a mut [u8]>;
        fn c_try_return_rust_string() -> Result<String>;
        fn c_try_return_unique_ptr_string() -> Result<UniquePtr<CxxString>>;
        fn c_try_return_rust_vec() -> Result<Vec<u8>>;
        fn c_try_return_rust_vec_string() -> Result<Vec<String>>;
        unsafe fn c_try_return_ref_rust_vec<'a>(c: &'a C) -> Result<&'a Vec<u8>>;

        fn get(self: &C) -> usize;
        fn set(self: Pin<&mut C>, n: usize) -> usize;
        fn get2(&self) -> usize;
        unsafe fn getRef<'a>(self: &'a C) -> &'a usize;
        unsafe fn getMut<'a>(self: Pin<&'a mut C>) -> &'a mut usize;
        fn set_succeed(self: Pin<&mut C>, n: usize) -> Result<usize>;
        fn get_fail(self: Pin<&mut C>) -> Result<usize>;
        fn get_fail_infallible(self: Pin<&mut C>) -> usize;
        fn c_method_on_shared(self: &Shared) -> usize;
        unsafe fn c_method_ref_on_shared<'a>(self: &'a Shared) -> &'a usize;
        unsafe fn c_method_mut_on_shared<'a>(self: &'a mut Shared) -> &'a mut usize;
        fn c_set_array(self: &mut Array, value: i32);

        fn c_get_use_count(weak: &WeakPtr<C>) -> usize;

        #[rust_name = "i32_overloaded_method"]
        fn cOverloadedMethod(&self, x: i32) -> String;
        #[rust_name = "str_overloaded_method"]
        fn cOverloadedMethod(&self, x: &str) -> String;
        #[rust_name = "i32_overloaded_function"]
        fn cOverloadedFunction(x: i32) -> String;
        #[rust_name = "str_overloaded_function"]
        fn cOverloadedFunction(x: &str) -> String;

        #[namespace = "other"]
        fn ns_c_take_ns_shared(shared: AShared);
    }

    unsafe extern "C++" {
        include!("tests/ffi/tests.h");

        type NonRefcountedRcC;
        type RcC;
    }

    unsafe extern "C++" {
        include!("tests/ffi/tests.h");

        type ArcC;
        type NonAtomicArcC;
    }

    extern "C++" {
        include!("tests/ffi/module.rs.h");

        type COwnedEnum;
        type Job = crate::module::ffi::Job;
    }

    extern "Rust" {
        #[derive(ExternType)]
        type Reference<'a>;
    }

    unsafe extern "C++" {
        type Borrow<'a>;

        fn c_return_borrow<'a>(s: &'a CxxString) -> UniquePtr<Borrow<'a>>;

        #[rust_name = "c_return_borrow_elided"]
        fn c_return_borrow<'a>(s: &'a CxxString) -> UniquePtr<Borrow<'a>>;

        fn const_member(self: &Borrow);
        fn nonconst_member(self: Pin<&mut Borrow>);
    }

    #[repr(u32)]
    #[derive(Hash)]
    enum COwnedEnum {
        #[cxx_name = "CVAL1"]
        CVal1,
        #[cxx_name = "CVAL2"]
        CVal2,
    }

    extern "C++" {
        type Buffer = crate::Buffer;
    }

    extern "Rust" {
        type R;

        fn r_return_primitive() -> usize;
        fn r_return_shared() -> Shared;
        fn r_return_box() -> Box<R>;
        fn r_return_unique_ptr() -> UniquePtr<C>;
        fn r_return_shared_ptr() -> SharedPtr<C>;
        unsafe fn r_return_ref<'a>(shared: &'a Shared) -> &'a usize;
        unsafe fn r_return_mut<'a>(shared: &'a mut Shared) -> &'a mut usize;
        unsafe fn r_return_str<'a>(shared: &'a Shared) -> &'a str;
        unsafe fn r_return_str_via_out_param<'a>(shared: &'a Shared, out_param: &mut &'a str);
        unsafe fn r_return_sliceu8<'a>(shared: &'a Shared) -> &'a [u8];
        unsafe fn r_return_mutsliceu8<'a>(slice: &'a mut [u8]) -> &'a mut [u8];
        fn r_return_rust_string() -> String;
        fn r_return_unique_ptr_string() -> UniquePtr<CxxString>;
        fn r_return_rust_vec() -> Vec<u8>;
        fn r_return_rust_vec_string() -> Vec<String>;
        fn r_return_rust_vec_extern_struct() -> Vec<Job>;
        unsafe fn r_return_ref_rust_vec<'a>(shared: &'a Shared) -> &'a Vec<u8>;
        unsafe fn r_return_mut_rust_vec<'a>(shared: &'a mut Shared) -> &'a mut Vec<u8>;
        fn r_return_identity(_: usize) -> usize;
        fn r_return_sum(_: usize, _: usize) -> usize;
        fn r_return_enum(n: u32) -> Enum;

        fn r_take_primitive(n: usize);
        fn r_take_shared(shared: Shared);
        fn r_take_box(r: Box<R>);
        fn r_take_unique_ptr(c: UniquePtr<C>);
        fn r_take_shared_ptr(c: SharedPtr<C>);
        fn r_take_ref_r(r: &R);
        fn r_take_ref_c(c: &C);
        fn r_take_str(s: &str);
        fn r_take_slice_char(s: &[c_char]);
        fn r_take_rust_string(s: String);
        fn r_take_unique_ptr_string(s: UniquePtr<CxxString>);
        fn r_take_ref_vector(v: &CxxVector<u8>);
        fn r_take_ref_empty_vector(v: &CxxVector<u64>);
        fn r_take_rust_vec(v: Vec<u8>);
        fn r_take_rust_vec_string(v: Vec<String>);
        fn r_take_ref_rust_vec(v: &Vec<u8>);
        fn r_take_ref_rust_vec_string(v: &Vec<String>);
        fn r_take_enum(e: Enum);

        fn r_try_return_void() -> Result<()>;
        fn r_try_return_primitive() -> Result<usize>;
        fn r_try_return_box() -> Result<Box<R>>;
        fn r_fail_return_primitive() -> Result<usize>;

        fn r_result_kj_exception_return_primitive() -> Result<usize>;
        fn r_result_kj_exception_fail_return_primitive() -> Result<usize>;
        fn r_result_kj_exception_disconnected_return_primitive() -> Result<usize>;
        fn r_result_kj_exception_with_details_return_primitive() -> Result<usize>;
        fn r_cancel_panic_test();
        fn r_call_c_cancel_return_primitive();
        fn r_call_c_infallible_fail_primitive();
        fn r_cancel_via_cpp_return_primitive() -> Result<usize>;
        fn r_cancel_roundtrip_return_primitive() -> Result<usize>;

        unsafe fn r_try_return_sliceu8<'a>(s: &'a [u8]) -> Result<&'a [u8]>;
        unsafe fn r_try_return_mutsliceu8<'a>(s: &'a mut [u8]) -> Result<&'a mut [u8]>;

        fn get(self: &R) -> usize;
        fn set(self: &mut R, n: usize) -> usize;
        fn r_method_on_shared(self: &Shared) -> String;
        fn r_get_array_sum(self: &Array) -> i32;

        #[cxx_name = "rAliasedFunction"]
        fn r_aliased_function(x: i32) -> String;

        fn r_panic(s: &str);
    }

    struct Dag0 {
        i: i32,
    }

    struct Dag1 {
        dag2: Dag2,
        vec: Vec<Dag3>,
    }

    struct Dag2 {
        dag4: Dag4,
    }

    struct Dag3 {
        dag1: Dag1,
    }

    struct Dag4 {
        dag0: Dag0,
    }

    impl Box<Shared> {}
    impl CxxVector<SharedString> {}
}

mod other {
    use cxx::CxxString;
    use cxx::ExternType;
    use cxx::kind::Opaque;
    use cxx::kind::Trivial;
    use cxx::type_id;

    #[repr(C)]
    pub struct D {
        pub d: u64,
    }

    #[repr(C)]
    pub struct E {
        e: u64,
        e_str: CxxString,
    }

    pub mod f {
        use cxx::CxxString;
        use cxx::ExternType;
        use cxx::kind::Opaque;
        use cxx::type_id;

        #[repr(C)]
        pub struct F {
            e: u64,
            e_str: CxxString,
        }

        // Safety: the test fixture provides valid bridge pointers and matching layouts.
        unsafe impl ExternType for F {
            type Id = type_id!("F::F");
            type Kind = Opaque;
        }
    }

    #[repr(C)]
    pub struct G {
        pub g: u64,
    }

    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe impl ExternType for G {
        type Id = type_id!("G::G");
        type Kind = Trivial;
    }

    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe impl ExternType for D {
        type Id = type_id!("tests::D");
        type Kind = Trivial;
    }

    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe impl ExternType for E {
        type Id = type_id!("tests::E");
        type Kind = Opaque;
    }
}

#[derive(PartialEq, Eq, Debug)]
pub struct R(pub usize);

impl R {
    fn get(&self) -> usize {
        self.0
    }

    fn set(&mut self, n: usize) -> usize {
        self.0 = n;
        n
    }
}

pub struct Reference<'a>(pub &'a String);

impl ffi::Shared {
    fn r_method_on_shared(&self) -> String {
        "2020".to_owned()
    }
}

impl ffi::Array {
    pub fn r_get_array_sum(&self) -> i32 {
        self.a.iter().sum()
    }
}

#[derive(Default)]
#[repr(C)]
pub struct Buffer([c_char; 12]);

// Safety: the test fixture provides valid bridge pointers and matching layouts.
unsafe impl ExternType for Buffer {
    type Id = type_id!("tests::Buffer");
    type Kind = cxx::kind::Trivial;
}

#[derive(Debug)]
struct Error;

impl std::error::Error for Error {}

impl Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        f.write_str("rust error")
    }
}

fn r_return_primitive() -> usize {
    2020
}

fn r_return_shared() -> ffi::Shared {
    ffi::Shared { z: 2020 }
}

fn r_return_box() -> Box<R> {
    Box::new(R(2020))
}

fn r_return_unique_ptr() -> UniquePtr<ffi::C> {
    unsafe extern "C" {
        fn cxx_test_suite_get_unique_ptr() -> *mut ffi::C;
    }
    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe { UniquePtr::from_raw(cxx_test_suite_get_unique_ptr()) }
}

fn r_return_shared_ptr() -> SharedPtr<ffi::C> {
    unsafe extern "C" {
        fn cxx_test_suite_get_shared_ptr(repr: *mut SharedPtr<ffi::C>);
    }
    let mut shared_ptr = MaybeUninit::<SharedPtr<ffi::C>>::uninit();
    let repr = shared_ptr.as_mut_ptr();
    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe {
        cxx_test_suite_get_shared_ptr(repr);
        shared_ptr.assume_init()
    }
}

fn r_return_ref(shared: &ffi::Shared) -> &usize {
    &shared.z
}

fn r_return_mut(shared: &mut ffi::Shared) -> &mut usize {
    &mut shared.z
}

fn r_return_str(shared: &ffi::Shared) -> &str {
    let _ = shared;
    "2020"
}

fn r_return_str_via_out_param<'a>(shared: &'a ffi::Shared, out_param: &mut &'a str) {
    let _ = shared;
    *out_param = "2020";
}

fn r_return_sliceu8(shared: &ffi::Shared) -> &[u8] {
    let _ = shared;
    b"2020"
}

fn r_return_mutsliceu8(slice: &mut [u8]) -> &mut [u8] {
    slice
}

fn r_return_rust_string() -> String {
    "2020".to_owned()
}

fn r_return_unique_ptr_string() -> UniquePtr<CxxString> {
    unsafe extern "C" {
        fn cxx_test_suite_get_unique_ptr_string() -> *mut CxxString;
    }
    // Safety: the test fixture provides valid bridge pointers and matching layouts.
    unsafe { UniquePtr::from_raw(cxx_test_suite_get_unique_ptr_string()) }
}

fn r_return_rust_vec() -> Vec<u8> {
    Vec::new()
}

fn r_return_rust_vec_string() -> Vec<String> {
    Vec::new()
}

fn r_return_rust_vec_extern_struct() -> Vec<ffi::Job> {
    Vec::new()
}

fn r_return_ref_rust_vec(shared: &ffi::Shared) -> &Vec<u8> {
    let _ = shared;
    unimplemented!()
}

fn r_return_mut_rust_vec(shared: &mut ffi::Shared) -> &mut Vec<u8> {
    shared.z += 0;
    unimplemented!()
}

fn r_return_identity(n: usize) -> usize {
    n
}

fn r_return_sum(n1: usize, n2: usize) -> usize {
    n1 + n2
}

fn r_return_enum(n: u32) -> ffi::Enum {
    if n == 0 {
        ffi::Enum::AVal
    } else if n <= 2020 {
        ffi::Enum::BVal
    } else {
        ffi::Enum::LastVal
    }
}

fn r_take_primitive(n: usize) {
    assert_eq!(n, 2020);
}

fn r_take_shared(shared: ffi::Shared) {
    assert_eq!(shared.z, 2020);
}

fn r_take_box(r: Box<R>) {
    let _ = r;
}

fn r_take_unique_ptr(c: UniquePtr<ffi::C>) {
    let _ = c;
}

fn r_take_shared_ptr(c: SharedPtr<ffi::C>) {
    let _ = c;
}

fn r_take_ref_r(r: &R) {
    let _ = r;
}

fn r_take_ref_c(c: &ffi::C) {
    let _ = c;
}

fn r_take_str(s: &str) {
    assert_eq!(s, "2020");
}

fn r_take_rust_string(s: String) {
    assert_eq!(s, "2020");
}

fn r_take_slice_char(s: &[c_char]) {
    assert_eq!(s.len(), 5);
    let s = cast::c_char_to_unsigned(s);
    assert_eq!(std::str::from_utf8(s), Ok("2020\0"));
}

fn r_take_unique_ptr_string(s: UniquePtr<CxxString>) {
    assert_eq!(
        s.as_ref().and_then(|value| value.to_str().ok()),
        Some("2020")
    );
}

fn r_take_ref_vector(v: &CxxVector<u8>) {
    let slice = v.as_slice();
    assert_eq!(slice, [20, 2, 0]);
}

fn r_take_ref_empty_vector(v: &CxxVector<u64>) {
    assert!(v.as_slice().is_empty());
    assert!(v.is_empty());
}

fn r_take_rust_vec(v: Vec<u8>) {
    let _ = v;
}

fn r_take_rust_vec_string(v: Vec<String>) {
    let _ = v;
}

fn r_take_ref_rust_vec(v: &Vec<u8>) {
    let _ = v;
}

fn r_take_ref_rust_vec_string(v: &Vec<String>) {
    let _ = v;
}

fn r_take_enum(e: ffi::Enum) {
    let _ = e;
}

fn r_try_return_void() -> Result<(), Error> {
    Ok(())
}

fn r_try_return_primitive() -> Result<usize, Error> {
    Ok(2020)
}

fn r_try_return_box() -> Result<Box<R>, Error> {
    Ok(Box::new(R(2020)))
}

fn r_fail_return_primitive() -> Result<usize, Error> {
    Err(Error)
}

fn r_result_kj_exception_return_primitive() -> Result<usize, KjError> {
    Ok(2020)
}

fn r_result_kj_exception_fail_return_primitive() -> Result<usize, KjError> {
    Err(KjError::new(
        KjExceptionType::Disconnected,
        "test kj exception".to_owned(),
    ))
}

fn r_result_kj_exception_disconnected_return_primitive() -> Result<usize, KjError> {
    Err(KjError::new(
        KjExceptionType::Disconnected,
        "connection lost from rust".to_owned(),
    )
    .with_location(String::from("rust/cxx/tests/ffi/lib.rs"), 675))
}

fn r_result_kj_exception_with_details_return_primitive() -> Result<usize, KjError> {
    // Create test details
    let details = vec![
        (123u64, b"rust detail 1".to_vec()),
        (456u64, b"rust detail 2".to_vec()),
    ];

    Err(KjError::new(
        KjExceptionType::Failed,
        "rust exception with details".to_owned(),
    )
    .with_location(String::from("rust/cxx/tests/ffi/lib.rs"), 685)
    .with_details(details))
}

// Panic with CanceledException to simulate cancellation
fn r_cancel_panic_test() {
    cxx::CanceledException::panic()
}

// Test calling C++ function that throws CanceledException
// This should result in a panic that propagates back
fn r_call_c_cancel_return_primitive() {
    let _result = ffi::c_cancel_return_primitive();
    unreachable!();
}

// Calling an infallible C++ function which throws panics. Neither this function
// nor the C++ shim it calls declares a Result, so the exception has to travel
// back to C++ as a panic converted at this function's own ffi boundary.
fn r_call_c_infallible_fail_primitive() {
    let _result = ffi::c_infallible_fail_primitive();
    unreachable!();
}

fn r_cancel_via_cpp_return_primitive() -> Result<usize, Error> {
    // Call C++ function that throws CanceledException
    match std::panic::catch_unwind(ffi::c_cancel_return_primitive) {
        Ok(result) => result.map_err(|_| Error),
        Err(_) => Err(Error), // Panic caught, likely CanceledException
    }
}

fn r_cancel_roundtrip_return_primitive() -> Result<usize, Error> {
    // Test Rust->C++->Rust cancellation roundtrip
    match std::panic::catch_unwind(ffi::c_cancel_roundtrip_return_primitive) {
        Ok(result) => result.map_err(|_| Error),
        Err(_) => Err(Error), // Panic caught, likely CanceledException
    }
}

fn r_try_return_sliceu8(slice: &[u8]) -> Result<&[u8], Error> {
    Ok(slice)
}

fn r_try_return_mutsliceu8(slice: &mut [u8]) -> Result<&mut [u8], Error> {
    Ok(slice)
}

fn r_aliased_function(x: i32) -> String {
    x.to_string()
}

fn r_panic(s: &str) {
    panic!("{s}");
}
