use std::{
    fs::File,
    io::{BufReader, BufWriter, Write},
};

use anyhow::Result;
use serde::{Deserialize, Serialize};

/// Contains the declarations we care about
#[derive(Deserialize, PartialEq)]
enum Clang {
    // Disambiguous -- nodes that help disambiguate functions
    NamespaceDecl { name: Option<String> },
    RecordType { name: Option<String> },

    // Function-like -- direct parents of parameters
    FunctionDecl { name: Option<String> },
    CXXMethodDecl { name: Option<String> },
    CXXConstructorDecl,

    // Parameter names
    ParmVarDecl { name: Option<String> },

    // Everything else
    Other { name: Option<String> },
}

impl Clang {
    fn is_disambiguous(&self) -> bool {
        matches!(*self, Self::NamespaceDecl { .. } | Self::RecordType { .. })
    }

    fn is_function_like(&self) -> bool {
        matches!(
            *self,
            Self::FunctionDecl { .. } | Self::CXXMethodDecl { .. } | Self::CXXConstructorDecl
        )
    }

    fn name(&self) -> Option<&str> {
        match *self {
            Clang::NamespaceDecl { ref name } => name.as_ref().map(|s| s.as_ref()),
            Clang::RecordType { ref name } => name.as_ref().map(|s| s.as_ref()),
            Clang::FunctionDecl { ref name } => name.as_ref().map(|s| s.as_ref()),
            Clang::CXXMethodDecl { ref name } => name.as_ref().map(|s| s.as_ref()),
            Clang::CXXConstructorDecl => Some("constructor"),
            Clang::ParmVarDecl { ref name } => name.as_ref().map(|s| s.as_ref()),
            Clang::Other { ref name } => name.as_ref().map(|s| s.as_ref()),
        }
    }

    // fn name(&self) -> Option<&str> {
    //     match *self {
    //         Self::Other { ref name } => name.as_ref().map(|s| s.as_ref())
    //         Self::ParmVarDecl { ref name } => name.as_ref().map(|s| s.as_ref())
    //     }
    // }
}

type ClangNode = clang_ast::Node<Clang>;

fn main() -> Result<()> {
    let mut args = pico_args::Arguments::from_env();

    let clang_ast = args.value_from_os_str::<_, _, anyhow::Error>("--input", |path| {
        let file = File::open(path)?;
        let rdr = BufReader::new(file);
        Ok(serde_json::from_reader(rdr)?)
    })?;

    let value = get_parameter_names(clang_ast);

    let mut writer = args.value_from_os_str::<_, _, anyhow::Error>("--output", |path| {
        let file = File::create(path)?;
        Ok(BufWriter::new(file))
    })?;

    serde_json::to_writer(&mut writer, &value)?;
    writer.flush()?;

    Ok(())
}

// #[derive(Debug, Serialize)]
// #[serde(transparent)]
// struct ParameterName(String);

// #[derive(Debug, Serialize, Hash)]
// #[serde(transparent)]
// struct FunctionLike(Vec<ParameterName>);

// #[derive(Debug, Serialize)]
// #[serde(transparent)]
// struct Disambiguous(HashMap<String, ParameterNameMapNode>);

// #[derive(Debug, Serialize)]
// enum ParameterNameMapNode {
//     ParameterName(ParameterName),
//     FunctionLike(FunctionLike),
//     Disambiguous(Disambiguous),
// }

fn get_parameter_names(clang_ast: ClangNode) -> Vec<Parameter> {
    clang_ast
        .inner
        .into_iter()
        .filter(|node| {
            node.kind
                == Clang::NamespaceDecl {
                    name: Some("workerd".to_owned()),
                }
        })
        .flat_map(|node| traverse_disambiguous(node, None))
        .collect()
}

#[derive(Serialize)]
struct Parameter {
    fully_qualified_parent_name: String,
    function_like: String,
    index: usize,
    name: String,
}

fn traverse_disambiguous(
    disambiguous: ClangNode,
    fully_qualified_parent_name: Option<String>,
) -> Vec<Parameter> {
    let disambiguous_name = disambiguous.kind.name().map(ToOwned::to_owned);
    disambiguous
        .inner
        .into_iter()
        .flat_map(|node| {
            if node.kind.is_disambiguous() {
                let fully_qualified_parent_name = match &fully_qualified_parent_name {
                    Some(name) => {
                        format!("{name}::{}", disambiguous_name.clone().unwrap_or_default())
                    }
                    None => disambiguous_name
                        .as_ref()
                        .map(ToOwned::to_owned)
                        .unwrap_or_default(),
                };
                traverse_disambiguous(node, Some(fully_qualified_parent_name.clone()))
            } else if node.kind.is_function_like() {
                traverse_function_like(
                    node,
                    fully_qualified_parent_name.clone().unwrap_or_default(),
                )
            } else if !node.inner.is_empty() {
                let fully_qualified_parent_name = match &fully_qualified_parent_name {
                    Some(name) => {
                        format!("{name}::{}", disambiguous_name.clone().unwrap_or_default())
                    }
                    None => disambiguous_name
                        .as_ref()
                        .map(ToOwned::to_owned)
                        .unwrap_or_default(),
                };
                node.inner
                    .into_iter()
                    .flat_map(|child| {
                        traverse_disambiguous(child, Some(fully_qualified_parent_name.clone()))
                    })
                    .collect::<Vec<_>>()
            } else {
                vec![]
            }
        })
        .collect()
}

fn traverse_function_like(node: ClangNode, fully_qualified_parent_name: String) -> Vec<Parameter> {
    let function_like_name = node.kind.name().unwrap().to_owned();

    node.inner
        .into_iter()
        .filter_map(|child| {
            if let Clang::ParmVarDecl { name } = child.kind {
                Some(name.unwrap_or_else(|| String::from("unnamed")))
            } else {
                None
            }
        })
        .enumerate()
        .map(|(i, param_name)| Parameter {
            fully_qualified_parent_name: fully_qualified_parent_name.clone(),
            function_like: function_like_name.clone(),
            index: i,
            name: param_name,
        })
        .collect()
}
