use std::mem;

use proc_macro2::Ident;
use syn::Attribute;
use syn::LitStr;
use syn::Token;
use syn::parenthesized;
use syn::parse::Error;
use syn::parse::ParseStream;
use syn::parse::Result;
use syn::token;

#[derive(Clone)]
pub enum CfgExpr {
    Unconditional,
    Eq(Ident, Option<LitStr>),
    All(Vec<Self>),
    Any(Vec<Self>),
    Not(Box<Self>),
}

impl CfgExpr {
    pub fn merge(&mut self, expr: Self) {
        if matches!(self, Self::Unconditional) {
            *self = expr;
        } else if let Self::All(list) = self {
            list.push(expr);
        } else {
            let prev = mem::replace(self, Self::Unconditional);
            *self = Self::All(vec![prev, expr]);
        }
    }
}

pub fn parse_attribute(attr: &Attribute) -> Result<CfgExpr> {
    attr.parse_args_with(|input: ParseStream| {
        let cfg_expr = input.call(parse_single)?;
        input.parse::<Option<Token![,]>>()?;
        Ok(cfg_expr)
    })
}

fn parse_single(input: ParseStream) -> Result<CfgExpr> {
    let ident: Ident = input.parse()?;
    let lookahead = input.lookahead1();
    if input.peek(token::Paren) {
        let content;
        parenthesized!(content in input);
        if ident == "all" {
            let list = content.call(parse_multiple)?;
            Ok(CfgExpr::All(list))
        } else if ident == "any" {
            let list = content.call(parse_multiple)?;
            Ok(CfgExpr::Any(list))
        } else if ident == "not" {
            let expr = content.call(parse_single)?;
            content.parse::<Option<Token![,]>>()?;
            Ok(CfgExpr::Not(Box::new(expr)))
        } else {
            Err(Error::new(ident.span(), "unrecognized cfg expression"))
        }
    } else if lookahead.peek(Token![=]) {
        input.parse::<Token![=]>()?;
        let string: LitStr = input.parse()?;
        Ok(CfgExpr::Eq(ident, Some(string)))
    } else if lookahead.peek(Token![,]) || input.is_empty() {
        Ok(CfgExpr::Eq(ident, None))
    } else {
        Err(lookahead.error())
    }
}

fn parse_multiple(input: ParseStream) -> Result<Vec<CfgExpr>> {
    let mut vec = Vec::new();
    while !input.is_empty() {
        let expr = input.call(parse_single)?;
        vec.push(expr);
        if input.is_empty() {
            break;
        }
        input.parse::<Token![,]>()?;
    }
    Ok(vec)
}
