// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A resource can hold a Fetcher and a DurableObjectClass, traced as C++ `jsg::Ref` fields are, and
// is `Send`, as resource state must be.
use channel_interop::DurableObjectClass;
use channel_interop::Fetcher;

pub struct Holder {
    pub fetcher: jsg::RefCell<Option<jsg_cpp::Ref<Fetcher>>>,
    pub class: jsg::RefCell<Option<jsg_cpp::Ref<DurableObjectClass>>>,
}

fn is_traced<T: jsg::Traced + Send>() {}

pub fn t() {
    is_traced::<Holder>();
}

impl jsg::Traced for Holder {
    fn trace(&self, visitor: &mut jsg::v8::GcVisitor) {
        jsg::Traced::trace(&self.fetcher, visitor);
        jsg::Traced::trace(&self.class, visitor);
    }
}
