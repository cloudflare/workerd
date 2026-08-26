#![deny(warnings)] // Check that expansion of `cxx::bridge` doesn't trigger warnings.

#[cxx::bridge(namespace = "tests")]
pub mod ffi {
    struct Job {
        raw: u32,
    }

    unsafe extern "C++" {
        include!("tests/ffi/tests.h");

        type C = crate::ffi::C;

        fn c_take_unique_ptr(c: UniquePtr<C>);
    }

    impl Vec<Job> {}
}

#[cxx::bridge(namespace = "tests")]
pub mod ffi2 {
    extern "Rust" {
        type R = crate::R;
    }

    unsafe extern "C++" {
        include!("tests/ffi/tests.h");

        type D = crate::other::D;
        type E = crate::other::E;
        #[namespace = "F"]
        type F = crate::other::f::F;
        #[namespace = "G"]
        type G = crate::other::G;

        #[namespace = "H"]
        type H;

        fn c_take_trivial_ptr(d: UniquePtr<D>);
        fn c_take_trivial_ref(d: &D);
        fn c_take_trivial_mut_ref(d: &mut D);
        fn c_take_trivial_pin_ref(d: Pin<&D>);
        fn c_take_trivial_pin_mut_ref(d: Pin<&mut D>);
        fn c_take_trivial_ref_method(self: &D);
        fn c_take_trivial_mut_ref_method(self: &mut D);
        fn c_take_trivial(d: D);
        fn c_take_trivial_ns_ptr(g: UniquePtr<G>);
        fn c_take_trivial_ns_ref(g: &G);
        fn c_take_trivial_ns(g: G);
        fn c_take_opaque_ptr(e: UniquePtr<E>);
        fn c_take_opaque_ref(e: &E);
        fn c_take_opaque_ref_method(self: &E);
        fn c_take_opaque_mut_ref_method(self: Pin<&mut E>);
        fn c_take_opaque_ns_ptr(e: UniquePtr<F>);
        fn c_take_opaque_ns_ref(e: &F);
        fn c_return_trivial_ptr() -> UniquePtr<D>;
        fn c_return_trivial() -> D;
        fn c_return_trivial_ns_ptr() -> UniquePtr<G>;
        fn c_return_trivial_ns() -> G;
        fn c_return_opaque_ptr() -> UniquePtr<E>;
        fn c_return_opaque_mut_pin<'a>(e: Pin<&'a mut E>) -> Pin<&'a mut E>;
        fn c_return_ns_opaque_ptr() -> UniquePtr<F>;
        fn c_return_ns_unique_ptr() -> UniquePtr<H>;
        fn c_take_ref_ns_c(h: &H);

        #[namespace = "other"]
        fn ns_c_take_trivial(d: D);
        #[namespace = "other"]
        fn ns_c_return_trivial() -> D;

        #[namespace = "I"]
        type I;

        fn get(self: &I) -> u32;

        #[namespace = "I"]
        fn ns_c_return_unique_ptr_ns() -> UniquePtr<I>;

        fn c_return_box_from_aliased_rust_type() -> Box<R>;
    }

    impl UniquePtr<D> {}
    impl UniquePtr<E> {}
    impl UniquePtr<F> {}
    impl UniquePtr<G> {}
}
