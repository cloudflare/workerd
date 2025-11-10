use proc_macro::TokenStream;
use quote::quote;
use syn::parse_macro_input;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::FnArg;
use syn::ItemFn;
use syn::Type;

/// Generates `jsg::Struct` implementation for data structures.
///
/// Only public fields are included in the generated JavaScript object.
///
/// # Example
/// ```rust
/// #[jsg::struct]
/// pub struct CaaRecord {
///     pub critical: u8,
///     pub field: String,
/// }
/// ```
///
/// # Panics
/// Panics if applied to non-struct items or structs without named fields.
#[proc_macro_attribute]
pub fn r#struct(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;

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
                let #field_name = self.#field_name.to_local(lock);
                obj.set(lock, #field_name_str, #field_name);
            })
        } else {
            None
        }
    });

    let expanded = quote! {
        #input

        impl jsg::Struct for #name {
            fn wrap<'a, 'b>(&self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                unsafe {
                    let mut obj = lock.new_object();
                    #(#field_assignments)*
                    obj.into()
                }
            }
        }
    };

    TokenStream::from(expanded)
}

/// Generates FFI callback for JSG methods.
///
/// Creates a `{method_name}_callback` extern "C" function that bridges JavaScript and Rust.
///
/// # Example
/// ```rust
/// #[jsg::method(name = "parseRecord")]
/// pub fn parse_record(&self, data: &str) -> Result<Record, Error> {
///     // implementation
/// }
/// ```
///
/// # Panics
/// Panics if attribute format is invalid.
#[proc_macro_attribute]
pub fn method(attr: TokenStream, item: TokenStream) -> TokenStream {
    let attr_str = attr.to_string();
    let _js_name = attr_str
        .trim()
        .strip_prefix("name =")
        .and_then(|s| s.trim().strip_prefix('"'))
        .and_then(|s| s.strip_suffix('"'))
        .expect("Expected attribute format: #[jsg::method(name = \"methodName\")]");

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

        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            let args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
            #(#unwrap_statements)*
            let this = args.this();
            let self_ = jsg::unwrap_resource::<Self>(&mut lock, this);
            match self_.#fn_name(#(#arg_refs),*) {
                Ok(result) => args.set_return_value(result.wrap(&mut lock)),
                Err(err) => {
                    todo!("{err}");
                }
            }
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
                jsg::v8::ffi::unwrap_string(lock.get_isolate(), args.get(#index).into_ffi())
            };
        }
    } else {
        quote! {
            compile_error!("Unsupported parameter type for jsg::method. Currently only &str is supported.");
        }
    }
}
