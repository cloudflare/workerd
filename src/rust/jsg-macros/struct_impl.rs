use proc_macro::TokenStream;
use quote::quote;
use syn::Data;
use syn::DeriveInput;
use syn::Fields;
use syn::parse_macro_input;

use crate::extract_name_attribute;

#[expect(clippy::needless_pass_by_value)]
pub fn jsg_struct_impl(attr: TokenStream, item: TokenStream) -> TokenStream {
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
                let #field_name = jsg::v8::ToLocalValue::to_local(&this.#field_name, lock);
                obj.set(lock, #field_name_str, #field_name);
            })
        } else {
            None
        }
    });

    let expanded = quote! {
        #input

        impl jsg::Type for #name {
            type This = Self;

            fn class_name() -> &'static str {
                #class_name
            }

            fn wrap<'a, 'b>(this: Self::This, lock: &'a mut jsg::Lock) -> jsg::v8::Local<'b, jsg::v8::Value>
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

            fn unwrap(lock: &mut jsg::Lock, value: jsg::v8::Local<jsg::v8::Value>) -> Self::This {
                todo!()
            }
        }

        impl jsg::Struct for #name {

        }
    };

    TokenStream::from(expanded)
}
