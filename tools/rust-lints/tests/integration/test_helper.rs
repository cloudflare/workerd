pub mod unannotated_helper {
    pub fn answer() -> u32 {
        super::renamed_answer()
    }
}

fn renamed_answer() -> u32 {
    dependency::answer()
}
