// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Rust names for Cap'n Proto names.

/// Rust's keywords, strict and reserved, in the 2024 edition.
const KEYWORDS: &[&str] = &[
    "abstract", "as", "async", "await", "become", "box", "break", "const", "continue", "crate",
    "do", "dyn", "else", "enum", "extern", "false", "final", "fn", "for", "gen", "if", "impl",
    "in", "let", "loop", "macro", "match", "mod", "move", "mut", "override", "priv", "pub", "ref",
    "return", "self", "Self", "static", "struct", "super", "trait", "true", "try", "type",
    "typeof", "unsafe", "unsized", "use", "virtual", "where", "while", "yield",
];

/// Keywords that cannot be raw identifiers.
const NOT_RAW: &[&str] = &["crate", "self", "Self", "super"];

/// Converts a camelCase or `PascalCase` name to `snake_case`. An acronym is one word: `HTTPServer`
/// becomes `http_server`.
pub fn snake_case(name: &str) -> String {
    let chars: Vec<char> = name.chars().collect();
    let mut result = String::with_capacity(name.len() + 4);
    for (index, &c) in chars.iter().enumerate() {
        if c.is_uppercase() {
            if let Some(&previous) = index.checked_sub(1).and_then(|i| chars.get(i)) {
                let next_is_lower = chars.get(index + 1).is_some_and(|next| next.is_lowercase());
                if previous.is_lowercase()
                    || previous.is_ascii_digit()
                    || (previous.is_uppercase() && next_is_lower)
                {
                    result.push('_');
                }
            }
            result.extend(c.to_lowercase());
        } else {
            result.push(c);
        }
    }
    result
}

/// Converts a camelCase or `snake_case` name to `PascalCase`.
pub fn pascal_case(name: &str) -> String {
    let mut result = String::with_capacity(name.len());
    for part in name.split('_') {
        let mut chars = part.chars();
        if let Some(first) = chars.next() {
            result.extend(first.to_uppercase());
            result.push_str(chars.as_str());
        }
    }
    result
}

/// Converts a camelCase or `PascalCase` name to `SCREAMING_SNAKE_CASE`.
pub fn screaming_snake_case(name: &str) -> String {
    snake_case(name).to_uppercase()
}

/// Makes `name` usable as an identifier: a keyword becomes a raw identifier, or gets a trailing
/// underscore if it cannot be one.
pub fn identifier(name: &str) -> String {
    if NOT_RAW.contains(&name) {
        format!("{name}_")
    } else if KEYWORDS.contains(&name) {
        format!("r#{name}")
    } else {
        name.to_owned()
    }
}

/// The name of an identifier as it appears in serde's output, without a raw identifier's `r#`.
pub fn unraw(identifier: &str) -> &str {
    identifier.strip_prefix("r#").unwrap_or(identifier)
}

#[cfg(test)]
#[path = "names-test.rs"]
mod tests;
