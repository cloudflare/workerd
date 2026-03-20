// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

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
    let class_name = extract_name_attribute(attr).unwrap_or_else(|| name.to_string());

    let named_fields = match extract_named_fields(&input, "jsg_struct") {
        Ok(fields) => fields,
        Err(err) => return err,
    };

    let mut field_assignments = Vec::new();
    let mut field_extractions = Vec::new();
    let mut field_names = Vec::new();

    for field in &named_fields {
        // Only public fields are projected into JavaScript objects.
        if !matches!(field.vis, syn::Visibility::Public(_)) {
            continue;
        }
        // Named fields always have an ident; guard is purely defensive.
        let Some(field_name) = field.ident.as_ref() else {
            continue;
        };
        let field_name_str = field_name.to_string();
        let field_type = &field.ty;

        field_assignments.push(quote! {
            let #field_name = jsg::v8::ToLocalValue::to_local(&this.#field_name, lock);
            obj.set(lock, #field_name_str, #field_name);
        });
        field_extractions.push(quote! {
            let #field_name = {
                let prop = obj.get(lock, #field_name_str)
                    .ok_or_else(|| jsg::Error::new_type_error(
                        format!("Missing property '{}'", #field_name_str)
                    ))?;
                <#field_type as jsg::FromJS>::from_js(lock, prop)?
            };
        });
        field_names.push(field_name);
    }

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
                {
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

    // Methods with a receiver (&self, &mut self) become instance methods on the prototype.
    // Methods without a receiver become static methods on the constructor.
    let has_self = fn_sig
        .inputs
        .iter()
        .any(|arg| matches!(arg, FnArg::Receiver(_)));

    let params: Vec<_> = fn_sig
        .inputs
        .iter()
        .filter_map(|arg| match arg {
            FnArg::Typed(pat_type) => Some(&pat_type.ty),
            FnArg::Receiver(_) => None,
        })
        .collect();

    // Check if the first typed parameter is `&mut Lock` — if so, pass `&mut lock`
    // directly instead of extracting it from JS args (like C++ jsg::Lock&).
    let has_lock_param = params.first().is_some_and(|ty| is_lock_ref(ty));
    let js_arg_offset = usize::from(has_lock_param);

    let (unwraps, arg_exprs): (Vec<_>, Vec<_>) = params
        .iter()
        .enumerate()
        .map(|(i, ty)| {
            // First param is &mut Lock — pass the callback's lock directly.
            if i == 0 && has_lock_param {
                let unwrap = quote! {};
                let arg_expr = quote! { &mut lock };
                return (unwrap, arg_expr);
            }

            let js_index = i - js_arg_offset;
            let arg = syn::Ident::new(&format!("arg{js_index}"), fn_name.span());
            let unwrap = quote! {
                let #arg = match <#ty as jsg::FromJS>::from_js(&mut lock, args.get(#js_index)) {
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

    let invocation = if has_self {
        quote! {
            let this = args.this();
            // SAFETY: `v8::Signature` (passed to `FunctionTemplate::New` in
            // `create_resource_template`) enforces that V8 only dispatches this
            // callback when `this` is an instance of the resource's own
            // `FunctionTemplate`. If the caller destructures the method and calls
            // it with a wrong receiver (e.g. `const {abort} = ac; abort()`), V8
            // throws a `TypeError: Illegal invocation` *before* reaching this
            // code. Given that guarantee, `from_js` / `resolve_resource` perform
            // a belt-and-suspenders `TypeId` check; the `.expect` panics
            // (aborting the isolate) rather than triggering UB on a mismatch.
            // The `&mut` is sound because V8 is single-threaded and no other
            // Rust code can alias the resource during the callback.
            let self_: &mut Self = unsafe {
                let wrappable = jsg::v8::WrappableRc::from_js(lock.isolate(), this)
                    .expect("receiver is not a Rust-wrapped resource");
                &mut *wrappable.resolve_resource::<Self>()
                    .expect("type mismatch in resource callback")
                    .as_ptr()
            };
            let result = self_.#fn_name(#(#arg_exprs),*);
        }
    } else {
        quote! {
            let result = Self::#fn_name(#(#arg_exprs),*);
        }
    };

    quote! {
        #fn_vis #fn_sig { #fn_block }

        #[automatically_derived]
        #[expect(clippy::undocumented_unsafe_blocks)]
        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            jsg::catch_panic(&mut lock, || {
                let mut lock = unsafe { jsg::Lock::from_args(args) };
                let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
                #(#unwraps)*
                #invocation
                #result_handling
            });
        }
    }
    .into()
}

/// Generates boilerplate for JSG resources.
///
/// On structs: generates `jsg::Type`, `jsg::ToJS`, `jsg::FromJS`, and `jsg::GarbageCollected`.
/// On impl blocks: generates `Resource` trait with method registrations.
///
/// The generated `GarbageCollected` implementation automatically traces fields that
/// need GC integration:
/// - `Rc<T>` fields - traced as a strong GC edge
/// - `Option<Rc<T>>` / `Nullable<Rc<T>>` - conditionally traced when `Some`
/// - `Weak<T>` fields - not traced (weak references don't keep the target alive)
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
        extract_name_attribute(attr).unwrap_or_else(|| name.to_string())
    };

    let fields = match extract_named_fields(&input, "jsg_resource") {
        Ok(fields) => fields,
        Err(err) => return err,
    };

    // Generate trace statements for traceable fields
    let trace_statements = generate_trace_statements(&fields);
    let name_str = name.to_string();
    let gc_impl = quote! {
        #[automatically_derived]
        impl jsg::GarbageCollected for #name {
            fn trace(&self, visitor: &mut jsg::GcVisitor) {
                // Suppress unused warning when there are no traceable fields.
                let _ = visitor;
                #(#trace_statements)*
            }

            fn memory_name(&self) -> &'static ::std::ffi::CStr {
                // from_bytes_with_nul on a concat!(name, "\0") literal is a
                // compile-time constant expression — the compiler folds the
                // unwrap and emits a direct pointer into the read-only data
                // segment. The C++ side constructs a kj::StringPtr directly
                // from data()+size() with no allocation.
                ::std::ffi::CStr::from_bytes_with_nul(concat!(#name_str, "\0").as_bytes())
                    .unwrap()
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
            fn to_js<'a, 'b>(self, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                let r = jsg::Rc::new(self);
                r.to_js(lock)
            }
        }

        #[automatically_derived]
        impl jsg::FromJS for #name {
            type ResultType = jsg::Rc<Self>;

            fn from_js(
                lock: &mut jsg::Lock,
                value: jsg::v8::Local<jsg::v8::Value>,
            ) -> Result<Self::ResultType, jsg::Error> {
                <jsg::Rc<Self> as jsg::FromJS>::from_js(lock, value)
            }
        }

        #gc_impl
    }
    .into()
}

#[derive(Debug, Clone, Copy, PartialEq)]
enum TraceableType {
    /// `jsg::Rc<T>` — strong GC edge; visited via `GcVisitor::visit_rc`.
    Ref,
    /// `jsg::Weak<T>` — weak reference, not traced (doesn't keep the target alive).
    Weak,
    /// `jsg::v8::Global<T>` — JS value strong/traced dual-mode handle;
    /// visited via `GcVisitor::visit_global`.
    Global,
    /// Not a traceable type.
    None,
}

enum OptionalKind {
    Option,
    Nullable,
}

/// Checks if a type path matches a known JSG traceable type.
///
/// Matches `jsg::Rc<T>`, `jsg::Weak<T>`, and `jsg::v8::Global<T>`.
/// All must be fully qualified — this avoids confusion with same-named types
/// from other crates.
fn get_traceable_type(ty: &Type) -> TraceableType {
    if let Type::Path(type_path) = ty {
        let segments = &type_path.path.segments;

        // `jsg::Rc<T>` or `jsg::Weak<T>` — exactly 2 segments.
        if segments.len() == 2 && segments[0].ident == "jsg" {
            match segments[1].ident.to_string().as_str() {
                "Rc" => return TraceableType::Ref,
                "Weak" => return TraceableType::Weak,
                _ => {}
            }
        }

        // `jsg::v8::Global<T>` — exactly 3 segments.
        if segments.len() == 3
            && segments[0].ident == "jsg"
            && segments[1].ident == "v8"
            && segments[2].ident == "Global"
        {
            return TraceableType::Global;
        }
    }
    TraceableType::None
}

/// Extracts the inner type from `Option<T>` or `Nullable<T>` if present.
fn extract_optional_inner(ty: &Type) -> Option<(OptionalKind, &Type)> {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
        && let syn::PathArguments::AngleBracketed(args) = &segment.arguments
        && let Some(syn::GenericArgument::Type(inner)) = args.args.first()
    {
        let kind = match segment.ident.to_string().as_str() {
            "Option" => OptionalKind::Option,
            "Nullable" => OptionalKind::Nullable,
            _ => return None,
        };
        return Some((kind, inner));
    }
    None
}

/// Extracts the inner type `T` from `Cell<T>` or `std::cell::Cell<T>` if present.
///
/// Accepts both unqualified `Cell<T>` and fully-qualified `std::cell::Cell<T>`.
fn extract_cell_inner(ty: &Type) -> Option<&Type> {
    if let Type::Path(type_path) = ty {
        let segments = &type_path.path.segments;

        // `Cell<T>` — single segment.
        let cell_seg = if segments.len() == 1 && segments[0].ident == "Cell" {
            &segments[0]
        // `std::cell::Cell<T>` — three segments.
        } else if segments.len() == 3
            && segments[0].ident == "std"
            && segments[1].ident == "cell"
            && segments[2].ident == "Cell"
        {
            &segments[2]
        } else {
            return None;
        };

        if let syn::PathArguments::AngleBracketed(args) = &cell_seg.arguments
            && let Some(syn::GenericArgument::Type(inner)) = args.args.first()
        {
            return Some(inner);
        }
    }
    None
}

/// Generates a trace statement for a field whose type is known to be a `Cell<T>`.
///
/// Because `GarbageCollected::trace` receives `&self`, `Cell<T>` fields cannot be
/// accessed through normal Rust references (they require `&mut self` or `T: Copy`).
/// We use `Cell::as_ptr` to obtain a raw pointer and dereference it for mutable
/// access.  This is safe because:
///
/// - V8 GC tracing is always single-threaded within an isolate.
/// - `trace` is never re-entrant on the same object during a single GC cycle.
fn generate_cell_trace_statement(
    field_name: &syn::Ident,
    cell_inner_ty: &Type,
) -> Option<quote::__private::TokenStream> {
    match get_traceable_type(cell_inner_ty) {
        // Cell<jsg::Rc<T>> — strong Rc reference inside a Cell, read-only visit.
        TraceableType::Ref => {
            return Some(quote! {
                // SAFETY: trace() is single-threaded and never re-entrant.
                // We only read through the pointer.
                unsafe { visitor.visit_rc(&*self.#field_name.as_ptr()); }
            });
        }
        // Cell<jsg::v8::Global<T>> — visit_global takes &Global (safe).
        TraceableType::Global => {
            return Some(quote! {
                // SAFETY: Cell::as_ptr() dereference is sound because GC
                // tracing is single-threaded and never re-entrant on the
                // same object. visit_global itself is safe.
                unsafe { visitor.visit_global(&*self.#field_name.as_ptr()); }
            });
        }
        TraceableType::Weak | TraceableType::None => {}
    }

    // Cell<Option<jsg::Rc<T>>> or Cell<jsg::Nullable<jsg::Rc<T>>>.
    if let Some((kind, inner_ty)) = extract_optional_inner(cell_inner_ty) {
        let pattern = match kind {
            OptionalKind::Option => quote! { Some(inner) },
            OptionalKind::Nullable => quote! { jsg::Nullable::Some(inner) },
        };
        match get_traceable_type(inner_ty) {
            TraceableType::Ref => {
                return Some(quote! {
                    // SAFETY: trace() is single-threaded and never re-entrant.
                    if let #pattern = unsafe { &*self.#field_name.as_ptr() } {
                        visitor.visit_rc(inner);
                    }
                });
            }
            TraceableType::Global => {
                return Some(quote! {
                    // SAFETY: Cell::as_ptr() dereference is sound because GC
                    // tracing is single-threaded and never re-entrant on the
                    // same object. visit_global itself is safe.
                    if let #pattern = unsafe { &*self.#field_name.as_ptr() } {
                        visitor.visit_global(inner);
                    }
                });
            }
            TraceableType::Weak | TraceableType::None => {}
        }
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

            // Check if this field is wrapped in a `Cell<T>`.
            if let Some(cell_inner_ty) = extract_cell_inner(ty) {
                return generate_cell_trace_statement(field_name, cell_inner_ty);
            }

            // Check if it's Option<Traceable> or Nullable<Traceable>
            if let Some((kind, inner_ty)) = extract_optional_inner(ty) {
                let pattern = match kind {
                    OptionalKind::Option => quote! { Some(ref inner) },
                    OptionalKind::Nullable => quote! { jsg::Nullable::Some(ref inner) },
                };
                match get_traceable_type(inner_ty) {
                    // Rc<T> is a strong reference — visit it so the GC knows the edge.
                    TraceableType::Ref => {
                        return Some(quote! {
                            if let #pattern = self.#field_name {
                                visitor.visit_rc(inner);
                            }
                        });
                    }
                    // Global<T> — visit_global takes &Global (safe).
                    TraceableType::Global => {
                        return Some(quote! {
                            if let #pattern = self.#field_name {
                                visitor.visit_global(inner);
                            }
                        });
                    }
                    // Weak<T> doesn't keep the target alive and has no GC edges to trace.
                    TraceableType::Weak | TraceableType::None => {}
                }
            }

            match get_traceable_type(ty) {
                TraceableType::Ref => Some(quote! {
                    visitor.visit_rc(&self.#field_name);
                }),
                // visit_global takes &Global and handles interior mutation safely.
                TraceableType::Global => Some(quote! {
                    visitor.visit_global(&self.#field_name);
                }),
                // Weak<T> doesn't keep the target alive and has no GC edges to trace.
                TraceableType::Weak | TraceableType::None => None,
            }
        })
        .collect()
}

fn generate_resource_impl(impl_block: &ItemImpl) -> TokenStream {
    let self_ty = &impl_block.self_ty;

    if !matches!(&**self_ty, syn::Type::Path(_)) {
        return error(
            self_ty,
            "#[jsg_resource] impl blocks must use a simple path type (e.g., `impl MyResource`)",
        );
    }

    let method_registrations: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| {
            // Skip non-function items (e.g. type aliases, consts).
            let syn::ImplItem::Fn(method) = item else {
                return None;
            };

            // Only methods annotated with #[jsg_method] are registered.
            let attr = method.attrs.iter().find(|a| is_attr(a, "jsg_method"))?;

            let rust_method_name = &method.sig.ident;
            // Use explicit name from #[jsg_method(name = "...")] if provided,
            // otherwise convert snake_case to camelCase.
            let js_name = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone().into())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| snake_to_camel(&rust_method_name.to_string()));
            let callback_name = syn::Ident::new(
                &format!("{rust_method_name}_callback"),
                rust_method_name.span(),
            );

            // Methods with a receiver (&self, &mut self) become instance methods on the prototype.
            // Methods without a receiver become static methods on the constructor.
            let has_self = method
                .sig
                .inputs
                .iter()
                .any(|arg| matches!(arg, FnArg::Receiver(_)));

            let member = if has_self {
                quote! {
                    jsg::Member::Method {
                        name: #js_name.to_owned(),
                        callback: Self::#callback_name,
                    }
                }
            } else {
                quote! {
                    jsg::Member::StaticMethod {
                        name: #js_name.to_owned(),
                        callback: Self::#callback_name,
                    }
                }
            };
            Some(member)
        })
        .collect();

    let constant_registrations: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| {
            let syn::ImplItem::Const(constant) = item else {
                return None;
            };
            let attr = constant.attrs.iter().find(|a| {
                a.path().is_ident("jsg_static_constant")
                    || a.path()
                        .segments
                        .last()
                        .is_some_and(|s| s.ident == "jsg_static_constant")
            })?;

            let rust_name = &constant.ident;
            let js_name = attr
                .meta
                .require_list()
                .ok()
                .map(|list| list.tokens.clone().into())
                .and_then(extract_name_attribute)
                .unwrap_or_else(|| rust_name.to_string());

            Some(quote! {
                jsg::Member::StaticConstant {
                    name: #js_name.to_owned(),
                    value: jsg::ConstantValue::from(Self::#rust_name),
                }
            })
        })
        .collect();

    let constructor_registration = generate_constructor_registration(impl_block, self_ty);

    let constructor_vec: Vec<_> = constructor_registration.into_iter().collect();

    quote! {
        #impl_block

        #[automatically_derived]
        impl jsg::Resource for #self_ty {
            fn members() -> Vec<jsg::Member>
            where
                Self: Sized,
            {
                vec![
                    #(#constructor_vec,)*
                    #(#method_registrations,)*
                    #(#constant_registrations,)*
                ]
            }
        }
    }
    .into()
}

