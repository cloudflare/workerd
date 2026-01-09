use proc_macro::TokenStream;
use quote::ToTokens;
use quote::quote;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::FnArg;
use syn::ItemFn;
use syn::ItemImpl;
use syn::parse_macro_input;
use syn::spanned::Spanned;

/// Generates `jsg::Struct` and `jsg::Type` implementations for data structures.
///
/// Only public fields are included in the generated JavaScript object.
/// Use `name` parameter for custom JavaScript class name.
#[proc_macro_attribute]
pub fn jsg_struct(attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;
    let class_name = extract_name_attribute(&attr.to_string()).unwrap_or_else(|| name.to_string());

    let Data::Struct(data) = &input.data else {
        return error(&input, "#[jsg_struct] can only be applied to structs");
    };
    let Fields::Named(fields) = &data.fields else {
        return error(
            &input,
            "#[jsg_struct] only supports structs with named fields",
        );
    };

    let field_assignments = fields.named.iter().filter_map(|field| {
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            return None;
        }
        let field_name = field.ident.as_ref()?;
        let field_name_str = field_name.to_string();
        Some(quote! {
            let #field_name = jsg::v8::ToLocalValue::to_local(&this.#field_name, lock);
            obj.set(lock, #field_name_str, #field_name);
        })
    });

    quote! {
        #input

        impl jsg::Type for #name {
            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        impl jsg::ToJS for #name {
            fn to_js<'a, 'b>(self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                // TODO(soon): Use a precached ObjectTemplate instance to create the object,
                // similar to how C++ JSG optimizes object creation. This would avoid recreating
                // the object shape on every wrap() call and improve performance.
                unsafe {
                    let this = self;
                    let mut obj = lock.new_object();
                    #(#field_assignments)*
                    obj.into()
                }
            }
        }

        impl jsg::FromJS for #name {
            type ResultType = Self;

            fn from_js(_lock: &mut jsg::Lock, _value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                todo!("Struct from_js is not yet supported")
            }
        }

        impl jsg::Struct for #name {}
    }
    .into()
}

/// Generates FFI callback for JSG methods.
///
/// Parameters and return values are handled via `jsg::FromJS`.
/// See `jsg/wrappable.rs` for supported types.
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
        .filter_map(|arg| match arg {
            FnArg::Typed(pat_type) => Some(&pat_type.ty),
            FnArg::Receiver(_) => None,
        })
        .collect();

    let (unwraps, arg_exprs): (Vec<_>, Vec<_>) = params
        .iter()
        .enumerate()
        .map(|(i, ty)| {
            let arg = syn::Ident::new(&format!("arg{i}"), fn_name.span());
            let unwrap = quote! {
                let #arg = match <#ty as jsg::FromJS>::from_js(&mut lock, args.get(#i)) {
                    Ok(v) => v,
                    Err(err) => {
                        lock.throw_exception(&err);
                        return;
                    }
                };
            };
            // For reference types (like &str), FromJS returns an owned type (String),
            // so we need to borrow it when passing to the function.
            let is_ref = matches!(ty.as_ref(), syn::Type::Reference(_));
            let arg_expr = if is_ref {
                quote! { &#arg }
            } else {
                quote! { #arg }
            };
            (unwrap, arg_expr)
        })
        .unzip();

    // Check if return type is Result<T, E>
    let is_result = matches!(&fn_sig.output, syn::ReturnType::Type(_, ty) if is_result_type(ty));

    let result_handling = if is_result {
        quote! {
            match result {
                Ok(value) => args.set_return_value(jsg::ToJS::to_js(value, &mut lock)),
                Err(err) => lock.throw_exception(&err.into()),
            }
        }
    } else {
        quote! {
            args.set_return_value(jsg::ToJS::to_js(result, &mut lock));
        }
    };

    quote! {
        #fn_vis #fn_sig { #fn_block }

        #[automatically_derived]
        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
            #(#unwraps)*
            let this = args.this();
            let self_ = jsg::unwrap_resource::<Self>(&mut lock, this);
            let result = self_.#fn_name(#(#arg_exprs),*);
            #result_handling
        }
    }
    .into()
}

