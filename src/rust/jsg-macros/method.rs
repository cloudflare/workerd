use proc_macro::TokenStream;
use quote::quote;
use syn::FnArg;
use syn::ItemFn;
use syn::Type;
use syn::parse_macro_input;

pub fn jsg_method_impl(_attr: TokenStream, item: TokenStream) -> TokenStream {
    let input_fn = parse_macro_input!(item as ItemFn);
    let fn_name = &input_fn.sig.ident;
    let fn_vis = &input_fn.vis;
    let fn_sig = &input_fn.sig;
    let fn_block = &input_fn.block;
    let fn_attrs = &input_fn.attrs;

    let callback_name = syn::Ident::new(&format!("{fn_name}_callback"), fn_name.span());

    let result_ok_type = extract_result_ok_type(&fn_sig.output);

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
        #(#fn_attrs)*
        #fn_vis #fn_sig {
            #fn_block
        }

        #[automatically_derived]
        extern "C" fn #callback_name(args: *mut jsg::v8::ffi::FunctionCallbackInfo) {
            let mut lock = unsafe { jsg::Lock::from_args(args) };
            let mut args = unsafe { jsg::v8::FunctionCallbackInfo::from_ffi(args) };
            #(#unwrap_statements)*
            let this = args.this();
            let self_ = Self::unwrap(&mut lock, this);
            let result = self_.#fn_name(#(#arg_refs),*);
            unsafe { jsg::handle_result::<#result_ok_type, _>(&mut lock, &mut args, result) };
        }
    };

    TokenStream::from(expanded)
}

/// Extracts the success type `T` from a `Result<T, E>` return type.
///
/// JSG methods return `Result<T, E>`, but `handle_result::<T, _>()` needs the success type `T`
/// as an explicit generic parameter to wrap it correctly for JavaScript. This function parses
/// the return type annotation at compile time to extract that inner type.
fn extract_result_ok_type(output: &syn::ReturnType) -> quote::__private::TokenStream {
    match output {
        syn::ReturnType::Type(_, ty) => {
            // Check if it's Result<T, E> or jsg::Result<T, E>
            if let Type::Path(type_path) = ty.as_ref()
                && let Some(segment) = type_path.path.segments.last()
                && segment.ident == "Result"
                && let syn::PathArguments::AngleBracketed(args) = &segment.arguments
                && let Some(syn::GenericArgument::Type(ok_type)) = args.args.first()
            {
                return quote! { #ok_type };
            }
            // Fallback: use the whole return type (might cause compile error)
            quote! { #ty }
        }
        syn::ReturnType::Default => quote! { () },
    }
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