/// Scans an impl block for a `#[jsg_constructor]` attribute and generates the
/// constructor callback registration. Returns `None` if no constructor is defined.
/// Validates that a `#[jsg_constructor]` method has the right shape and returns
/// a compile-error token stream if it doesn't.
fn validate_constructor(method: &syn::ImplItemFn) -> Option<quote::__private::TokenStream> {
    let has_self = method
        .sig
        .inputs
        .iter()
        .any(|arg| matches!(arg, FnArg::Receiver(_)));
    if has_self {
        return Some(quote! {
            compile_error!("#[jsg_constructor] must be a static method (no self receiver)");
        });
    }

    let returns_self = matches!(&method.sig.output,
        syn::ReturnType::Type(_, ty) if matches!(&**ty,
            syn::Type::Path(p) if p.path.is_ident("Self")
        )
    );
    if !returns_self {
        return Some(quote! {
            compile_error!("#[jsg_constructor] must return Self");
        });
    }

    None
}

/// Extracts constructor argument unwrap statements and argument expressions.
fn extract_constructor_params(
    method: &syn::ImplItemFn,
) -> (
    bool,
    Vec<quote::__private::TokenStream>,
    Vec<quote::__private::TokenStream>,
) {
    let params: Vec<_> = method
        .sig
        .inputs
        .iter()
        .filter_map(|arg| {
            if let FnArg::Typed(pat_type) = arg {
                Some((*pat_type.ty).clone())
            } else {
                None
            }
        })
        .collect();

    let has_lock_param = params.first().is_some_and(is_lock_ref);
    let js_arg_offset = usize::from(has_lock_param);

    let (unwraps, arg_exprs) = params
        .iter()
        .enumerate()
        .skip(js_arg_offset)
        .map(|(i, ty)| {
            let js_index = i - js_arg_offset;
            let var = syn::Ident::new(&format!("arg{js_index}"), method.sig.ident.span());
            let unwrap = quote! {
                let #var = match <#ty as jsg::FromJS>::from_js(&mut lock, args.get(#js_index)) {
                    Ok(v) => v,
                    Err(e) => {
                        lock.throw_exception(&e);
                        return;
                    }
                };
            };
            (unwrap, quote! { #var })
        })
        .unzip();

    (has_lock_param, unwraps, arg_exprs)
}

fn generate_constructor_registration(
    impl_block: &ItemImpl,
    self_ty: &syn::Type,
) -> Option<quote::__private::TokenStream> {
    let constructors: Vec<_> = impl_block
        .items
        .iter()
        .filter_map(|item| match item {
            syn::ImplItem::Fn(m) if m.attrs.iter().any(|a| is_attr(a, "jsg_constructor")) => {
                Some(m)
            }
            _ => None,
        })
        .collect();

    if constructors.len() > 1 {
        return Some(quote! {
            compile_error!("only one #[jsg_constructor] is allowed per impl block");
        });
    }

    constructors
        .into_iter()
        .map(|method| {
            if let Some(err) = validate_constructor(method) {
                return err;
            }

            let rust_method_name = &method.sig.ident;
            let callback_name = syn::Ident::new(
                &format!("{rust_method_name}_constructor_callback"),
                rust_method_name.span(),
            );

            let (has_lock_param, unwraps, arg_exprs) = extract_constructor_params(method);
            let lock_arg = if has_lock_param {
                quote! { &mut lock, }
            } else {
                quote! {}
            };

            quote! {
                jsg::Member::Constructor {
                    callback: {
                        unsafe extern "C" fn #callback_name(
                            info: *mut jsg::v8::ffi::FunctionCallbackInfo,
                        ) {
                            let mut lock = unsafe { jsg::Lock::from_args(info) };
                            jsg::catch_panic(&mut lock, || {
                                // SAFETY: info is a valid V8 FunctionCallbackInfo from the constructor call.
                                let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(info) };
                                let mut lock = unsafe { jsg::Lock::from_args(info) };

                                #(#unwraps)*

                                let resource = #self_ty::#rust_method_name(#lock_arg #(#arg_exprs),*);
                                let rc = jsg::Rc::new(resource);
                                rc.attach_to_this(&mut args);
                            });
                        }
                        #callback_name
                    },
                }
            }
        })
        .next()
}

