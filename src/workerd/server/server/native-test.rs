// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

use super::*;
use crate::config::InvalidConfigChannel;

#[test]
fn registered_services_are_taken_once_in_the_order_registered() {
    assert!(take().is_empty());
    register("first", |_, _| Ok(Rc::new(InvalidConfigChannel)));
    register("second", |_, _| Ok(Rc::new(InvalidConfigChannel)));
    let names: Vec<String> = take().into_iter().map(|(name, _)| name).collect();
    assert_eq!(names, ["first", "second"]);
    assert!(take().is_empty());
}

#[test]
fn a_registration_belongs_to_its_thread() {
    register("here", |_, _| Ok(Rc::new(InvalidConfigChannel)));
    let elsewhere = std::thread::spawn(|| take().len()).join().unwrap();
    assert_eq!(elsewhere, 0);
    assert_eq!(take().len(), 1);
}
