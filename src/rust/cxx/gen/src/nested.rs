use proc_macro2::Ident;
use syntax::Api;
use syntax::map::UnorderedMap as Map;

use crate::namespace::namespace;

pub struct NamespaceEntries<'a> {
    direct: Vec<&'a Api>,
    nested: Vec<(&'a Ident, Self)>,
}

impl<'a> NamespaceEntries<'a> {
    pub fn new(apis: Vec<&'a Api>) -> Self {
        sort_by_inner_namespace(apis, 0)
    }

    pub fn direct_content(&self) -> &[&'a Api] {
        &self.direct
    }

    pub fn nested_content(&self) -> impl Iterator<Item = (&'a Ident, &NamespaceEntries<'a>)> {
        self.nested.iter().map(|(k, entries)| (*k, entries))
    }
}

fn sort_by_inner_namespace(apis: Vec<&Api>, depth: usize) -> NamespaceEntries<'_> {
    let mut direct = Vec::new();
    let mut nested_namespaces = Vec::new();
    let mut index_of_namespace = Map::new();

    for api in &apis {
        if let Some(first_ns_elem) = namespace(api).iter().nth(depth) {
            match index_of_namespace.get(first_ns_elem) {
                None => {
                    index_of_namespace.insert(first_ns_elem, nested_namespaces.len());
                    nested_namespaces.push((first_ns_elem, vec![*api]));
                }
                Some(&index) => nested_namespaces[index].1.push(*api),
            }
            continue;
        }
        direct.push(*api);
    }

    let nested = nested_namespaces
        .into_iter()
        .map(|(k, apis)| (k, sort_by_inner_namespace(apis, depth + 1)))
        .collect();

    NamespaceEntries { direct, nested }
}

#[cfg(test)]
#[path = "nested-test.rs"]
mod tests;
