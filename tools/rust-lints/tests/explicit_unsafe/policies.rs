#![forbid(unsafe_code)]

#[deny(dead_code, unsafe_code)]
mod multi_lint {}

#[forbid(unsafe_code)]
mod stronger {}

#[cfg_attr(policy_active, forbid(unsafe_code))]
mod conditional {}
