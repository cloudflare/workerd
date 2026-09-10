#![allow(
    clippy::assertions_on_constants,
    clippy::cast_possible_truncation,
    clippy::cast_possible_wrap,
    clippy::float_cmp,
    clippy::needless_pass_by_value,
    clippy::ptr_cast_constness,
    clippy::unit_cmp
)]

use std::cell::Cell;
use std::ffi::CStr;
use std::mem::align_of;
use std::mem::size_of;
use std::panic::RefUnwindSafe;
use std::panic::UnwindSafe;
use std::panic::{self};

use cxx::SharedPtr;
use cxx::UniquePtr;
use cxx_test_suite::cast;
use cxx_test_suite::ffi;
use cxx_test_suite::module::ffi2;

thread_local! {
    static CORRECT: Cell<bool> = const { Cell::new(false) };
}

#[unsafe(no_mangle)]
extern "C" fn cxx_test_suite_set_correct() {
    CORRECT.with(|correct| correct.set(true));
}

macro_rules! check {
    ($run:expr) => {{
        CORRECT.with(|correct| correct.set(false));
        $run;
        assert!(CORRECT.with(Cell::get), "{}", stringify!($run));
    }};
}

#[test]
fn test_c_return() {
    let shared = ffi::Shared { z: 2020 };
    let ns_shared = ffi::AShared { z: 2020 };
    let nested_ns_shared = ffi::ABShared { z: 2020 };

    assert_eq!(2020, ffi::c_return_primitive());
    assert_eq!(2020, ffi::c_return_shared().z);
    assert_eq!(2020, ffi::c_return_box().0);
    assert_eq!(2020, ffi2::c_return_box_from_aliased_rust_type().0);
    ffi::c_return_unique_ptr();
    ffi2::c_return_ns_unique_ptr();
    // SAFETY: The returned borrows are bounded by their live input values.
    unsafe {
        assert_eq!(2020, *ffi::c_return_ref(&shared));
        assert_eq!(2020, *ffi::c_return_ns_ref(&ns_shared));
        assert_eq!(2020, *ffi::c_return_nested_ns_ref(&nested_ns_shared));
        assert_eq!("2020", ffi::c_return_str(&shared));
        assert_eq!(
            b"2020\0",
            cast::c_char_to_unsigned(ffi::c_return_slice_char(&shared)),
        );
    }
    assert_eq!("2020", ffi::c_return_rust_string());
    assert_eq!("Hello \u{fffd}World", ffi::c_return_rust_string_lossy());
    assert_eq!("2020", ffi::c_return_unique_ptr_string().to_str().unwrap());
    assert_eq!(c"2020", ffi::c_return_unique_ptr_string().as_c_str());
    assert_eq!(4, ffi::c_return_unique_ptr_vector_u8().len());
    assert_eq!(
        200_u8,
        ffi::c_return_unique_ptr_vector_u8().into_iter().sum(),
    );
    assert_eq!(
        200.5_f64,
        ffi::c_return_unique_ptr_vector_f64().into_iter().sum(),
    );
    assert_eq!(2, ffi::c_return_unique_ptr_vector_shared().len());
    assert_eq!(
        2021_usize,
        ffi::c_return_unique_ptr_vector_shared()
            .into_iter()
            .map(|o| o.z)
            .sum(),
    );
    assert_eq!(b"\x02\0\x02\0"[..], ffi::c_return_rust_vec_u8());
    assert_eq!([true, true, false][..], ffi::c_return_rust_vec_bool());
    assert_eq!(2020, ffi::c_return_identity(2020));
    assert_eq!(2021, ffi::c_return_sum(2020, 1));
    match ffi::c_return_enum(0) {
        enm @ ffi::Enum::AVal => assert_eq!(0, enm.repr),
        _ => assert!(false),
    }
    match ffi::c_return_enum(1) {
        enm @ ffi::Enum::BVal => assert_eq!(2020, enm.repr),
        _ => assert!(false),
    }
    match ffi::c_return_enum(2021) {
        enm @ ffi::Enum::LastVal => assert_eq!(2021, enm.repr),
        _ => assert!(false),
    }
    match ffi::c_return_ns_enum(0) {
        enm @ ffi::AEnum::AAVal => assert_eq!(0, enm.repr),
        _ => assert!(false),
    }
    match ffi::c_return_nested_ns_enum(0) {
        enm @ ffi::ABEnum::ABAVal => assert_eq!(0, enm.repr),
        _ => assert!(false),
    }
}

