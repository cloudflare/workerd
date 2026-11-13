// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// A method or accessor borrows its resource from the JavaScript wrapper it was called on, which
// keeps the resource alive only for the call. The borrow cannot outlive the call, so a receiver
// that asks for a longer one, to keep it, does not compile.

use std::cell::Cell;

use jsg_macros::jsg_method;
use jsg_macros::jsg_property;
use jsg_macros::jsg_resource;
use jsg_macros::jsg_wildcard_property;

thread_local! {
    static SAVED: Cell<Option<&'static Stash>> = const { Cell::new(None) };
    static SAVED_NAMES: Cell<Option<&'static Names>> = const { Cell::new(None) };
}

#[jsg_resource]
pub struct Stash {
    value: u32,
}

#[jsg_resource]
impl Stash {
    #[jsg_method]
    pub fn save(&'static self, _lock: &mut jsg::Lock<'_>) {
        //~^^ ERROR E0521
        //~| ERROR E0716
        SAVED.with(|saved| saved.set(Some(self)));
    }

    #[jsg_method]
    pub fn save_explicit(self: &'static Self, _lock: &mut jsg::Lock<'_>) {
        //~^^ ERROR E0521
        //~| ERROR E0716
        SAVED.with(|saved| saved.set(Some(self)));
    }

    #[jsg_method]
    pub fn save_bounded<'a: 'static>(&'a self, _lock: &mut jsg::Lock<'_>) {
        //~^^ ERROR E0521
        //~| ERROR E0716
        SAVED.with(|saved| saved.set(Some(self)));
    }

    #[jsg_property(prototype)]
    pub fn get_value(&'static self, _lock: &mut jsg::Lock<'_>) -> u32 {
        //~^^ ERROR E0521
        //~| ERROR E0716
        SAVED.with(|saved| saved.set(Some(self)));
        self.value
    }

    // A borrow for the call is all a method needs, whatever it is named.
    #[jsg_method]
    pub fn read<'a>(&'a self, _lock: &mut jsg::Lock<'_>) -> u32 {
        SAVED.with(|saved| saved.get().map_or(self.value, |stash| stash.value))
    }
}

#[jsg_resource]
pub struct Names {
    prefix: String,
}

#[jsg_resource]
impl Names {
    #[jsg_wildcard_property]
    pub fn lookup(&'static self, name: String) -> Option<String> {
        //~^^ ERROR E0521
        //~| ERROR E0716
        SAVED_NAMES.with(|saved| saved.set(Some(self)));
        Some(format!("{}{name}", self.prefix))
    }
}
