use quote::quote;
use syn::Abi;
use syn::Attribute;
use syn::ForeignItem;
use syn::Ident;
use syn::Item as RustItem;
use syn::ItemEnum;
use syn::ItemImpl;
use syn::ItemStruct;
use syn::ItemUse;
use syn::Token;
use syn::Visibility;
use syn::braced;
use syn::parse::Error;
use syn::parse::Parse;
use syn::parse::ParseStream;
use syn::parse::Result;
use syn::token;

use crate::cfg::CfgExpr;
use crate::namespace::Namespace;

pub struct Module {
    pub cfg: CfgExpr,
    pub namespace: Namespace,
    pub attrs: Vec<Attribute>,
    pub vis: Visibility,
    pub unsafety: Option<Token![unsafe]>,
    pub mod_token: Token![mod],
    pub ident: Ident,
    pub brace_token: token::Brace,
    pub content: Vec<Item>,
}

pub enum Item {
    Struct(ItemStruct),
    Enum(ItemEnum),
    ForeignMod(ItemForeignMod),
    Use(ItemUse),
    Impl(ItemImpl),
    Other(RustItem),
}

pub struct ItemForeignMod {
    pub attrs: Vec<Attribute>,
    pub unsafety: Option<Token![unsafe]>,
    pub abi: Abi,
    pub brace_token: token::Brace,
    pub items: Vec<ForeignItem>,
}

impl Parse for Module {
    fn parse(input: ParseStream) -> Result<Self> {
        let cfg = CfgExpr::Unconditional;
        let namespace = Namespace::ROOT;
        let mut attrs = input.call(Attribute::parse_outer)?;
        let vis: Visibility = input.parse()?;
        let unsafety: Option<Token![unsafe]> = input.parse()?;
        let mod_token: Token![mod] = input.parse()?;
        let ident: Ident = input.parse()?;

        let semi: Option<Token![;]> = input.parse()?;
        if let Some(semi) = semi {
            let span = quote!(#vis #mod_token #semi);
            return Err(Error::new_spanned(
                span,
                "#[cxx::bridge] module must have inline contents",
            ));
        }

        let content;
        let brace_token = braced!(content in input);
        attrs.extend(content.call(Attribute::parse_inner)?);

        let mut items = Vec::new();
        while !content.is_empty() {
            items.push(content.parse()?);
        }

        Ok(Self {
            cfg,
            namespace,
            attrs,
            vis,
            unsafety,
            mod_token,
            ident,
            brace_token,
            content: items,
        })
    }
}

impl Parse for Item {
    fn parse(input: ParseStream) -> Result<Self> {
        let attrs = input.call(Attribute::parse_outer)?;

        let ahead = input.fork();
        if ahead.parse::<Option<Token![unsafe]>>()?.is_some()
            && ahead.parse::<Option<Token![extern]>>()?.is_some()
        {
            let unsafety = input.parse()?;
            let abi = input.parse()?;
            let content;
            let brace_token = braced!(content in input);
            let mut items = Vec::new();
            while !content.is_empty() {
                items.push(content.parse()?);
            }
            return Ok(Self::ForeignMod(ItemForeignMod {
                attrs,
                unsafety: Some(unsafety),
                abi,
                brace_token,
                items,
            }));
        }

        let item = input.parse()?;
        match item {
            RustItem::Struct(mut item) => {
                item.attrs.splice(..0, attrs);
                Ok(Self::Struct(item))
            }
            RustItem::Enum(mut item) => {
                item.attrs.splice(..0, attrs);
                Ok(Self::Enum(item))
            }
            RustItem::ForeignMod(mut item) => {
                item.attrs.splice(..0, attrs);
                Ok(Self::ForeignMod(ItemForeignMod {
                    attrs: item.attrs,
                    unsafety: item.unsafety,
                    abi: item.abi,
                    brace_token: item.brace_token,
                    items: item.items,
                }))
            }
            RustItem::Impl(mut item) => {
                item.attrs.splice(..0, attrs);
                Ok(Self::Impl(item))
            }
            RustItem::Use(mut item) => {
                item.attrs.splice(..0, attrs);
                Ok(Self::Use(item))
            }
            other => Ok(Self::Other(other)),
        }
    }
}
