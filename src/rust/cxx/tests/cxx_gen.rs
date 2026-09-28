use std::str;

use cxx_gen::generate_header_and_cc;

const BRIDGE0: &str = r#"
    #[cxx::bridge]
    mod ffi {
        unsafe extern "C++" {
            pub fn do_cpp_thing(foo: &str);
        }
    }
"#;

const BRIDGE1: &str = r"
    #[cxx::bridge]
    mod ffi {
        #[derive(JsgStruct)]
        struct MyStruct {
            field1: i32,
            field2: String,
        }
    }
";

const BRIDGE2: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            own: KjOwn<Thing>,
        }

        struct MultiHolder {
            first: KjOwn<Thing>,
            second: KjOwn<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

const BRIDGE3: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            rc: KjRc<Thing>,
        }

        struct MultiHolder {
            first: KjRc<Thing>,
            second: KjRc<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

const BRIDGE4: &str = r#"
    #[cxx::bridge]
    mod ffi {
        struct Holder {
            arc: KjArc<Thing>,
        }

        struct MultiHolder {
            first: KjArc<Thing>,
            second: KjArc<Thing>,
        }

        unsafe extern "C++" {
            type Thing;
        }

        extern "Rust" {
            fn pass_holder(holder: Holder) -> Holder;
        }
    }
"#;

#[test]
fn test_extern_c_function() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE0.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.implementation).unwrap();
    // To avoid continual breakage we won't test every byte.
    // Let's look for the major features.
    assert!(
        output.contains("::rust::repr::Result cxxbridge1$do_cpp_thing(::rust::Str foo) noexcept")
    );
    // The shim must catch every exception: letting one escape the extern "C" boundary
    // would terminate the process.
    assert!(output.contains("return ::rust::repr::Result::run("));
}

#[test]
fn test_impl_annotation() {
    let opt = cxx_gen::Opt {
        cxx_impl_annotations: Some("ANNOTATION".to_owned()),
        ..Default::default()
    };
    let source = BRIDGE0.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.implementation).unwrap();
    assert!(
        output.contains("ANNOTATION ::rust::repr::Result cxxbridge1$do_cpp_thing(::rust::Str foo)")
    );
}

#[test]
fn test_jsg_struct_derive() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE1.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let output = str::from_utf8(&generated.header).unwrap();
    assert!(output.contains("JSG_STRUCT(field1, field2);"));
    assert!(output.contains("jsg.h"));
}

#[test]
fn test_kj_own_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE2.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Own<::Thing> own;"));
    assert!(header.contains("::kj::Own<::Thing> first;"));
    assert!(header.contains("::kj::Own<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}

#[test]
fn test_kj_rc_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE3.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Rc<::Thing> rc;"));
    assert!(header.contains("::kj::Rc<::Thing> first;"));
    assert!(header.contains("::kj::Rc<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}

#[test]
fn test_kj_arc_in_shared_struct() {
    let opt = cxx_gen::Opt::default();
    let source = BRIDGE4.parse().unwrap();
    let generated = generate_header_and_cc(source, &opt).unwrap();
    let header = str::from_utf8(&generated.header).unwrap();
    let implementation = str::from_utf8(&generated.implementation).unwrap();
    assert!(header.contains("::kj::Arc<::Thing> arc;"));
    assert!(header.contains("::kj::Arc<::Thing> first;"));
    assert!(header.contains("::kj::Arc<::Thing> second;"));
    assert!(header.contains("kj-rs/kj-rs.h"));
    assert!(
        implementation.contains("static_assert(sizeof(::kj::Arc<::Thing>) == 2 * sizeof(void *)")
    );
    assert!(!implementation.contains("is_base_of<::kj::AtomicRefcounted"));
    assert!(!implementation.contains("cxxbridge1$kj_rs$arc$"));
    let expected = "::rust::ManuallyDrop<::Holder> holder$(::std::move(holder));";
    assert!(implementation.contains(expected));
}
