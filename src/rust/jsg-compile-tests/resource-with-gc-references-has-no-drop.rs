// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `Drop` has `&mut self`, so it could move a `jsg::Rc` or a `TracedGlobal` out of a field that
// the garbage collector made weak, and nothing would make it strong again. So a resource whose
// fields hold GC references, inside a wrapper or a cell too, can't implement `Drop`.
use jsg::v8;
use jsg_macros::jsg_resource;

#[jsg_resource]
struct Leaf;

#[jsg_resource]
impl Leaf {}

#[jsg_resource] //~ ERROR E0080
struct HoldsRc {
    child: jsg::Rc<Leaf>,
}

impl Drop for HoldsRc {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl HoldsRc {}

#[jsg_resource] //~ ERROR E0080
struct HoldsTracedGlobal {
    handle: Option<v8::TracedGlobal<v8::Value>>,
}

impl Drop for HoldsTracedGlobal {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl HoldsTracedGlobal {}

#[jsg_resource] //~ ERROR E0080
struct HoldsCellOfRc {
    name: String,
    child: jsg::Cell<Option<jsg::Rc<Leaf>>>,
}

impl Drop for HoldsCellOfRc {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl HoldsCellOfRc {}

// A `custom_trace` resource's fields need not be `Traced`; one that isn't counts as holding GC
// references.
#[jsg_resource(custom_trace)] //~ ERROR E0080
struct CustomTrace {
    child: jsg::Rc<Leaf>,
}

impl jsg::Traced for CustomTrace {
    fn trace(&self, visitor: &mut jsg::GcVisitor) {
        visitor.visit_rc(&self.child);
    }
}

impl Drop for CustomTrace {
    fn drop(&mut self) {}
}

#[jsg_resource]
impl CustomTrace {}
