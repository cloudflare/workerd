// Production declarations remain required beside test-only inner cfg attributes.
mod production {
    mod nested {}
}

mod inline_test {
    #![cfg(test)]
    mod nested {}
}

#[path = "modules/test_inner.rs"]
mod external_test;
