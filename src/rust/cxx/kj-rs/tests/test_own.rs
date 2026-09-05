use crate::ffi;

// Send and Sync are explicity opt-in when using cxx.
// # Safety
// The type `OqaqueCxxClass` contains no raw pointers or interior mutability.
// It is a wrapper around an integer, and thus safe to send between threads.
// Safety: the test type follows the thread-safety contract of its C++ implementation.
unsafe impl Send for ffi::OpaqueCxxClass {}
// # Safety
// `OpaqueCxxClass` cannot be unsafely mutated from a shared reference.
// Safety: the test type follows the thread-safety contract of its C++ implementation.
unsafe impl Sync for ffi::OpaqueCxxClass {}

#[cfg(test)]
pub mod tests {
    use std::collections::HashMap;

    use crate::ffi;

    #[test]
    fn kj_own() {
        let mut own = ffi::cxx_kj_own();
        // Methods on C++ classes can be called from Rust
        assert_eq!(own.get_data(), 42);
        own.pin_mut().set_data(14);
        assert_eq!(own.get_data(), 14);
        // Explicitly drop for clarity / debugging drop impl
        std::mem::drop(own);
    }

    #[test]
    fn kj_move() {
        let mut owned = ffi::cxx_kj_own();
        owned.pin_mut().set_data(27);
        // Move owned into moved_value
        let moved_value = owned;
        let moved_value2 = moved_value;
        let mut moved_value3 = moved_value2;
        assert_eq!(moved_value3.get_data(), 27);
        moved_value3.pin_mut().set_data(14);
        assert_eq!(moved_value3.get_data(), 14);
    }

    #[test]
    fn rust_take_own() {
        ffi::rust_take_own_driver();
    }

    #[test]
    fn test_pass_own_back() {
        let mut own = ffi::cxx_kj_own();
        own.pin_mut().set_data(14);
        ffi::give_own_back(own);
    }

    #[test]
    fn kj_get_test() {
        let mut own = ffi::cxx_kj_own();
        let data_ptr = own.as_ptr();
        own.pin_mut().set_data(75193);
        // SAFETY: `data_ptr` remains valid while `own` is alive.
        unsafe {
            // This one does need to be unwrapped because it is created from a pointer
            assert_eq!(data_ptr.as_ref().unwrap().get_data(), 75193);
        }
    }

    #[test]
    fn test_breaking_things() {
        let _ = ffi::breaking_things();
    }

    #[test]
    fn modify_own_return_test_rust() {
        ffi::modify_own_return_test();
    }

    #[test]
    fn test_primitive() {
        let own = ffi::own_integer();
        assert_eq!(*own.as_ref(), -67582);
    }

    #[test]
    fn test_attached_primitive() {
        let own = ffi::own_integer_attached();
        assert_eq!(*own.as_ref(), -67582);
        // The own here additionally owns an [`OpaqueCxxClass`]
    }

    #[test]
    fn test_result_returning_own_ok() {
        let own = ffi::cxx_try_return_own().unwrap();
        assert_eq!(own.get_data(), 5150);
    }

    #[test]
    fn test_result_returning_own_err() {
        let err = ffi::cxx_fail_return_own().unwrap_err();
        assert_eq!(err.what(), "std::exception: cxx_fail_return_own");
    }

    #[test]
    fn heap_alloc() {
        let mut nodes = vec![];
        for i in 0..1456 {
            nodes.push(Box::new(i));
        }
        assert_eq!(nodes.len(), 1456);
    }

    #[test]
    fn test_own_as_ref_as_mut() {
        let mut own = ffi::cxx_kj_own();

        assert_eq!(own.as_ref().get_data(), 42);

        own.as_mut().set_data(99);
        assert_eq!(own.get_data(), 99);
    }

    #[test]
    #[should_panic]
    fn test_null() {
        let _null_own = ffi::null_kj_own();
    }

    #[test]
    fn test_own_as_ptr() {
        let own = ffi::cxx_kj_own();
        let ptr = own.as_ptr();
        assert!(!ptr.is_null());
    }

    #[test]
    fn null_exception_test() {
        assert!(ffi::null_exception_test_driver_1().contains("Cannot pass a null Own to Rust"));
        assert!(ffi::null_exception_test_driver_2().contains("panic in tests::ffi::get_null"));
    }

    #[test]
    fn test_own_in_collections() {
        let mut owns_vec = Vec::new();
        let mut owns_map = HashMap::new();

        // Test in Vec
        for i in 0..10 {
            let mut own = ffi::cxx_kj_own();
            own.pin_mut().set_data(i * 10);
            owns_vec.push(own);
        }

        // Test in HashMap
        for i in 0..10 {
            let mut own = ffi::cxx_kj_own();
            own.pin_mut().set_data(i * 100);
            owns_map.insert(format!("key_{i}"), own);
        }

        // Verify all values
        for (i, own) in owns_vec.iter().enumerate() {
            assert_eq!(own.get_data(), (i * 10) as u64);
        }

        for i in 0u64..10 {
            let key = format!("key_{i}");
            assert_eq!(owns_map[&key].get_data(), i * 100);
        }
    }

    #[test]
    fn test_own_rapid_create_destroy() {
        for _ in 0..1000 {
            let mut own = ffi::cxx_kj_own();
            own.pin_mut().set_data(12345);
            assert_eq!(own.get_data(), 12345);
            // Implicit drop at end of loop
        }
    }

    #[test]
    fn test_own_nested_operations() {
        let mut own1 = ffi::cxx_kj_own();
        let mut own2 = ffi::cxx_kj_own();

        own1.pin_mut().set_data(100);
        own2.pin_mut().set_data(200);

        // Use one own's value to modify another
        let val1 = own1.get_data();
        own1.pin_mut().set_data(val1 + own2.get_data());

        assert_eq!(own1.get_data(), 300);
        assert_eq!(own2.get_data(), 200);
    }

    #[test]
    fn test_own_debug_display() {
        let own = ffi::cxx_kj_own();
        let debug_str = format!("{own:?}");
        assert!(!debug_str.is_empty());
    }

    // NOTE: `KjOwn<T>` is deliberately neither `Send` nor `Sync`, even for `T: Send + Sync`
    // (like `OpaqueCxxClass` here): the `KjOwn` carries a type-erased C++ disposer which may
    // not be thread-safe (kj::Rc, arena allocations, ...). The negative is asserted at compile
    // time in lib.rs (assert_not_impl_any).

    #[test]
    fn test_own_concurrent_creation() {
        use std::sync::Arc;
        use std::sync::Barrier;
        use std::thread;

        let num_threads = 10;
        let barrier = Arc::new(Barrier::new(num_threads));
        let mut handles = Vec::new();

        for thread_id in 0..num_threads {
            let barrier_clone = Arc::clone(&barrier);
            let handle = thread::spawn(move || {
                // Wait for all threads to be ready
                barrier_clone.wait();

                // Create and use Own concurrently
                let mut own = ffi::cxx_kj_own();
                own.pin_mut().set_data(thread_id as u64 * 100);
                assert_eq!(own.get_data(), thread_id as u64 * 100);

                // Return the final value for verification
                own.get_data()
            });
            handles.push(handle);
        }

        // Collect results
        let mut results = Vec::new();
        for handle in handles {
            results.push(handle.join().unwrap());
        }

        // Verify all threads completed successfully
        results.sort_unstable();
        let expected: Vec<u64> = (0..num_threads).map(|i| i as u64 * 100).collect();
        assert_eq!(results, expected);
    }
}
