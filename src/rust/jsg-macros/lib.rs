use proc_macro::TokenStream;
use quote::ToTokens;
use quote::quote;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::FnArg;
use syn::ItemFn;
use syn::ItemImpl;
use syn::Type;
use syn::parse_macro_input;
use syn::spanned::Spanned;

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
///
/// # Panics
/// Panics if applied to non-struct items or structs without named fields.
#[proc_macro_attribute]
pub fn jsg_struct(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;

    let class_name = if attr.is_empty() {
        name.to_string()
    } else {
        extract_name_attribute(&attr.to_string()).unwrap_or_else(|| name.to_string())
    };

    let fields = match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(fields) => &fields.named,
            _ => {
                return syn::Error::new_spanned(
                    &input,
                    "#[jsg::struct] only supports structs with named fields",
                )
                .to_compile_error()
                .into();
            }
        },
        _ => {
            return syn::Error::new_spanned(
                &input,
                "#[jsg::struct] can only be applied to structs",
            )
            .to_compile_error()
            .into();
        }
    };

    let field_assignments = fields.iter().filter_map(|field| {
        let is_public = matches!(field.vis, syn::Visibility::Public(_));
        if is_public {
            let field_name = &field.ident;
            let field_name_str = field_name
                .as_ref()
                .expect("Named fields must have identifiers")
                .to_string();
            Some(quote! {
                let #field_name = jsg::v8::ToLocalValue::to_local(&self.#field_name, lock);
                obj.set(lock, #field_name_str, #field_name);
            })
        } else {
            None
        }
    });

    let expanded = quote! {
        #input

        impl jsg::Type for #name {
            fn class_name() -> &'static str {
                #class_name
            }

            fn wrap<'a, 'b>(&self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                // TODO(soon): Use a precached ObjectTemplate instance to create the object,
                // similar to how C++ JSG optimizes object creation. This would avoid recreating
                // the object shape on every wrap() call and improve performance.
                unsafe {
                    let mut obj = lock.new_object();
                    #(#field_assignments)*
                    obj.into()
                }
            }
        }

        impl jsg::Struct for #name {

        }
    };

    TokenStream::from(expanded)
}

/// Generates FFI callback for JSG methods.
///
/// Creates a `{method_name}_callback` extern "C" function that bridges JavaScript and Rust.
/// If no name is provided, automatically converts `snake_case` to `camelCase`.
///
/// # Example
/// ```rust
/// // With explicit name
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
pub fn jsg_method(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let fn_vis = &input_fn.vis;
    let fn_sig = &input_fn.sig;
    let fn_block = &input_fn.block;

    let callback_name = syn::Ident::new(&format!("{fn_name}_callback"), fn_name.span());

    let params: Vec<_> = fn_sig
        .inputs
        .iter()
        .filter_map(|arg| {
            if let FnArg::Typed(pat_type) = arg {
                Some((&pat_type.pat, &pat_type.ty))
            } else {
                None
            }
        })
        .collect();

    let arg_unwraps: Vec<_> = params
        .iter()
        .enumerate()
        .map(|(i, (_pat, ty))| {
            let arg_name = syn::Ident::new(&format!("arg{i}"), fn_name.span());
            let unwrap_code = generate_unwrap_code(&arg_name, ty, i);
            (arg_name, unwrap_code)
        })
        .collect();

    let unwrap_statements = arg_unwraps
        .iter()
        .map(|(_arg_name, unwrap_code)| unwrap_code);

    let arg_refs: Vec<_> = params
        .iter()
        .zip(arg_unwraps.iter())
        .map(|((_, ty), (arg_name, _))| {
            if is_str_reference(ty) {
                quote! { &#arg_name }
            } else {
                quote! { #arg_name }
            }
        })
        .collect();

    let expanded = quote! {
        #fn_vis #fn_sig {
            #fn_block
        }

        #[automatically_derived]
        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
            #(#unwrap_statements)*
            let this = args.this();
            let self_ = jsg::unwrap_resource::<Self>(&mut lock, this);
            let result = self_.#fn_name(#(#arg_refs),*);
            unsafe { jsg::handle_result(&mut lock, &mut args, result) };
        }
    };

    TokenStream::from(expanded)
}

fn is_str_reference(ty: &Type) -> bool {
    match ty {
        Type::Reference(type_ref) => {
            matches!(&*type_ref.elem, Type::Path(type_path) if type_path.path.is_ident("str"))
        }
        _ => false,
    }
}

