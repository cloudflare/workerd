// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// JavaScript can call back into a resource while one of its methods or accessors runs, so none
// of them may get exclusive access to the resource.

use jsg_macros::jsg_inspect_property;
use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;

#[jsg_resource]
pub struct Counter {
    count: u32,
}

#[jsg_resource]
impl Counter {
    #[jsg_method]
    pub fn increment(&mut self) {
        //~^ ERROR a resource method takes `&self`
        self.count += 1;
    }

    #[jsg_method]
    pub fn consume(self) {} //~ ERROR a resource method takes `&self`

    #[jsg_method]
    pub fn nested(self: &&Self) {} //~ ERROR a resource method takes `&self`

    #[jsg_method]
    pub fn explicit<'a>(self: &'a Self) -> f64 {
        f64::from(self.count)
    }

    #[jsg_property(prototype)]
    pub fn get_total(&self) -> f64 {
        f64::from(self.count)
    }

    #[jsg_property(prototype)]
    pub fn set_total(&mut self, _v: f64) {
        //~^ ERROR a resource method takes `&self`
    }

    #[jsg_property(instance)]
    pub fn get_current(&mut self) -> f64 {
        //~^ ERROR a resource method takes `&self`
        f64::from(self.count)
    }

    #[jsg_property(instance)]
    pub fn set_current(&mut self, _v: f64) {
        //~^ ERROR a resource method takes `&self`
    }

    #[jsg_inspect_property]
    pub fn debug_info(&mut self) -> String {
        //~^ ERROR a resource method takes `&self`
        String::new()
    }
}

// A rejected method is still emitted, so its callers add no errors of their own.
fn _calls(mut counter: Counter) -> f64 {
    counter.increment();
    let _ = counter.get_current();
    counter.consume();
    0.0
}
