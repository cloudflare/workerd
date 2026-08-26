use std::sync::Mutex;
use std::sync::OnceLock;
use std::sync::PoisonError;

use syntax::set::UnorderedSet as Set;

#[derive(Copy, Clone, Default)]
pub struct InternedString(&'static str);

impl InternedString {
    pub(crate) fn str(self) -> &'static str {
        self.0
    }
}

pub fn intern(s: &str) -> InternedString {
    static INTERN: OnceLock<Mutex<Set<&'static str>>> = OnceLock::new();

    let mut set = INTERN
        .get_or_init(|| Mutex::new(Set::new()))
        .lock()
        .unwrap_or_else(PoisonError::into_inner);

    InternedString(match set.get(s) {
        Some(interned) => *interned,
        None => {
            let interned = Box::leak(Box::from(s));
            set.insert(interned);
            interned
        }
    })
}