/// Extracts named fields from a struct, returning an empty list for unit structs.
/// Returns `Err` with a compile error for tuple structs or non-struct data.
fn extract_named_fields(
    input: &DeriveInput,
    macro_name: &str,
) -> Result<syn::punctuated::Punctuated<syn::Field, syn::token::Comma>, TokenStream> {
    match &input.data {
        Data::Struct(data) => match &data.fields {
            Fields::Named(fields) => Ok(fields.named.clone()),
            Fields::Unit => Ok(syn::punctuated::Punctuated::new()),
            Fields::Unnamed(_) => Err(error(
                input,
                &format!("#[{macro_name}] does not support tuple structs"),
            )),
        },
        _ => Err(error(
            input,
            &format!("#[{macro_name}] can only be applied to structs or impl blocks"),
        )),
    }
}

/// Checks if an attribute matches a given name, handling both unqualified (`#[jsg_method]`)
/// and qualified (`#[jsg_macros::jsg_method]`) paths.
fn is_attr(attr: &syn::Attribute, name: &str) -> bool {
    attr.path().is_ident(name) || attr.path().segments.last().is_some_and(|s| s.ident == name)
}

fn error(tokens: &impl ToTokens, msg: &str) -> TokenStream {
    syn::Error::new_spanned(tokens, msg)
        .to_compile_error()
        .into()
}

