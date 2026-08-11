use syn::Ident;
use syn::LitStr;
use syn::Token;
use syn::ext::IdentExt;
use syn::parse::Error;
use syn::parse::ParseStream;
use syn::parse::Result;

pub struct QualifiedName {
    pub segments: Vec<Ident>,
}

impl QualifiedName {
    pub fn parse_quoted(lit: &LitStr) -> Result<Self> {
        if lit.value().is_empty() {
            let segments = Vec::new();
            Ok(Self { segments })
        } else {
            lit.parse_with(|input: ParseStream| parse_unquoted(input, RawIdentifiers::Reject))
        }
    }

    pub fn parse_unquoted(input: ParseStream) -> Result<Self> {
        parse_unquoted(input, RawIdentifiers::Allow)
    }

    pub fn parse_quoted_or_unquoted(input: ParseStream) -> Result<Self> {
        if input.peek(LitStr) {
            let lit: LitStr = input.parse()?;
            Self::parse_quoted(&lit)
        } else {
            Self::parse_unquoted(input)
        }
    }
}

enum RawIdentifiers {
    Allow,
    Reject,
}

fn parse_unquoted(input: ParseStream, raw_identifiers: RawIdentifiers) -> Result<QualifiedName> {
    let mut segments = Vec::new();
    let mut trailing_punct = true;
    let leading_colons: Option<Token![::]> = input.parse()?;
    while trailing_punct && input.peek(Ident::peek_any) {
        let mut ident = Ident::parse_any(input)?;
        if let Some(unraw) = ident.to_string().strip_prefix("r#") {
            if matches!(raw_identifiers, RawIdentifiers::Reject) {
                let msg = format!(
                    "raw identifier `{}` is not allowed in a quoted namespace; use `{}`, or remove quotes",
                    ident, unraw,
                );
                return Err(Error::new(ident.span(), msg));
            }
            ident = Ident::new(unraw, ident.span());
        }
        segments.push(ident);
        let colons: Option<Token![::]> = input.parse()?;
        trailing_punct = colons.is_some();
    }
    if segments.is_empty() && leading_colons.is_none() {
        return Err(input.error("expected path"));
    } else if trailing_punct {
        return Err(input.error("expected path segment"));
    }
    Ok(QualifiedName { segments })
}
