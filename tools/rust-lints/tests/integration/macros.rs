#![deny(unsafe_code)]

extern crate proc_macro;

#[proc_macro_attribute]
pub fn identity(
    _attr: proc_macro::TokenStream,
    item: proc_macro::TokenStream,
) -> proc_macro::TokenStream {
    item
}

#[proc_macro]
pub fn generate(_input: proc_macro::TokenStream) -> proc_macro::TokenStream {
    "mod generated_helper {}".parse().unwrap()
}
