use kj_rs::KjArc;
use kj_rs::KjMaybe;
use kj_rs::KjRc;

use crate::ffi;

pub fn modify_own_ret_rc(
    mut rc: KjRc<ffi::OpaqueRefcountedClass>,
) -> KjRc<ffi::OpaqueRefcountedClass> {
    let Some(mut_ref) = rc.get_mut() else {
        panic!("newly-owned KjRc must be unique");
    };
    mut_ref.set_data(467);
    rc
}

pub fn take_maybe_rc_ret(
    maybe: KjMaybe<KjRc<ffi::OpaqueRefcountedClass>>,
) -> KjMaybe<KjRc<ffi::OpaqueRefcountedClass>> {
    let option: Option<KjRc<ffi::OpaqueRefcountedClass>> = maybe.into();
    option
        .map(|mut rc| {
            let Some(mut_ref) = rc.get_mut() else {
                panic!("newly-owned KjRc must be unique");
            };
            mut_ref.set_data(467);
            rc
        })
        .into()
}

pub fn modify_own_ret_arc(
    mut arc: KjArc<ffi::OpaqueAtomicRefcountedClass>,
) -> KjArc<ffi::OpaqueAtomicRefcountedClass> {
    let Some(mut_ref) = arc.get_mut() else {
        panic!("newly-owned KjArc must be unique");
    };
    mut_ref.set_data(328);
    arc
}

#[cfg(test)]
pub mod tests {
    use crate::ffi;

    #[test]
    fn test_rc() {
        let rc = ffi::get_rc();
        assert_eq!(rc.get_data(), 15);
        let rc_clone = rc.clone();
        assert_eq!(rc_clone.get_data(), 15);

        assert!(rc.is_shared());
        std::mem::drop(rc_clone);
        assert!(!rc.is_shared());
    }

    #[test]
    fn test_arc() {
        let arc = ffi::get_arc();
        assert_eq!(arc.get_data(), 16);
        let arc_clone = arc.clone();
        assert_eq!(arc_clone.get_data(), 16);

        assert!(arc.is_shared());
        std::mem::drop(arc_clone);
        assert!(!arc.is_shared());
    }

    #[test]
    fn test_pass_cxx() {
        let rc = ffi::get_rc();
        let arc = ffi::get_arc();
        ffi::give_rc_back(rc);
        ffi::give_arc_back(arc);
    }

    #[test]
    fn test_maybe_rc_some() {
        let maybe = ffi::return_maybe_rc_some();
        assert!(maybe.is_some());
        assert!(!maybe.is_none());

        let opt: Option<kj_rs::KjRc<ffi::OpaqueRefcountedClass>> = maybe.into();
        let rc = opt.unwrap();
        assert_eq!(rc.get_data(), 111);
    }

    #[test]
    fn test_maybe_rc_none() {
        let maybe = ffi::return_maybe_rc_none();
        assert!(maybe.is_none());
        assert!(!maybe.is_some());

        let opt: Option<kj_rs::KjRc<ffi::OpaqueRefcountedClass>> = maybe.into();
        assert!(opt.is_none());
    }

    #[test]
    fn test_maybe_arc_some() {
        let maybe = ffi::return_maybe_arc_some();
        assert!(maybe.is_some());

        let opt: Option<kj_rs::KjArc<ffi::OpaqueAtomicRefcountedClass>> = maybe.into();
        assert_eq!(opt.unwrap().get_data(), 222);
    }

    #[test]
    fn test_maybe_arc_none() {
        let maybe = ffi::return_maybe_arc_none();
        assert!(maybe.is_none());

        let opt: Option<kj_rs::KjArc<ffi::OpaqueAtomicRefcountedClass>> = maybe.into();
        assert!(opt.is_none());
    }

    #[test]
    fn test_maybe_rc_pass_cxx() {
        // Construct a `Maybe<Rc>` in Rust and hand it to C++.
        let mut rc = ffi::get_rc();
        // `take_maybe_rc` asserts the pointee data is 111.
        rc.get_mut().unwrap().set_data(111);
        let maybe = kj_rs::KjMaybe::Some(rc);
        ffi::take_maybe_rc(maybe);
    }

    #[test]
    fn test_maybe_rc_none_pass_cxx() {
        let none: kj_rs::KjMaybe<kj_rs::KjRc<ffi::OpaqueRefcountedClass>> = kj_rs::KjMaybe::None;
        assert!(none.is_none());
        // Dropping a None must not touch the uninitialized Rc storage.
        std::mem::drop(none);
    }

    #[test]
    fn test_maybe_rc_rust_return_driver() {
        ffi::maybe_rc_rust_driver();
    }

    #[test]
    fn test_arc_thread() {
        let mut arc = ffi::get_arc();

        std::thread::scope(|s| {
            let mut c = arc.clone();
            assert!(c.get_mut().is_none());

            s.spawn(|| {
                assert_eq!(arc.clone().get_data(), 16);
            });

            s.spawn(|| {
                let a = arc.clone();
                assert!(a.is_shared());
            });
        });

        {
            let mut cloned = arc.clone();
            assert!(cloned.get_mut().is_none());
        }

        let mut_ref = arc.get_mut();
        assert!(mut_ref.is_some());
        mut_ref.unwrap().set_data(35643);
        assert_eq!(arc.get_data(), 35643);
    }
}
