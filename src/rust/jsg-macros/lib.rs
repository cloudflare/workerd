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

    let field_extractions = fields.named.iter().filter_map(|field| {
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            return None;
        }
        let field_name = field.ident.as_ref()?;
        let field_name_str = field_name.to_string();
        let field_type = &field.ty;
        Some(quote! {
            let #field_name = {
                let prop = obj.get(lock, #field_name_str)
                    .ok_or_else(|| jsg::Error::new_type_error(
                        format!("Missing property '{}'", #field_name_str)
                    ))?;
                <#field_type as jsg::FromJS>::from_js(lock, prop)?
            };
        })
    });

    let field_names = fields.named.iter().filter_map(|field| {
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            return None;
        }
        field.ident.as_ref()
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

            fn from_js(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                if !value.is_object() {
                    return Err(jsg::Error::new_type_error(
                        format!("Expected object but got {}", value.type_of())
                    ));
                }
                let obj: jsg::v8::Local<'_, jsg::v8::Object> = value.into();
                #(#field_extractions)*
                Ok(Self { #(#field_names),* })
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
            let self_ = <Self as jsg::Resource>::unwrap(&mut lock, this);
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
///
/// The generated `GarbageCollected` implementation automatically traces fields that
/// need GC integration:
/// - `Ref<T>` fields - traces the underlying resource
/// - `TracedReference<T>` fields - traces the JavaScript handle
/// - `Option<T>` where T is traceable - conditionally traces
/// - `RefCell<Option<Ref<T>>>` - supports cyclic references through interior mutability
#[proc_macro_attribute]
pub fn jsg_resource(attr: TokenStream, item: TokenStream) -> TokenStream {
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(&impl_block);
    }

    let input = parse_macro_input!(item as DeriveInput);
    let name: &syn::Ident = &input.ident;

    let class_name = if attr.is_empty() {
        name.to_string()
    } else {
        extract_name_attribute(&attr.to_string()).unwrap_or_else(|| name.to_string())
    };

    let template_name = template_name(name);

    // Extract fields for trace generation
    let fields = match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(fields) => &fields.named,
            _ => {
                return syn::Error::new_spanned(
                    &input,
                    "#[jsg::resource] only supports structs with named fields",
                )
                .to_compile_error()
                .into();
            }
        },
        _ => {
            return syn::Error::new_spanned(
                &input,
                "#[jsg::resource] can only be applied to structs or impl blocks",
            )
            .to_compile_error()
            .into();
        }
    };

    // Generate trace statements for traceable fields
    let trace_statements = generate_trace_statements(fields);
    let gc_impl = if trace_statements.is_empty() {
        quote! {
            #[automatically_derived]
            impl jsg::GarbageCollected for #name {}
        }
    } else {
        quote! {
            #[automatically_derived]
            impl jsg::GarbageCollected for #name {
                fn trace(&self, visitor: &mut jsg::GcVisitor) {
                    #(#trace_statements)*
                }
            }
        }
    };

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

        #gc_impl

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

#[derive(Debug, Clone, Copy, PartialEq)]
enum TraceableType {
    /// `Ref<T>` - trace via `GarbageCollected` trait on the inner type
    Ref,
    /// `WeakRef<T>` - weak reference, no tracing needed (doesn't keep alive)
    WeakRef,
    /// `TracedReference<T>` - trace via `visitor.trace()`
    TracedReference,
    /// Not a traceable type
    None,
}

/// Checks if a type path matches a known traceable type.
/// Supports both unqualified (`Ref<T>`) and qualified (`jsg::Ref<T>`) paths.
fn get_traceable_type(ty: &Type) -> TraceableType {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
    {
        match segment.ident.to_string().as_str() {
            "Ref" => return TraceableType::Ref,
            "WeakRef" => return TraceableType::WeakRef,
            "TracedReference" => return TraceableType::TracedReference,
            _ => {}
        }
    }
    TraceableType::None
}

/// Extracts the inner type from `Option<T>` or `std::option::Option<T>` if present.
fn extract_option_inner(ty: &Type) -> Option<&Type> {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
        && segment.ident == "Option"
        && let syn::PathArguments::AngleBracketed(args) = &segment.arguments
        && let Some(syn::GenericArgument::Type(inner)) = args.args.first()
    {
        return Some(inner);
    }
    None
}

/// Extracts the inner type from `RefCell<T>` or `std::cell::RefCell<T>` if present.
fn extract_refcell_inner(ty: &Type) -> Option<&Type> {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
        && segment.ident == "RefCell"
        && let syn::PathArguments::AngleBracketed(args) = &segment.arguments
        && let Some(syn::GenericArgument::Type(inner)) = args.args.first()
    {
        return Some(inner);
    }
    None
}

/// Generates trace statements for all traceable fields in a struct.
fn generate_trace_statements(
    fields: &syn::punctuated::Punctuated<syn::Field, syn::token::Comma>,
) -> Vec<quote::__private::TokenStream> {
    fields
        .iter()
        .filter_map(|field| {
            let field_name = field.ident.as_ref()?;
            let ty = &field.ty;

            // Check if it's RefCell<...> wrapping a traceable type
            if let Some(refcell_inner) = extract_refcell_inner(ty) {
                // RefCell<Option<Traceable>>
                if let Some(option_inner) = extract_option_inner(refcell_inner) {
                    match get_traceable_type(option_inner) {
                        TraceableType::Ref => {
                            return Some(quote! {
                                if let Some(ref inner) = *self.#field_name.borrow() {
                                    visitor.visit_ref(inner);
                                }
                            });
                        }
                        TraceableType::WeakRef => {
                            return Some(quote! {
                                if let Some(ref inner) = *self.#field_name.borrow() {
                                    inner.trace(visitor);
                                }
                            });
                        }
                        TraceableType::TracedReference => {
                            return Some(quote! {
                                if let Some(ref inner) = *self.#field_name.borrow() {
                                    visitor.trace(inner);
                                }
                            });
                        }
                        TraceableType::None => {}
                    }
                }

                // RefCell<Traceable> (without Option)
                match get_traceable_type(refcell_inner) {
                    TraceableType::Ref => {
                        return Some(quote! {
                            visitor.visit_ref(&*self.#field_name.borrow());
                        });
                    }
                    TraceableType::WeakRef => {
                        return Some(quote! {
                            self.#field_name.borrow().trace(visitor);
                        });
                    }
                    TraceableType::TracedReference => {
                        return Some(quote! {
                            visitor.trace(&*self.#field_name.borrow());
                        });
                    }
                    TraceableType::None => {}
                }
            }

            // Check if it's Option<Traceable>
            if let Some(inner_ty) = extract_option_inner(ty) {
                match get_traceable_type(inner_ty) {
                    TraceableType::Ref => {
                        return Some(quote! {
                            if let Some(ref inner) = self.#field_name {
                                visitor.visit_ref(inner);
                            }
                        });
                    }
                    TraceableType::WeakRef => {
                        return Some(quote! {
                            if let Some(ref inner) = self.#field_name {
                                inner.trace(visitor);
                            }
                        });
                    }
                    TraceableType::TracedReference => {
                        return Some(quote! {
                            if let Some(ref inner) = self.#field_name {
                                visitor.trace(inner);
                            }
                        });
                    }
                    TraceableType::None => {}
                }
            }

            match get_traceable_type(ty) {
                TraceableType::Ref => Some(quote! {
                    visitor.visit_ref(&self.#field_name);
                }),
                TraceableType::WeakRef => Some(quote! {
                    self.#field_name.trace(visitor);
                }),
                TraceableType::TracedReference => Some(quote! {
                    visitor.trace(&self.#field_name);
                }),
                TraceableType::None => None,
            }
        })
        .collect()
}

