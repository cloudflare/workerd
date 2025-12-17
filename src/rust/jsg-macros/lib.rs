mod method;
mod resource;
mod struct_impl;

use proc_macro::TokenStream;

/// Generates `jsg::Struct` and `jsg::Type` implementations for data structures.
///
/// Only public fields are included in the generated JavaScript object.
/// Automatically implements `jsg::Type::class_name()` using the struct name,
/// or a custom name if provided via the `name` parameter.
///
/// # Example
/// ```rust
/// #[jsg::struct]
/// pub struct CaaRecord {
///     pub critical: u8,
///     pub field: String,
/// }
///
/// #[jsg::struct(name = "CustomName")]
/// pub struct MyRecord {
///     pub value: String,
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_struct(attr: TokenStream, item: TokenStream) -> TokenStream {
    struct_impl::jsg_struct_impl(attr, item)
}

/// Generates FFI callback for JSG methods.
///
/// Creates a `{method_name}_callback` extern "C" function that bridges JavaScript and Rust.
/// If no name is provided, automatically converts `snake_case` to `camelCase`.
///
/// # Example
/// ```rust
/// #[jsg::method(name = "parseRecord")]
/// pub fn parse_record(&self, data: &str) -> Result<Record, Error> {
///     // implementation
/// }
///
/// // Without name - automatically becomes "parseRecord"
/// #[jsg::method]
/// pub fn parse_record(&self, data: &str) -> Result<Record, Error> {
///     // implementation
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_method(attr: TokenStream, item: TokenStream) -> TokenStream {
    method::jsg_method_impl(attr, item)
}

/// Generates boilerplate code for JSG resources.
///
/// Works in two contexts:
/// 1. On a struct - generates `jsg::Type`, Wrapper, and `ResourceTemplate` implementations
/// 2. On an impl block - scans for `#[jsg::method]` and generates `Resource` trait implementation
///
/// The generated `GarbageCollected` implementation automatically traces fields that
/// need GC integration:
/// - `Ref<T>` fields - traces the underlying resource
/// - `TracedReference<T>` fields - traces the JavaScript handle
/// - `Option<T>` where T is traceable - conditionally traces
/// - `RefCell<Option<Ref<T>>>` - supports cyclic references through interior mutability
///
/// # Example
/// ```rust
/// #[jsg::resource]
/// pub struct DnsUtil {
///     pub _private: u8,
/// }
///
/// #[jsg::resource(name = "CustomUtil")]
/// pub struct MyUtil {
///     pub _private: u8,
/// }
///
/// #[jsg::resource]
/// impl DnsUtil {
///     #[jsg::method(name = "parseRecord")]
///     pub fn parse_record(&self, data: &str) -> Result<Record, Error> {
///         // implementation
///     }
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    resource::jsg_resource_impl(attr, item)
}

/// Extracts the `name = "..."` value from an attribute string.
pub(crate) fn extract_name_attribute(attr_str: &str) -> Option<String> {
    if !attr_str.contains("name") {
        return None;
    }

    attr_str
        .split('=')
        .nth(1)?
        .trim()
        .trim_matches(|c| c == '"' || c == ')' || c == ' ')
        .split('"')
        .next()
        .map(str::to_owned)
}

/// Converts `snake_case` to `camelCase`.
pub(crate) fn snake_to_camel_case(s: &str) -> String {
    let mut result = String::new();
    let mut capitalize_next = false;

    for (i, ch) in s.chars().enumerate() {
        if ch == '_' {
            capitalize_next = true;
        } else if i == 0 {
            result.push(ch);
        } else if capitalize_next {
            result.push(ch.to_ascii_uppercase());
            capitalize_next = false;
        } else {
            result.push(ch);
        }
    }

    result
}
