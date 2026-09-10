use std::path::PathBuf;

#[derive(Debug)]
pub enum Output {
    Stdout,
    File(PathBuf),
}

impl Output {
    pub fn ends_with(&self, suffix: &str) -> bool {
        match self {
            Self::Stdout => false,
            Self::File(path) => path.to_string_lossy().ends_with(suffix),
        }
    }
}
