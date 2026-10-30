// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;

#[test]
fn states_and_mapping() {
    struct NoDefault;

    let mut value = NullOrUndefined::Some(String::from("x"));
    assert!(value.is_some());
    assert!(!value.is_null());
    assert!(!value.is_undefined());
    assert_eq!(
        value.as_ref().map(String::as_str),
        NullOrUndefined::Some("x")
    );
    if let NullOrUndefined::Some(s) = value.as_mut() {
        s.push('y');
    }
    assert_eq!(value.map(|s| s.len()), NullOrUndefined::Some(2));

    for absent in [NullOrUndefined::<String>::Null, NullOrUndefined::Undefined] {
        assert!(!absent.is_some());
        assert_ne!(absent.is_null(), absent.is_undefined());
        assert_eq!(absent.as_ref().map(|_| panic!("absent")), absent);
        assert_eq!(absent.clone().map(|_| panic!("absent")), absent);
    }
    // No T: Default bound is needed.
    assert!(NullOrUndefined::<NoDefault>::default().is_undefined());
}

#[test]
fn optional_nullable_conversions_preserve_all_states() {
    for value in [None, Some(Nullable::Null), Some(Nullable::Some("x"))] {
        let converted = NullOrUndefined::from(value.clone());
        assert_eq!(Option::<Nullable<_>>::from(converted), value);
    }
    assert_eq!(
        NullOrUndefined::from(Nullable::Some(1)),
        NullOrUndefined::Some(1)
    );
    assert_eq!(
        NullOrUndefined::<i32>::from(Nullable::Null),
        NullOrUndefined::Null
    );
}

#[test]
fn lenient_conversions_preserve_convertible_states() {
    for value in [
        NullOrUndefined::Some(1),
        NullOrUndefined::Null,
        NullOrUndefined::Undefined,
    ] {
        let lenient = crate::Lenient::from(value);
        assert_eq!(NullOrUndefined::from(lenient), value);
        let (parts, original) = crate::Lenient::from(value).to_parts();
        assert_eq!(parts, value);
        assert!(original.is_none());
    }
}

#[test]
fn tracing_visits_only_some() {
    use std::cell::Cell;

    use crate::Traced;
    use crate::v8;

    struct Edge<'a>(&'a Cell<usize>);
    impl Traced for Edge<'_> {
        fn trace(&self, _visitor: &mut v8::GcVisitor) {
            self.0.set(self.0.get() + 1);
        }
    }
    let visits = Cell::new(0);
    let mut visitor = v8::GcVisitor {
        handle: v8::ffi::GcVisitor { ptr: 0 },
    };
    NullOrUndefined::Some(Edge(&visits)).trace(&mut visitor);
    NullOrUndefined::<Edge<'_>>::Null.trace(&mut visitor);
    NullOrUndefined::<Edge<'_>>::Undefined.trace(&mut visitor);
    assert_eq!(visits.get(), 1);
}
