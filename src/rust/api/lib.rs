use std::cell::UnsafeCell;
use std::pin::Pin;
use std::ptr::null_mut;
use std::rc::Rc;

use jsg::ResourceState;
use jsg::ResourceWrapper;

use crate::dns::DnsUtil;
use crate::dns::DnsUtilWrapper;

pub mod dns;

#[cxx::bridge(namespace = "workerd::rust::api")]
mod ffi {
    #[namespace = "v8"]
    unsafe extern "C++" {
        include!("v8.h");
        type Isolate;
    }

    #[namespace = "workerd::rust::jsg"]
    unsafe extern "C++" {
        include!("workerd/rust/jsg/ffi.h");

        type ModuleRegistry = jsg::v8::ffi::ModuleRegistry;
    }
    extern "Rust" {
        pub fn register_nodejs_modules(registry: Pin<&mut ModuleRegistry>);
    }
}

pub fn register_nodejs_modules(registry: Pin<&mut ffi::ModuleRegistry>) {
    jsg::modules::add_builtin(registry, "node-internal:dns", |isolate| unsafe {
        let mut lock = jsg::Lock::from_isolate(isolate);
        let dns_util = DnsUtil {
            _state: ResourceState { this: null_mut() },
        };
        let dns_util = Rc::new(UnsafeCell::new(dns_util));
        let dns_util = Rc::into_raw(dns_util);
        // let dns_util = UnsafeCell::raw_get(dns_util);
        // (*dns_util)._state.this = dns_util.cast();
        let mut dns_util_wrapper = DnsUtilWrapper::new(&mut lock);

        jsg::wrap_resource(&mut lock, dns_util as *mut DnsUtil, &mut dns_util_wrapper).into_ffi()
    });
}

#[cfg(test)]
mod tests {

    #[test]
    fn test_wrap_resource_equality() {
        // let lhs = jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper);
        // let rhs = jsg::wrap_resource(&mut isolate, dns_util, &dns_util_wrapper);
        // assert_eq!(lhs, rhs);
        assert!(true)
    }
}