#[test]
fn test_kj_own_shared_struct_abi() {
    assert_eq!(
        size_of::<ffi::SharedWithKjOwn>(),
        ffi::c_sizeof_shared_with_kj_own()
    );
    assert_eq!(
        align_of::<ffi::SharedWithKjOwn>(),
        ffi::c_alignof_shared_with_kj_own()
    );
    assert_eq!(
        size_of::<ffi::SharedWithMultipleKjOwns>(),
        ffi::c_sizeof_shared_with_multiple_kj_owns()
    );
    assert_eq!(
        align_of::<ffi::SharedWithMultipleKjOwns>(),
        ffi::c_alignof_shared_with_multiple_kj_owns()
    );
}

#[test]
fn test_kj_own_shared_struct_roundtrip() {
    let shared = ffi::c_return_shared_with_kj_own(3030);
    assert_eq!(3030, shared.own.get());
    assert_eq!(3030, ffi::c_take_shared_with_kj_own_by_ref(&shared));
    assert_eq!(3030, ffi::c_take_shared_with_kj_own_by_value(shared));

    let shared = ffi::c_return_shared_with_multiple_kj_owns(1010, 2020);
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_owns_by_ref(&shared)
    );
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_owns_by_value(shared)
    );

    let shared = ffi::SharedWithKjOwn {
        own: ffi::c_return_kj_own(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_kj_own(shared);
    assert_eq!(2021, shared.own.get());

    let shared = ffi::SharedWithMultipleKjOwns {
        first: ffi::c_return_kj_own(1010),
        second: ffi::c_return_kj_own(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_multiple_kj_owns(shared);
    assert_eq!(1020, shared.first.get());
    assert_eq!(2040, shared.second.get());
}

#[test]
fn test_kj_rc_shared_struct_abi() {
    assert_eq!(
        size_of::<ffi::SharedWithKjRc>(),
        ffi::c_sizeof_shared_with_kj_rc()
    );
    assert_eq!(
        align_of::<ffi::SharedWithKjRc>(),
        ffi::c_alignof_shared_with_kj_rc()
    );
    assert_eq!(
        size_of::<ffi::SharedWithMultipleKjRcs>(),
        ffi::c_sizeof_shared_with_multiple_kj_rcs()
    );
    assert_eq!(
        align_of::<ffi::SharedWithMultipleKjRcs>(),
        ffi::c_alignof_shared_with_multiple_kj_rcs()
    );
}

#[test]
fn test_kj_rc_shared_struct_roundtrip() {
    let shared = ffi::c_return_shared_with_kj_rc(3030);
    assert_eq!(3030, ffi::c_take_shared_with_kj_rc_by_ref(&shared));
    assert_eq!(3030, ffi::c_take_shared_with_kj_rc_by_value(shared));

    let shared = ffi::c_return_shared_with_multiple_kj_rcs(1010, 2020);
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_rcs_by_ref(&shared)
    );
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_rcs_by_value(shared)
    );

    let shared = ffi::SharedWithKjRc {
        rc: ffi::c_return_kj_rc(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_kj_rc(shared);
    assert_eq!(2021, ffi::c_take_shared_with_kj_rc_by_ref(&shared));

    let shared = ffi::SharedWithMultipleKjRcs {
        first: ffi::c_return_kj_rc(1010),
        second: ffi::c_return_kj_rc(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_multiple_kj_rcs(shared);
    assert_eq!(
        3060,
        ffi::c_take_shared_with_multiple_kj_rcs_by_ref(&shared)
    );
}

#[test]
fn test_non_refcounted_kj_rc() {
    let mut rc = ffi::c_return_non_refcounted_kj_rc(4040);
    assert_eq!(4040, ffi::c_take_non_refcounted_kj_rc_by_ref(&rc));
    assert!(!rc.is_shared());
    assert!(rc.get_mut().is_some());

    let clone = rc.clone();
    assert!(rc.is_shared());
    assert!(rc.get_mut().is_none());
    assert_eq!(4040, ffi::c_take_non_refcounted_kj_rc_by_ref(&clone));

    drop(clone);
    assert!(!rc.is_shared());
    assert!(rc.get_mut().is_some());
}

#[test]
fn test_non_atomic_kj_arc() {
    let mut arc = ffi::c_return_non_atomic_kj_arc(5050);
    assert_eq!(5050, ffi::c_take_non_atomic_kj_arc_by_ref(&arc));
    assert!(!arc.is_shared());
    assert!(arc.get_mut().is_some());

    let clone = arc.clone();
    assert!(arc.is_shared());
    assert!(arc.get_mut().is_none());
    assert_eq!(5050, ffi::c_take_non_atomic_kj_arc_by_ref(&clone));

    drop(clone);
    assert!(!arc.is_shared());
    assert!(arc.get_mut().is_some());
}

#[test]
fn test_kj_arc_shared_struct_abi() {
    assert_eq!(
        size_of::<ffi::SharedWithKjArc>(),
        ffi::c_sizeof_shared_with_kj_arc()
    );
    assert_eq!(
        align_of::<ffi::SharedWithKjArc>(),
        ffi::c_alignof_shared_with_kj_arc()
    );
    assert_eq!(
        size_of::<ffi::SharedWithMultipleKjArcs>(),
        ffi::c_sizeof_shared_with_multiple_kj_arcs()
    );
    assert_eq!(
        align_of::<ffi::SharedWithMultipleKjArcs>(),
        ffi::c_alignof_shared_with_multiple_kj_arcs()
    );
}

#[test]
fn test_kj_arc_shared_struct_roundtrip() {
    let shared = ffi::c_return_shared_with_kj_arc(3030);
    assert_eq!(3030, ffi::c_take_shared_with_kj_arc_by_ref(&shared));
    assert_eq!(3030, ffi::c_take_shared_with_kj_arc_by_value(shared));

    let shared = ffi::c_return_shared_with_multiple_kj_arcs(1010, 2020);
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_arcs_by_ref(&shared)
    );
    assert_eq!(
        3030,
        ffi::c_take_shared_with_multiple_kj_arcs_by_value(shared)
    );

    let shared = ffi::SharedWithKjArc {
        arc: ffi::c_return_kj_arc(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_kj_arc(shared);
    assert_eq!(2021, ffi::c_take_shared_with_kj_arc_by_ref(&shared));

    let shared = ffi::SharedWithMultipleKjArcs {
        first: ffi::c_return_kj_arc(1010),
        second: ffi::c_return_kj_arc(2020),
    };
    let shared = ffi::c_roundtrip_shared_with_multiple_kj_arcs(shared);
    assert_eq!(
        3060,
        ffi::c_take_shared_with_multiple_kj_arcs_by_ref(&shared)
    );
}

#[test]
fn test_c_try_return() {
    assert_eq!((), ffi::c_try_return_void().unwrap());
    assert_eq!(2020, ffi::c_try_return_primitive().unwrap());
    assert_eq!(
        "std::exception: logic error",
        ffi::c_fail_return_primitive().unwrap_err().what(),
    );
    let err = ffi::c_fail_kj_exception_return_primitive().unwrap_err();
    assert_eq!("logic error", err.what());
    assert_eq!(cxx::KjExceptionType::Failed, err.r#type());

    // Test C++->Rust DISCONNECTED exception passing
    let err = ffi::c_fail_kj_exception_disconnected_return_primitive().unwrap_err();
    assert_eq!("connection lost", err.what());
    assert_eq!(cxx::KjExceptionType::Disconnected, err.r#type());

    // Test C++->Rust exception with details passing
    let err = ffi::c_fail_kj_exception_with_details_return_primitive().unwrap_err();
    assert_eq!("test exception with details", err.what());
    assert_eq!(cxx::KjExceptionType::Failed, err.r#type());
    assert_eq!(2020, ffi::c_try_return_box().unwrap().0);
    let value = String::from("2020");
    // SAFETY: The returned borrows do not outlive their live input values.
    unsafe {
        assert_eq!("2020", *ffi::c_try_return_ref(&value).unwrap());
        assert_eq!("2020", ffi::c_try_return_str("2020").unwrap());
        assert_eq!(b"2020", ffi::c_try_return_sliceu8(b"2020").unwrap());
    }
    assert_eq!("2020", ffi::c_try_return_rust_string().unwrap());
    assert_eq!("2020", &*ffi::c_try_return_unique_ptr_string().unwrap());
}

#[test]
fn test_c_take() {
    let unique_ptr = ffi::c_return_unique_ptr();
    let unique_ptr_ns = ffi2::c_return_ns_unique_ptr();

    check!(ffi::c_take_primitive(2020));
    check!(ffi::c_take_shared(ffi::Shared { z: 2020 }));
    check!(ffi::c_take_ns_shared(ffi::AShared { z: 2020 }));
    check!(ffi::ns_c_take_ns_shared(ffi::AShared { z: 2020 }));
    check!(ffi::c_take_nested_ns_shared(ffi::ABShared { z: 2020 }));
    check!(ffi::c_take_box(Box::new(cxx_test_suite::R(2020))));
    check!(ffi::c_take_ref_c(&unique_ptr));
    check!(ffi2::c_take_ref_ns_c(&unique_ptr_ns));
    check!(cxx_test_suite::module::ffi::c_take_unique_ptr(unique_ptr));
    check!(ffi::c_take_str("2020"));
    check!(ffi::c_take_slice_char(cast::unsigned_to_c_char(b"2020")));
    check!(ffi::c_take_slice_shared(&[
        ffi::Shared { z: 2020 },
        ffi::Shared { z: 2021 },
    ]));
    let shared_sort_slice = &mut [
        ffi::Shared { z: 2 },
        ffi::Shared { z: 0 },
        ffi::Shared { z: 7 },
        ffi::Shared { z: 4 },
    ];
    check!(ffi::c_take_slice_shared_sort(shared_sort_slice));
    assert_eq!(shared_sort_slice[0].z, 0);
    assert_eq!(shared_sort_slice[1].z, 2);
    assert_eq!(shared_sort_slice[2].z, 4);
    assert_eq!(shared_sort_slice[3].z, 7);
    let r_sort_slice = &mut [
        cxx_test_suite::R(2020),
        cxx_test_suite::R(2050),
        cxx_test_suite::R(2021),
    ];
    check!(ffi::c_take_slice_r(r_sort_slice));
    check!(ffi::c_take_slice_r_sort(r_sort_slice));
    assert_eq!(r_sort_slice[0].0, 2020);
    assert_eq!(r_sort_slice[1].0, 2021);
    assert_eq!(r_sort_slice[2].0, 2050);
    check!(ffi::c_take_rust_string("2020".to_owned()));
    check!(ffi::c_take_unique_ptr_string(
        ffi::c_return_unique_ptr_string()
    ));
    let mut vector = ffi::c_return_unique_ptr_vector_u8();
    assert_eq!(vector.pin_mut().pop(), Some(9));
    check!(ffi::c_take_unique_ptr_vector_u8(vector));
    let mut vector = ffi::c_return_unique_ptr_vector_f64();
    vector.pin_mut().push(9.0);
    check!(ffi::c_take_unique_ptr_vector_f64(vector));
    let mut vector = ffi::c_return_unique_ptr_vector_shared();
    vector.pin_mut().push(ffi::Shared { z: 9 });
    check!(ffi::c_take_unique_ptr_vector_shared(vector));
    check!(ffi::c_take_ref_vector(&ffi::c_return_unique_ptr_vector_u8()));
    let test_vec = [86_u8, 75_u8, 30_u8, 9_u8].to_vec();
    check!(ffi::c_take_rust_vec(test_vec.clone()));
    check!(ffi::c_take_rust_vec_index(test_vec.clone()));
    let shared_test_vec = vec![ffi::Shared { z: 1010 }, ffi::Shared { z: 1011 }];
    check!(ffi::c_take_rust_vec_shared(shared_test_vec.clone()));
    check!(ffi::c_take_rust_vec_shared_index(shared_test_vec.clone()));
    check!(ffi::c_take_rust_vec_shared_push(shared_test_vec.clone()));
    check!(ffi::c_take_rust_vec_shared_truncate(
        shared_test_vec.clone()
    ));
    check!(ffi::c_take_rust_vec_shared_clear(shared_test_vec.clone()));
    check!(ffi::c_take_rust_vec_shared_forward_iterator(
        shared_test_vec,
    ));
    let shared_sort_vec = vec![
        ffi::Shared { z: 2 },
        ffi::Shared { z: 0 },
        ffi::Shared { z: 7 },
        ffi::Shared { z: 4 },
    ];
    check!(ffi::c_take_rust_vec_shared_sort(shared_sort_vec));
    check!(ffi::c_take_ref_rust_vec(&test_vec));
    check!(ffi::c_take_ref_rust_vec_index(&test_vec));
    check!(ffi::c_take_ref_rust_vec_copy(&test_vec));
    // SAFETY: The temporary remains alive for the duration of the call.
    check!(unsafe {
        ffi::c_take_ref_shared_string(&ffi::SharedString {
            msg: "2020".to_owned(),
        })
    });
    let ns_shared_test_vec = vec![ffi::AShared { z: 1010 }, ffi::AShared { z: 1011 }];
    check!(ffi::c_take_rust_vec_ns_shared(ns_shared_test_vec));
    let nested_ns_shared_test_vec = vec![ffi::ABShared { z: 1010 }, ffi::ABShared { z: 1011 }];
    check!(ffi::c_take_rust_vec_nested_ns_shared(
        nested_ns_shared_test_vec
    ));

    check!(ffi::c_take_enum(ffi::Enum::AVal));
    check!(ffi::c_take_ns_enum(ffi::AEnum::AAVal));
    check!(ffi::c_take_nested_ns_enum(ffi::ABEnum::ABAVal));
}

#[test]
fn test_c_callback() {
    fn callback(s: String) -> usize {
        if s == "2020" {
            cxx_test_suite_set_correct();
        }
        0
    }

    fn callback_ref(s: &String) {
        let _capacity = s.capacity();
        if s == "2020" {
            cxx_test_suite_set_correct();
        }
    }

    fn callback_mut(s: &mut String) {
        if s == "2020" {
            cxx_test_suite_set_correct();
        }
    }

    check!(ffi::c_take_callback(callback));
    check!(ffi::c_take_callback_ref(callback_ref));
    check!(ffi::c_take_callback_ref_lifetime(callback_ref));
    check!(ffi::c_take_callback_mut(callback_mut));
}

#[test]
fn test_c_call_r() {
    fn cxx_run_test() {
        unsafe extern "C" {
            fn cxx_run_test() -> *const i8;
        }
        // SAFETY: The test entry point has the declaration above and returns a null or C string.
        let failure = unsafe { cxx_run_test() };
        if !failure.is_null() {
            // SAFETY: A non-null failure result points to a NUL-terminated error message.
            let msg = unsafe { CStr::from_ptr(failure as *mut std::os::raw::c_char) };
            eprintln!("{}", msg.to_string_lossy());
        }
    }
    check!(cxx_run_test());
}

#[test]
fn test_c_method_calls() {
    let mut unique_ptr = ffi::c_return_unique_ptr();

    let old_value = unique_ptr.get();
    assert_eq!(2020, old_value);
    assert_eq!(2021, unique_ptr.pin_mut().set(2021));
    assert_eq!(2021, unique_ptr.get());
    assert_eq!(2021, unique_ptr.get2());
    // SAFETY: `unique_ptr` remains live and non-null throughout these calls.
    unsafe {
        assert_eq!(2021, *unique_ptr.getRef());
        assert_eq!(2021, (&*unique_ptr.as_mut_ptr()).get());
        assert_eq!(2021, (&*unique_ptr.as_ptr()).get());
        assert_eq!(2021, *unique_ptr.pin_mut().getMut());
    }
    assert_eq!(2022, unique_ptr.pin_mut().set_succeed(2022).unwrap());
    assert!(unique_ptr.pin_mut().get_fail().is_err());
    assert_eq!(2021, ffi::Shared { z: 0 }.c_method_on_shared());
    let shared_ref = ffi::Shared { z: 2022 };
    let mut shared = ffi::Shared { z: 2023 };
    // SAFETY: Both returned references are bounded by live `Shared` values.
    unsafe {
        assert_eq!(2022, *shared_ref.c_method_ref_on_shared());
        assert_eq!(2023, *shared.c_method_mut_on_shared());
    }

    let val = 42;
    let mut array = ffi::Array {
        a: [0, 0, 0, 0],
        b: ffi::Buffer::default(),
    };
    array.c_set_array(val);
    assert_eq!(array.a.len() as i32 * val, array.r_get_array_sum());
}

#[test]
fn test_shared_ptr_weak_ptr() {
    let shared_ptr = ffi::c_return_shared_ptr();
    let weak_ptr = SharedPtr::downgrade(&shared_ptr);
    assert_eq!(1, ffi::c_get_use_count(&weak_ptr));

    assert!(!weak_ptr.upgrade().is_null());
    assert_eq!(1, ffi::c_get_use_count(&weak_ptr));

    drop(shared_ptr);
    assert_eq!(0, ffi::c_get_use_count(&weak_ptr));
    assert!(weak_ptr.upgrade().is_null());
}

#[test]
fn test_c_ns_method_calls() {
    let unique_ptr = ffi2::ns_c_return_unique_ptr_ns();

    let old_value = unique_ptr.get();
    assert_eq!(1000, old_value);
}

#[test]
fn test_enum_representations() {
    assert_eq!(0, ffi::Enum::AVal.repr);
    assert_eq!(2020, ffi::Enum::BVal.repr);
    assert_eq!(2021, ffi::Enum::LastVal.repr);
}

#[test]
fn test_debug() {
    assert_eq!("Shared { z: 1 }", format!("{:?}", ffi::Shared { z: 1 }));
    assert_eq!("BVal", format!("{:?}", ffi::Enum::BVal));
    assert_eq!("Enum(9)", format!("{:?}", ffi::Enum { repr: 9 }));
}

#[unsafe(no_mangle)]
extern "C" fn cxx_test_suite_get_box() -> *mut cxx_test_suite::R {
    Box::into_raw(Box::new(cxx_test_suite::R(2020usize)))
}

#[unsafe(no_mangle)]
unsafe extern "C" fn cxx_test_suite_r_is_correct(r: *const cxx_test_suite::R) -> bool {
    // Safety: the C++ test passes a valid pointer to a live R.
    unsafe { (*r).0 == 2020 }
}

#[test]
fn test_rust_name_attribute() {
    assert_eq!("2020", ffi::i32_overloaded_function(2020));
    assert_eq!("2020", ffi::str_overloaded_function("2020"));
    let unique_ptr = ffi::c_return_unique_ptr();
    assert_eq!("2020", unique_ptr.i32_overloaded_method(2020));
    assert_eq!("2020", unique_ptr.str_overloaded_method("2020"));
}

#[test]
fn test_extern_trivial() {
    let mut d = ffi2::c_return_trivial();
    check!(ffi2::c_take_trivial_ref(&d));
    check!(d.c_take_trivial_ref_method());
    check!(d.c_take_trivial_mut_ref_method());
    check!(ffi2::c_take_trivial(d));
    let mut d = ffi2::c_return_trivial_ptr();
    check!(d.c_take_trivial_ref_method());
    check!(d.c_take_trivial_mut_ref_method());
    check!(ffi2::c_take_trivial_ptr(d));
    cxx::UniquePtr::new(ffi2::D { d: 42 });
    let d = ffi2::ns_c_return_trivial();
    check!(ffi2::ns_c_take_trivial(d));

    let g = ffi2::c_return_trivial_ns();
    check!(ffi2::c_take_trivial_ns_ref(&g));
    check!(ffi2::c_take_trivial_ns(g));
    let g = ffi2::c_return_trivial_ns_ptr();
    check!(ffi2::c_take_trivial_ns_ptr(g));
    cxx::UniquePtr::new(ffi2::G { g: 42 });
}

#[test]
fn test_extern_opaque() {
    let mut e = ffi2::c_return_opaque_ptr();
    check!(ffi2::c_take_opaque_ref(e.as_ref().unwrap()));
    check!(e.c_take_opaque_ref_method());
    check!(e.pin_mut().c_take_opaque_mut_ref_method());
    check!(ffi2::c_take_opaque_ptr(e));

    let f = ffi2::c_return_ns_opaque_ptr();
    check!(ffi2::c_take_opaque_ns_ref(f.as_ref().unwrap()));
    check!(ffi2::c_take_opaque_ns_ptr(f));
}

#[test]
fn test_raw_ptr() {
    let c = ffi::c_return_mut_ptr(2023);
    // SAFETY: `c` is a fresh allocation whose ownership is transferred to the unique pointer.
    let mut c_unique = unsafe { cxx::UniquePtr::from_raw(c) };
    assert_eq!(2023, c_unique.pin_mut().set_succeed(2023).unwrap());
    // c will be dropped as it's now in a UniquePtr

    let c2 = ffi::c_return_mut_ptr(2024);
    // SAFETY: `c2` remains allocated until the final call consumes it.
    unsafe {
        assert_eq!(2024, ffi::c_take_const_ptr(c2));
        assert_eq!(2024, ffi::c_take_mut_ptr(c2)); // deletes c2
    }

    let c3 = ffi::c_return_const_ptr(2025);
    // SAFETY: `c3` remains allocated until the final call consumes it.
    unsafe {
        assert_eq!(2025, ffi::c_take_const_ptr(c3));
        assert_eq!(2025, ffi::c_take_mut_ptr(c3 as *mut ffi::C)); // deletes c3
    }
}

#[test]
fn test_unwind_safe() {
    fn inspect(_c: &ffi::C) {}
    fn require_unwind_safe<T: UnwindSafe>() {}
    fn require_ref_unwind_safe<T: RefUnwindSafe>() {}
    fn require_unwind_safe_value<T: UnwindSafe>(_: T) {}
    fn require_ref_unwind_safe_value<T: RefUnwindSafe>(_: T) {}

    require_unwind_safe_value(|c: UniquePtr<ffi::C>| panic::catch_unwind(|| drop(c)));
    require_ref_unwind_safe_value(|c: &ffi::C| panic::catch_unwind(|| inspect(c)));
    require_unwind_safe::<ffi::C>();
    require_ref_unwind_safe::<ffi::C>();
}

/// Runs `f`, which is expected to panic, and returns the panic message.
fn expect_panic_message<F: FnOnce() + UnwindSafe>(f: F) -> String {
    let payload = panic::catch_unwind(f).expect_err("expected a panic");
    if let Some(message) = payload.downcast_ref::<String>() {
        message.clone()
    } else if let Some(message) = payload.downcast_ref::<&str>() {
        (*message).to_owned()
    } else {
        panic!("panic payload was not a message");
    }
}

// An `extern "C++"` function which is not declared to return a Result has no way of
// reporting an exception to its caller. The exception must become a panic; letting it
// escape the extern "C" boundary would terminate the process.
#[test]
fn test_infallible_cxx_function_throws() {
    let message = expect_panic_message(ffi::c_infallible_fail_void);
    assert!(message.contains("infallible void failure"), "{message}");

    let message = expect_panic_message(|| {
        let _ = ffi::c_infallible_fail_primitive();
    });
    assert!(
        message.contains("std::exception: infallible logic error"),
        "{message}"
    );

    // The kj::Exception type is preserved in the panic message.
    let message = expect_panic_message(|| {
        let _ = ffi::c_infallible_fail_kj_exception_disconnected();
    });
    assert!(message.contains("Disconnected"), "{message}");
    assert!(message.contains("infallible disconnect"), "{message}");

    // Return type which travels through an out parameter.
    let message = expect_panic_message(|| {
        let _ = ffi::c_infallible_fail_rust_string();
    });
    assert!(message.contains("infallible string failure"), "{message}");

    // Neither a kj::Exception nor a std::exception, like jsg::JsExceptionThrown.
    let message = expect_panic_message(|| {
        let _ = ffi::c_infallible_fail_foreign_exception();
    });
    assert!(message.contains("unknown non-KJ exception"), "{message}");

    // Member function with a receiver.
    let message = expect_panic_message(|| {
        let mut unique_ptr = ffi::c_return_unique_ptr();
        let _ = unique_ptr.pin_mut().get_fail_infallible();
    });
    assert!(message.contains("infallible method failure"), "{message}");
}

#[test]
fn test_infallible_cxx_function_cancellation() {
    let payload = panic::catch_unwind(|| {
        let _ = ffi::c_infallible_cancel();
    })
    .expect_err("expected a panic");
    assert!(
        payload.downcast_ref::<cxx::CanceledException>().is_some(),
        "expected panic payload to be CanceledException"
    );
}

// C++ -> Rust -> C++ -> Rust, without a Result anywhere along the way. The exception
// becomes a panic in Rust, a kj::Exception again when it reenters C++, and finally a
// panic once more.
#[test]
fn test_infallible_exception_roundtrip() {
    let message = expect_panic_message(|| {
        let _ = ffi::c_infallible_fail_roundtrip();
    });
    assert!(message.contains("infallible logic error"), "{message}");
}

#[test]
fn test_rust_to_cpp_cancellation() {
    // Test Rust->C++ cancellation: Rust calls C++ function that throws CanceledException
    let result = panic::catch_unwind(|| {
        let _ = ffi::c_cancel_return_primitive();
    });

    // Should panic with CanceledException
    assert!(
        result.is_err(),
        "Expected CanceledException panic from C++ function"
    );

    // Verify it's specifically a CanceledException
    if let Err(panic_payload) = result {
        assert!(
            panic_payload
                .downcast_ref::<cxx::CanceledException>()
                .is_some(),
            "Expected panic payload to be CanceledException"
        );
    }
}

#[test]
fn test_kj_exception_with_details() {
    // Test C++->Rust KjException with details
    let err = ffi::c_fail_kj_exception_with_details_return_primitive().unwrap_err();
    assert_eq!("test exception with details", err.what());
    assert_eq!(cxx::KjExceptionType::Failed, err.r#type());

    // Check details
    let details = err.details();
    assert!(details.is_some(), "Expected details to be present");
    let details = details.unwrap();
    assert_eq!(2, details.len(), "Expected 2 details");

    // Check first detail
    assert_eq!(42, details[0].0);
    assert_eq!(b"test detail 1", details[0].1.as_slice());

    // Check second detail
    assert_eq!(999, details[1].0);
    assert_eq!(b"another detail", details[1].1.as_slice());
}

#[test]
fn test_rust_to_cpp_to_rust_cancellation() {
    // Test Rust->C++->Rust cancellation roundtrip
    let result = panic::catch_unwind(|| {
        let _ = ffi::c_cancel_roundtrip_return_primitive();
    });

    // Should panic with CanceledException
    assert!(
        result.is_err(),
        "Expected CanceledException panic from roundtrip"
    );

    // Verify it's specifically a CanceledException
    if let Err(panic_payload) = result {
        assert!(
            panic_payload
                .downcast_ref::<cxx::CanceledException>()
                .is_some(),
            "Expected panic payload to be CanceledException"
        );
    }
}
