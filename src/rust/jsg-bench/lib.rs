// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use std::cell::Cell;
use std::hint::black_box;

use jsg::FromJS;
use jsg::ToJS;
use jsg::v8;
use jsg_macros::jsg_constructor;
use jsg_macros::jsg_method;
use jsg_macros::jsg_oneof;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_struct;
use kj_rs::KjMaybe;

#[cxx::bridge(namespace = "workerd::rust::jsg_bench")]
mod ffi {
    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");
        include!("workerd/rust/jsg/v8.rs.h");
        type Isolate = jsg::v8::ffi::Isolate;
        type MaybeLocal = jsg::v8::ffi::MaybeLocal;
    }

    extern "Rust" {
        unsafe fn make_target(isolate: *mut Isolate) -> MaybeLocal;
        unsafe fn node_constructor(isolate: *mut Isolate) -> MaybeLocal;
        fn fast_call_count() -> KjMaybe<u64>;
        fn native_lifetime(count: u32, clones: u32) -> u32;
        type NativeResources;
        #[expect(
            clippy::unnecessary_box_returns,
            reason = "cxx takes an opaque Rust type boxed"
        )]
        fn retain_native(count: u32) -> Box<NativeResources>;
    }
}

#[jsg_struct]
struct Pair {
    pub a: f64,
    pub b: f64,
}

#[jsg_struct]
struct Octet {
    pub a: f64,
    pub b: f64,
    pub c: f64,
    pub d: f64,
    pub e: f64,
    pub f: f64,
    pub g: f64,
    pub h: f64,
}

#[jsg_oneof]
enum NumberOrString {
    Number(f64),
    String(String),
}

#[jsg_resource]
struct Node {
    value: Cell<u32>,
    child: Cell<Option<jsg::Rc<Self>>>,
}

#[jsg_resource]
impl Node {
    #[jsg_constructor]
    fn constructor(value: u32) -> Self {
        Self {
            value: Cell::new(value),
            child: Cell::new(None),
        }
    }

    #[jsg_property(prototype)]
    fn get_value(&self) -> u32 {
        self.value.get()
    }

    #[jsg_method]
    fn set_child(&self, child: jsg::Rc<Node>) {
        self.child.set(Some(child));
    }

    #[jsg_property(prototype)]
    fn get_child(&self) -> Option<jsg::Rc<Node>> {
        let child = self.child.take();
        self.child.set(child.clone());
        child
    }
}

#[jsg_resource]
struct Target {
    value: Cell<u32>,
    child: jsg::Rc<Node>,
}

#[jsg_resource]
impl Target {
    #[jsg_method]
    fn noop(&self) {}

    #[jsg_method]
    fn static_noop() {}

    #[jsg_method]
    fn add(&self, a: u32) -> u32 {
        a + 1
    }

    #[jsg_method]
    fn add_four(&self, a: u32, b: u32, c: u32, d: u32) -> u32 {
        a + b + c + d
    }

    #[jsg_method]
    fn number(&self, a: f64) -> f64 {
        a + 0.5
    }

    #[jsg_method]
    fn boolean(&self, a: bool) -> bool {
        !a
    }

