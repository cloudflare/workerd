#![allow(unsafe_code)]

#[cfg(test)]
mod tests {
    mod nested {}
    #[cxx::bridge]
    mod ffi {
        extern "Rust" {
            fn answer() -> u32;
        }
    }
    fn answer() -> u32 {
        42
    }
}

#[cfg(test)]
#[path = "modules/test_external.rs"]
mod external_test;

#[cfg(all(test, policy_active))]
mod conditional_test {
    mod nested {}
}

#[cfg(not(not(test)))]
mod double_negation_test {}

#[cfg_attr(policy_active, cfg(test))]
mod conditional_attribute_test {
    mod nested {}
}

#[path = "modules/test_inner.rs"]
mod inner_test;
