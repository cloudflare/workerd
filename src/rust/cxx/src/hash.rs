use core::hash::BuildHasher as _;
use core::hash::Hash;

#[doc(hidden)]
pub fn hash<V: Hash>(value: &V) -> usize {
    foldhash::quality::FixedState::default().hash_one(value) as usize
}
