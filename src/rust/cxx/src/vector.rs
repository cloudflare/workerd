//! Less used details of `CxxVector`.
//!
//! `CxxVector` itself is exposed at the crate root.

#[doc(no_inline)]
pub use cxx::CxxVector;

#[doc(inline)]
pub use crate::Vector;
pub use crate::cxx_vector::Iter;
pub use crate::cxx_vector::IterMut;
pub use crate::cxx_vector::VectorElement;