fn generate_unwrap_code(
    arg_name: &syn::Ident,
    ty: &Type,
    index: usize,
) -> quote::__private::TokenStream {
    if is_str_reference(ty) {
        quote! {
            let #arg_name = unsafe {
                jsg::v8::ffi::unwrap_string(lock.isolate().as_ptr(), args.get(#index).into_ffi())
            };
        }
    } else {
        quote! {
            compile_error!("Unsupported parameter type for jsg::method. Currently only &str is supported.");
        }
    }
}

/// Generates boilerplate code for JSG resources.
///
/// Works in two contexts:
/// 1. On a struct - generates `jsg::Type`, Wrapper, and `ResourceTemplate` implementations
/// 2. On an impl block - scans for `#[jsg::method]` and generates `Resource` trait implementation
///
/// Automatically implements `jsg::Type::class_name()` using the struct name,
/// or a custom name if provided via the `name` parameter.
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
///
/// # Panics
/// Panics if applied to items other than structs or impl blocks.
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    // Try to parse as an impl block first
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(&impl_block);
    }

    // Otherwise, parse as a struct
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;

    let class_name = if attr.is_empty() {
        name.to_string()
    } else {
        extract_name_attribute(&attr.to_string()).unwrap_or_else(|| name.to_string())
    };

    let template_name = syn::Ident::new(&format!("{name}Template"), name.span());

    // Ensure it's a struct
    if !matches!(&input.data, Data::Struct(_)) {
        return syn::Error::new_spanned(
            &input,
            "#[jsg::resource] can only be applied to structs or impl blocks",
        )
        .to_compile_error()
        .into();
    }

    let expanded = quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str {
                #class_name
            }

            fn wrap<'a, 'b>(&self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                todo!("Implement wrap for jsg::Resource")
            }
        }

        #[automatically_derived]
        pub struct #template_name {
            pub constructor: jsg::v8::Global<jsg::v8::FunctionTemplate>,
        }

        #[automatically_derived]
        impl jsg::ResourceTemplate for #template_name {
            fn new(lock: &mut jsg::Lock) -> Self {
                Self {
                    constructor: jsg::create_resource_constructor::<#name>(lock),
                }
            }

            fn get_constructor(&self) -> &jsg::v8::Global<jsg::v8::FunctionTemplate> {
                &self.constructor
            }
        }
    };

    TokenStream::from(expanded)
}

fn generate_resource_impl(impl_block: &ItemImpl) -> TokenStream {
    let self_ty = &impl_block.self_ty;
    let mut method_registrations = vec![];

    for item in &impl_block.items {
        if let syn::ImplItem::Fn(method) = item {
            for attr in &method.attrs {
                // TODO: More reliable way to detect jsg_method attribute
                if attr.path().is_ident("jsg")
                    || (attr.path().segments.len() == 1
                        && attr.path().segments[0].ident == "jsg_method")
                {
                    let rust_method_name = &method.sig.ident;

                    let attr_str = attr.meta.to_token_stream().to_string();
                    let js_name = extract_name_attribute(&attr_str)
                        .unwrap_or_else(|| snake_to_camel_case(&rust_method_name.to_string()));

                    let callback_name = syn::Ident::new(
                        &format!("{rust_method_name}_callback"),
                        rust_method_name.span(),
                    );

                    method_registrations.push(quote! {
                        jsg::Member::Method {
                            name: #js_name,
                            callback: Self::#callback_name,
                        }
                    });
                }
            }
        }
    }

    // Create a unique drop callback function name based on the type
    let type_name = match &**self_ty {
        syn::Type::Path(type_path) => type_path
            .path
            .segments
            .last()
            .map_or_else(|| "Unknown".to_owned(), |seg| seg.ident.to_string()),
        _ => "Unknown".to_owned(),
    };
    let drop_callback_name =
        syn::Ident::new(&format!("drop_{type_name}"), impl_block.self_ty.span());

    let expanded = quote! {
        #impl_block

        #[allow(non_snake_case)]
        #[automatically_derived]
        unsafe extern "C" fn #drop_callback_name(isolate: *mut jsg::v8::ffi::Isolate, this: *mut std::os::raw::c_void) {
            jsg::drop_resource::<#self_ty>(isolate, this);
        }

        #[automatically_derived]
        impl jsg::Resource for #self_ty {
            fn members() -> Vec<jsg::Member>
            where
                Self: Sized,
            {
                vec![
                    #(#method_registrations,)*
                ]
            }

            fn get_drop_fn(&self) -> unsafe extern "C" fn(*mut jsg::v8::ffi::Isolate, *mut std::os::raw::c_void) {
                #drop_callback_name
            }

            fn get_state(&mut self) -> &mut jsg::ResourceState {
                &mut self._state
            }
        }
    };

    TokenStream::from(expanded)
}

fn extract_name_attribute(attr_str: &str) -> Option<String> {
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

fn snake_to_camel_case(s: &str) -> String {
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
