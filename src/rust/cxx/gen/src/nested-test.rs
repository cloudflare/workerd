use proc_macro2::Ident;
use proc_macro2::Span;
use syn::Token;
use syn::punctuated::Punctuated;
use syntax::Api;
use syntax::Doc;
use syntax::ExternType;
use syntax::ForeignName;
use syntax::Lang;
use syntax::Lifetimes;
use syntax::Pair;
use syntax::attrs::OtherAttrs;
use syntax::cfg::CfgExpr;
use syntax::namespace::Namespace;

use super::NamespaceEntries;

#[test]
fn test_ns_entries_sort() {
    let apis = &[
        make_api(None, "C"),
        make_api(None, "A"),
        make_api(Some("G"), "E"),
        make_api(Some("D"), "F"),
        make_api(Some("G"), "H"),
        make_api(Some("D::K"), "L"),
        make_api(Some("D::K"), "M"),
        make_api(None, "B"),
        make_api(Some("D"), "I"),
        make_api(Some("D"), "J"),
    ];

    let root = NamespaceEntries::new(Vec::from_iter(apis));

    // ::
    let root_direct = root.direct_content();
    assert_eq!(root_direct.len(), 3);
    assert_ident(root_direct[0], "C");
    assert_ident(root_direct[1], "A");
    assert_ident(root_direct[2], "B");

    let mut root_nested = root.nested_content();
    let (id, g) = root_nested.next().unwrap();
    assert_eq!(id, "G");
    let (id, d) = root_nested.next().unwrap();
    assert_eq!(id, "D");
    assert!(root_nested.next().is_none());

    // ::G
    let g_direct = g.direct_content();
    assert_eq!(g_direct.len(), 2);
    assert_ident(g_direct[0], "E");
    assert_ident(g_direct[1], "H");

    let mut g_nested = g.nested_content();
    assert!(g_nested.next().is_none());

    // ::D
    let d_direct = d.direct_content();
    assert_eq!(d_direct.len(), 3);
    assert_ident(d_direct[0], "F");
    assert_ident(d_direct[1], "I");
    assert_ident(d_direct[2], "J");

    let mut d_nested = d.nested_content();
    let (id, k) = d_nested.next().unwrap();
    assert_eq!(id, "K");

    // ::D::K
    let k_direct = k.direct_content();
    assert_eq!(k_direct.len(), 2);
    assert_ident(k_direct[0], "L");
    assert_ident(k_direct[1], "M");
}

fn assert_ident(api: &Api, expected: &str) {
    if let Api::CxxType(cxx_type) = api {
        assert_eq!(cxx_type.name.cxx.to_string(), expected);
    } else {
        unreachable!()
    }
}

fn make_api(ns: Option<&str>, ident: &str) -> Api {
    let ns = ns.map_or(Namespace::ROOT, |ns| syn::parse_str(ns).unwrap());
    Api::CxxType(ExternType {
        cfg: CfgExpr::Unconditional,
        lang: Lang::Rust,
        doc: Doc::new(),
        derives: Vec::new(),
        attrs: OtherAttrs::none(),
        visibility: Token![pub](Span::call_site()),
        type_token: Token![type](Span::call_site()),
        name: Pair {
            namespace: ns,
            cxx: ForeignName::parse(ident, Span::call_site()).unwrap(),
            rust: Ident::new(ident, Span::call_site()),
        },
        generics: Lifetimes {
            lt_token: None,
            lifetimes: Punctuated::new(),
            gt_token: None,
        },
        colon_token: None,
        bounds: Vec::new(),
        semi_token: Token![;](Span::call_site()),
        trusted: false,
    })
}