fn template_name(name: &syn::Ident) -> syn::Ident {
    syn::Ident::new(&format!("{name}Template"), name.span())
}

fn generate_resource_impl(impl_block: &ItemImpl) -> TokenStream {
    let self_ty = &impl_block.self_ty;
    let mut method_registrations = Vec::new();

    for item in &impl_block.items {
        if let syn::ImplItem::Fn(method) = item {
            for attr in &method.attrs {
                if attr.path().is_ident("jsg")
                    || (attr.path().segments.len() == 1
                        && attr.path().segments[0].ident == "jsg_method")
                {
                    let rust_method_name = &method.sig.ident;

                    let js_name = extract_name_attribute(&attr.meta.to_token_stream().to_string())
                        .unwrap_or_else(|| snake_to_camel(&rust_method_name.to_string()));
                    let callback_name = syn::Ident::new(
                        &format!("{rust_method_name}_callback"),
                        rust_method_name.span(),
                    );

                    method_registrations.push(quote! {
                        jsg::Member::Method {
                            name: #js_name.to_owned(),
                            callback: Self::#callback_name,
                        }
                    });
                }
            }
        }
    }

    let type_name = match &**self_ty {
        syn::Type::Path(type_path) => {
            &type_path
                .path
                .segments
                .last()
                .expect("Type path must have at least one segment")
                .ident
        }
        _ => todo!(),
    };
    let template_name = template_name(type_name);

    quote! {
        #impl_block

        #[automatically_derived]
        impl jsg::Resource for #self_ty {
            type Template = #template_name;

            fn members() -> Vec<jsg::Member>
            where
                Self: Sized,
            {
                vec![
                    #(#method_registrations,)*
                ]
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

/// Generates `jsg::Type` and `jsg::FromJS` implementations for union types.
///
/// This macro automatically implements the traits needed for enums with
/// single-field tuple variants to be used directly as `jsg_method` parameters.
/// Each variant should contain a type that implements `jsg::Type` and `jsg::FromJS`.
///
/// # Example
///
/// ```ignore
/// use jsg_macros::jsg_oneof;
///
/// #[jsg_oneof]
/// #[derive(Debug, Clone)]
/// enum StringOrNumber {
///     String(String),
///     Number(f64),
/// }
///
/// // Use directly as a parameter type:
/// #[jsg_method]
/// fn process(&self, value: StringOrNumber) -> Result<String, jsg::Error> {
///     match value {
///         StringOrNumber::String(s) => Ok(format!("string: {}", s)),
///         StringOrNumber::Number(n) => Ok(format!("number: {}", n)),
///     }
/// }
/// ```
#[proc_macro_attribute]
pub fn jsg_oneof(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input = parse_macro_input!(item as DeriveInput);
    let name = &input.ident;

    let Data::Enum(data) = &input.data else {
        return error(&input, "#[jsg_oneof] can only be applied to enums");
    };

    let mut variants = Vec::new();
    for variant in &data.variants {
        let variant_name = &variant.ident;
        let Fields::Unnamed(fields) = &variant.fields else {
            return error(
                variant,
                "#[jsg_oneof] variants must be tuple variants (e.g., `Variant(Type)`)",
            );
        };
        if fields.unnamed.len() != 1 {
            return error(variant, "#[jsg_oneof] variants must have exactly one field");
        }
        let inner_type = &fields.unnamed[0].ty;
        variants.push((variant_name, inner_type));
    }

    if variants.is_empty() {
        return error(&input, "#[jsg_oneof] requires at least one variant");
    }

    let type_checks: Vec<_> = variants
        .iter()
        .map(|(variant_name, inner_type)| {
            quote! {
                if let Some(result) = <#inner_type as jsg::FromJS>::try_from_js_exact(lock, &value) {
                    return result.map(Self::#variant_name);
                }
            }
        })
        .collect();

    let type_names: Vec<_> = variants
        .iter()
        .map(|(_, inner_type)| {
            quote! { <#inner_type as jsg::Type>::class_name() }
        })
        .collect();

    let is_exact_checks: Vec<_> = variants
        .iter()
        .map(|(_, inner_type)| {
            quote! { <#inner_type as jsg::Type>::is_exact(value) }
        })
        .collect();

    let error_msg = quote! {
        let expected: Vec<&str> = vec![#(#type_names),*];
        let msg = format!(
            "Expected one of [{}] but got {}",
            expected.join(", "),
            value.type_of()
        );
        Err(jsg::Error::new_type_error(msg))
    };

    quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            fn class_name() -> &'static str {
                stringify!(#name)
            }

            fn is_exact(value: &jsg::v8::Local<jsg::v8::Value>) -> bool {
                #(#is_exact_checks)||*
            }
        }

        #[automatically_derived]
        impl jsg::FromJS for #name {
            type ResultType = Self;

            fn from_js(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Result<Self::ResultType, jsg::Error> {
                #(#type_checks)*
                #error_msg
            }
        }
    }
    .into()
}