/// Generates boilerplate for JSG resources.
///
/// On structs: generates `jsg::Type` and `ResourceTemplate`.
/// On impl blocks: generates `Resource` trait with method registrations.
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(&impl_block);
    }

    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;
    let class_name = extract_name_attribute(&attr.to_string()).unwrap_or_else(|| name.to_string());
    let template_name = syn::Ident::new(&format!("{name}Template"), name.span());

    if !matches!(&input.data, Data::Struct(_)) {
        return error(
            &input,
            "#[jsg_resource] can only be applied to structs or impl blocks",
        );
    }

    quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str { #class_name }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                value.is_object()
            }
        }

        #[automatically_derived]
        impl jsg::ToJS for #name {
            fn to_js<'a, 'b>(self, _lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                todo!("Resource to_js is not yet supported")
            }
        }

        #[automatically_derived]
        impl jsg::FromJS for #name {
            type ResultType = jsg::Ref<Self>;

            fn from_js(_lock: &mut jsg::Lock, _value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                todo!("Resource from_js is not yet supported")
            }
        }

        #[automatically_derived]
        pub struct #template_name {
            pub constructor: jsg::v8::Global<jsg::v8::FunctionTemplate>,
        }

        #[automatically_derived]
        impl jsg::ResourceTemplate for #template_name {
            fn new(lock: &mut jsg::Lock) -> Self {
                Self { constructor: jsg::create_resource_constructor::<#name>(lock) }
            }

            fn get_constructor(&self) -> &jsg::v8::Global<jsg::v8::FunctionTemplate> {
                &self.constructor
            }
        }
    }
    .into()
}

fn generate_resource_impl(impl_block: &ItemImpl) -> TokenStream {
    let self_ty = &impl_block.self_ty;

    let method_registrations: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| {
            let syn::ImplItem::Fn(method) = item else {
                return None;
            };
            let attr = method.attrs.iter().find(|a| {
                a.path().is_ident("jsg")
                    || a.path()
                        .segments
                        .last()
                        .is_some_and(|s| s.ident == "jsg_method")
            })?;

            let rust_name = &method.sig.ident;
            let js_name = extract_name_attribute(&attr.meta.to_token_stream().to_string())
                .unwrap_or_else(|| snake_to_camel(&rust_name.to_string()));
            let callback = syn::Ident::new(&format!("{rust_name}_callback"), rust_name.span());

            Some(quote! {
                jsg::Member::Method { name: #js_name.to_owned(), callback: Self::#callback }
            })
        })
        .collect();

    let type_name = match &**self_ty {
        syn::Type::Path(p) => p
            .path
            .segments
            .last()
            .map_or("Unknown", |s| s.ident.to_string().leak()),
        _ => "Unknown",
    };
    let drop_fn = syn::Ident::new(&format!("drop_{type_name}"), self_ty.span());

    quote! {
        #impl_block

        #[allow(non_snake_case)]
        #[automatically_derived]
        unsafe extern "C" fn #drop_fn(isolate: *mut jsg::v8::ffi::Isolate, this: *mut std::os::raw::c_void) {
            jsg::drop_resource::<#self_ty>(isolate, this);
        }

        #[automatically_derived]
        impl jsg::Resource for #self_ty {
            fn members() -> Vec<jsg::Member> where Self: Sized {
                vec![#(#method_registrations,)*]
            }

            fn get_drop_fn(&self) -> unsafe extern "C" fn(*mut jsg::v8::ffi::Isolate, *mut std::os::raw::c_void) {
                #drop_fn
            }

            fn get_state(&mut self) -> &mut jsg::ResourceState {
                &mut self._state
            }
        }
    }
    .into()
}

fn error(tokens: &impl ToTokens, msg: &str) -> TokenStream {
    syn::Error::new_spanned(tokens, msg)
        .to_compile_error()
        .into()
}

fn extract_name_attribute(attr_str: &str) -> Option<String> {
    attr_str.find("name")?.checked_add(0)?;
    attr_str
        .split('=')
        .nth(1)?
        .trim()
        .trim_matches(|c| c == '"' || c == ')' || c == ' ')
        .split('"')
        .next()
        .map(str::to_owned)
}

fn snake_to_camel(s: &str) -> String {
    let mut result = String::new();
    let mut cap_next = false;
    for (i, c) in s.chars().enumerate() {
        match c {
            '_' => cap_next = true,
            _ if i == 0 => result.push(c),
            _ if cap_next => {
                result.push(c.to_ascii_uppercase());
                cap_next = false;
            }
            _ => result.push(c),
        }
    }
    result
}

/// Checks if a type is `Result<T, E>`.
fn is_result_type(ty: &syn::Type) -> bool {
    if let syn::Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
    {
        return segment.ident == "Result";
    }
    false
}