    #[jsg_method]
    fn raw<'s>(&self, a: v8::Local<'s, v8::Value>) -> v8::Local<'s, v8::Value> {
        a
    }

    #[jsg_property(prototype)]
    fn get_value(&self) -> u32 {
        self.value.get()
    }

    #[jsg_property(prototype)]
    fn set_value(&self, value: u32) {
        self.value.set(value);
    }

    #[jsg_property(instance)]
    fn get_own(&self) -> u32 {
        self.value.get()
    }

    #[jsg_property(instance)]
    fn set_own(&self, value: u32) {
        self.value.set(value);
    }

    #[jsg_method]
    fn optional(&self, a: Option<u32>) -> u32 {
        a.unwrap_or(7)
    }

    #[jsg_method]
    fn union(&self, a: NumberOrString) -> f64 {
        match a {
            NumberOrString::Number(n) => n,
            NumberOrString::String(s) => (s.len() as u32).into(),
        }
    }

    #[jsg_method]
    fn string_len(&self, a: String) -> u32 {
        a.len() as u32
    }

    #[jsg_method]
    fn string_echo(&self, a: String) -> String {
        a
    }

    #[jsg_method]
    fn array_sum(&self, a: Vec<f64>) -> f64 {
        a.iter().copied().sum()
    }

    #[jsg_method]
    fn array_echo(&self, a: Vec<f64>) -> Vec<f64> {
        a
    }

    #[jsg_method]
    fn array_make(&self, n: u32) -> Vec<f64> {
        (0..n).map(f64::from).collect()
    }

    #[jsg_method]
    fn pair_sum(&self, a: Pair) -> f64 {
        a.a + a.b
    }

    #[jsg_method]
    fn pair_echo(&self, a: Pair) -> Pair {
        a
    }

    #[jsg_method]
    fn octet_sum(&self, a: Octet) -> f64 {
        a.a + a.b + a.c + a.d + a.e + a.f + a.g + a.h
    }

    #[jsg_method]
    fn octet_make(&self) -> Octet {
        Octet {
            a: 1.0,
            b: 2.0,
            c: 3.0,
            d: 4.0,
            e: 5.0,
            f: 6.0,
            g: 7.0,
            h: 8.0,
        }
    }

    #[jsg_method]
    fn bytes_len(&self, a: v8::Local<'_, v8::Uint8Array>) -> u32 {
        a.byte_length() as u32
    }

    #[jsg_method]
    fn bytes_copy<'s>(&self, lock: &mut jsg::Lock<'s>, a: v8::Local<'s, v8::Uint8Array>) -> u32 {
        let a = a.as_slice(lock).to_vec();
        black_box(&a);
        a.len() as u32
    }

    #[jsg_method]
    fn bytes_sum<'s>(&self, lock: &mut jsg::Lock<'s>, a: v8::Local<'s, v8::Uint8Array>) -> u32 {
        a.as_slice(lock).iter().map(|&x| u32::from(x)).sum()
    }

    #[jsg_method]
    fn new_node(&self, a: u32) -> jsg::Rc<Node> {
        jsg::Rc::new(Node::constructor(a))
    }

    #[jsg_method]
    fn same_node(&self) -> jsg::Rc<Node> {
        self.child.clone()
    }

    #[jsg_method]
    fn take_node(&self, a: jsg::Rc<Node>) -> u32 {
        a.value.get()
    }

    #[jsg_method]
    fn callback<'s>(
        &self,
        lock: &mut jsg::Lock<'s>,
        a: v8::Local<'s, v8::Value>,
        n: f64,
    ) -> Result<f64, jsg::Error> {
        let a: v8::Local<'s, v8::Function> = a
            .try_into()
            .map_err(|_| jsg::Error::new_type_error("expected a function"))?;
        let n = n.to_js(lock)?;
        let result = a.call(lock, None, &[n])?;
        f64::from_js(lock, result)
    }

    #[jsg_method]
    fn fail(&self) -> Result<(), jsg::Error> {
        Err(jsg::Error::new_type_error("benchmark error"))
    }
}

/// # Safety
/// C++ calls this with the isolate locked, a context entered and a `HandleScope` open.
unsafe fn make_target(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            let target = Target {
                value: Cell::new(0),
                child: jsg::Rc::new(Node::constructor(7)),
            };
            target.to_js(lock)
        })
    }
}

/// # Safety
/// As for `make_target`.
unsafe fn node_constructor(isolate: *mut v8::ffi::Isolate) -> v8::ffi::MaybeLocal {
    // SAFETY: forwarded from the caller.
    unsafe {
        v8::run_ffi_callback(isolate, |lock| {
            Ok::<_, jsg::Error>(jsg::resource::function_template_of::<Node>(lock))
        })
    }
}

/// Rust JSG Fast API callbacks run since the first call, or none in builds without debug
/// assertions, which don't count them.
fn fast_call_count() -> KjMaybe<u64> {
    #[cfg(debug_assertions)]
    let count = Some(jsg::fast_api::call_count());
    #[cfg(not(debug_assertions))]
    let count = None;
    count.into()
}

fn native_lifetime(count: u32, clones: u32) -> u32 {
    let mut result = 0;
    for i in 0..count {
        let node = jsg::Rc::new(Node::constructor(i));
        black_box(&node);
        for _ in 0..clones {
            let copy = node.clone();
            black_box(&copy);
        }
        result ^= node.value.get();
    }
    result
}

pub struct NativeResources {
    _nodes: Box<[jsg::Rc<Node>]>,
}

#[expect(
    clippy::unnecessary_box_returns,
    reason = "cxx takes an opaque Rust type boxed"
)]
fn retain_native(count: u32) -> Box<NativeResources> {
    Box::new(NativeResources {
        _nodes: (0..count)
            .map(|i| jsg::Rc::new(Node::constructor(i)))
            .collect::<Vec<_>>()
            .into_boxed_slice(),
    })
}
