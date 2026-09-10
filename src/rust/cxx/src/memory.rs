//! Less used details of `UniquePtr` and `SharedPtr`.
//!
//! The pointer types themselves are exposed at the crate root.

#[doc(no_inline)]
pub use cxx::SharedPtr;
#[doc(no_inline)]
pub use cxx::UniquePtr;

pub use crate::shared_ptr::SharedPtrTarget;
pub use crate::unique_ptr::UniquePtrTarget;
pub use crate::weak_ptr::WeakPtrTarget;