fn extract_name_attribute(tokens: TokenStream) -> Option<String> {
    let nv: syn::MetaNameValue = syn::parse(tokens).ok()?;
    if !nv.path.is_ident("name") {
        return None;
    }
    if let syn::Expr::Lit(syn::ExprLit {
        lit: syn::Lit::Str(s),
        ..
    }) = &nv.value
    {
        Some(s.value())
    } else {
        None
    }
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

/// Marks a `const` item inside a `#[jsg_resource]` impl block as a static constant
/// exposed to JavaScript on both the constructor and prototype.
///
/// The constant name is used as-is for the JavaScript property name (no camelCase
/// conversion), matching the convention that constants are `UPPER_SNAKE_CASE` in
/// both Rust and JavaScript.
///
/// Only numeric types are supported (`i8`..`i64`, `u8`..`u64`, `f32`, `f64`).
///
/// # Example
///
/// ```ignore
/// #[jsg_resource]
/// impl MyResource {
///     #[jsg_static_constant]
///     pub const MAX_SIZE: u32 = 1024;
///
///     #[jsg_static_constant]
///     pub const STATUS_OK: i32 = 0;
/// }
/// // In JavaScript: MyResource.MAX_SIZE === 1024
/// ```
#[proc_macro_attribute]
pub fn jsg_static_constant(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Marker attribute — the actual registration is handled by #[jsg_resource] on the impl block.
    item
}

/// Marks a static method as the JavaScript constructor for a `#[jsg_resource]`.
///
/// The method must be a static function (no `self` receiver) that returns `Self`.
/// When JavaScript calls `new MyResource(args)`, V8 invokes this method,
/// wraps the returned resource, and attaches it to the `this` object.
///
/// ```ignore
/// #[jsg_resource]
/// impl MyResource {
///     #[jsg_constructor]
///     fn constructor(name: String) -> Self {
///         Self { name }
///     }
/// }
/// // JS: let obj = new MyResource("hello");
/// ```
///
/// Only one `#[jsg_constructor]` is allowed per impl block.
#[proc_macro_attribute]
pub fn jsg_constructor(_attr: TokenStream, item: TokenStream) -> TokenStream {
    // Marker attribute — the actual registration is handled by #[jsg_resource] on the impl block.
    item
}

/// Returns true if the type is `&mut Lock` or `&mut jsg::Lock`.
///
/// When a method's first typed parameter matches this pattern, the macro passes the
/// callback's `lock` directly instead of extracting it from JavaScript arguments.
fn is_lock_ref(ty: &syn::Type) -> bool {
    let syn::Type::Reference(ref_type) = ty else {
        return false;
    };
    if ref_type.mutability.is_none() {
        return false;
    }
    let syn::Type::Path(type_path) = ref_type.elem.as_ref() else {
        return false;
    };
    let segments: Vec<_> = type_path.path.segments.iter().collect();
    match segments.len() {
        // `&mut Lock` — bare import (assumes `use jsg::Lock;`)
        1 => segments[0].ident == "Lock",
        // `&mut jsg::Lock` — fully qualified path
        2 => segments[0].ident == "jsg" && segments[1].ident == "Lock",
        _ => false,
    }
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
