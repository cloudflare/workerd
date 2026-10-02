#![deny(unsafe_code)]

#[cfg(not(feature = "policy"))]
compile_error!("crate feature was not preserved");

const _: &str = env!("POLICY_ENV");
const _: &str = env!("POLICY_ENV_FILE");

include!("generated.rs");
include!("included.rs");

#[macros::identity]
mod attributed {
    #![deny(unsafe_code)]
}

macros::generate!();

pub fn answer() -> u32 {
    renamed_dependency::answer()
}

#[cfg(test)]
mod tests {
    #[test]
    fn answer() {
        assert_eq!(super::answer(), 42);
    }
}
