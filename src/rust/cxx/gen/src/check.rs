use std::path::Component;
use std::path::Path;

use quote::quote;
use quote::quote_spanned;
use syntax::Api;
pub use syntax::check::Generator;
pub use syntax::check::typecheck;
use syntax::error;
use syntax::report::Errors;

use crate::Opt;

pub fn precheck(cx: &mut Errors, apis: &[Api], opt: &Opt) {
    if !opt.allow_dot_includes {
        check_dot_includes(cx, apis);
    }
}

fn check_dot_includes(cx: &mut Errors, apis: &[Api]) {
    for api in apis {
        if let Api::Include(include) = api {
            let first_component = Path::new(&include.path).components().next();
            if let Some(Component::CurDir | Component::ParentDir) = first_component {
                let begin = quote_spanned!(include.begin_span=> .);
                let end = quote_spanned!(include.end_span=> .);
                let span = quote!(#begin #end);
                cx.error(span, error::DOT_INCLUDE.msg);
            }
        }
    }
}
