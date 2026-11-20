use std::rc::Rc;

use static_assertions::assert_impl_all;
use static_assertions::assert_not_impl_any;

use super::LoopChecked;

assert_impl_all!(LoopChecked<Rc<u8>>: Send);
assert_not_impl_any!(LoopChecked<u8>: Sync);
