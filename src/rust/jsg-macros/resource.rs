use proc_macro::TokenStream;
use quote::ToTokens;
use quote::quote;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::ItemImpl;
use syn::Type;
use syn::parse_macro_input;

use crate::extract_name_attribute;
use crate::snake_to_camel_case;

#[expect(clippy::needless_pass_by_value)]
pub fn jsg_resource_impl(attr: TokenStream, item: TokenStream) -> TokenStream {
    // Try to parse as an impl block first
    if let Ok(impl_block) = syn::parse::<ItemImpl>(item.clone()) {
        return generate_resource_impl(&impl_block);
    }

    // Otherwise, parse as a struct
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

    let expanded = quote! {
        #input

        #[automatically_derived]
        impl jsg::Type for #name {
            type This = jsg::Ref<Self>;

            fn class_name() -> &'static str {
                #class_name
            }

            fn wrap<'a, 'b>(this: Self::This, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
            where
                'b: 'a,
            {
                jsg::resource::wrap::<Self>(lock, this)
            }

            fn unwrap(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Self::This {
                todo!()
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

#[derive(Debug, Clone, Copy, PartialEq)]
enum TraceableType {
    /// `WeakRef<T>` - weak reference, needs tracing but doesn't keep alive
    WeakRef,
    /// `TracedReference<T>` - trace via `visitor.trace()`
    TracedReference,
    /// `Ref<T>` - persistent reference (uses Persistent, doesn't need tracing)
    Ref,
    /// Not a traceable type
    None,
}

/// Checks if a type path matches a known traceable type.
fn get_traceable_type(ty: &Type) -> TraceableType {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
    {
        match segment.ident.to_string().as_str() {
            "WeakRef" => return TraceableType::WeakRef,
            "TracedReference" => return TraceableType::TracedReference,
            "Ref" => return TraceableType::Ref,
            _ => {}
        }
    }
    TraceableType::None
}

/// Extracts the inner type from a generic wrapper like `Option<T>` or `RefCell<T>`.
fn extract_inner_type<'a>(ty: &'a Type, wrapper_name: &str) -> Option<&'a Type> {
    if let Type::Path(type_path) = ty
        && let Some(segment) = type_path.path.segments.last()
        && segment.ident == wrapper_name
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

            // Check if it's RefCell<Option<Ref<T>>> for cyclic references
            if let Some(refcell_inner) = extract_inner_type(ty, "RefCell")
                && let Some(option_inner) = extract_inner_type(refcell_inner, "Option")
                && get_traceable_type(option_inner) == TraceableType::Ref
            {
                return Some(quote! {
                    if let Some(ref inner) = *self.#field_name.borrow() {
                        visitor.visit_ref(inner);
                    }
                });
            }

            // Check if it's Option<Traceable>
            if let Some(inner_ty) = extract_inner_type(ty, "Option") {
                match get_traceable_type(inner_ty) {
                    TraceableType::WeakRef => {
                        return Some(quote! {
                            if let Some(ref inner) = self.#field_name {
                                jsg::GarbageCollected::trace(inner, visitor);
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
                    TraceableType::Ref => {
                        return Some(quote! {
                            if let Some(ref inner) = self.#field_name {
                                visitor.visit_ref(inner);
                            }
                        });
                    }
                    TraceableType::None => {}
                }
            }

            match get_traceable_type(ty) {
                TraceableType::WeakRef => Some(quote! {
                    jsg::GarbageCollected::trace(&self.#field_name, visitor);
                }),
                TraceableType::TracedReference => Some(quote! {
                    visitor.trace(&self.#field_name);
                }),
                TraceableType::Ref => Some(quote! {
                    visitor.visit_ref(&self.#field_name);
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
    let mut method_registrations = vec![];

    for item in &impl_block.items {
        if let syn::ImplItem::Fn(method) = item {
            for attr in &method.attrs {
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

    let expanded = quote! {
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
    };

    TokenStream::from(expanded)
}
