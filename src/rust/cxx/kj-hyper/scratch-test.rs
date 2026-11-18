use http::header::HOST;

use super::*;

#[test]
fn copies_share_a_block_that_is_reclaimed_once_they_are_gone() {
    let scratch = Scratch::default();
    let a = scratch.copy(b"text/plain");
    let b = scratch.copy(b"11");
    assert_eq!(a, &b"text/plain"[..]);
    assert_eq!(b, &b"11"[..]);
    // Cut from one block, one after the other.
    assert_eq!(a.as_ptr().wrapping_add(a.len()), b.as_ptr());
    let start = a.as_ptr();
    drop((a, b));
    // Filling the rest of the block makes the next copy reclaim it from the start.
    let mut rest = Vec::new();
    while scratch.block.borrow().capacity() >= MAX_CUT {
        rest.push(scratch.copy(&[b'x'; MAX_CUT]));
    }
    drop(rest);
    let c = scratch.copy(&[b'y'; MAX_CUT]);
    assert_eq!(c.as_ptr(), start);
}

#[test]
fn a_block_still_in_use_is_replaced_not_overwritten() {
    let scratch = Scratch::default();
    let mut held = Vec::new();
    for _ in 0..=(2 * BLOCK / MAX_CUT) {
        held.push(scratch.copy(&[b'z'; MAX_CUT]));
    }
    assert!(held.iter().all(|cut| cut.iter().all(|&byte| byte == b'z')));
}

#[test]
fn a_large_copy_is_its_own() {
    let scratch = Scratch::default();
    let large = vec![b'a'; MAX_CUT + 1];
    assert_eq!(scratch.copy(&large), &large[..]);
    assert_eq!(scratch.block.borrow().capacity(), 0);
}

#[test]
fn a_kept_header_map_comes_back_empty_with_its_room() {
    let scratch = Scratch::default();
    let mut map = http::HeaderMap::with_capacity(16);
    map.insert(HOST, http::HeaderValue::from_static("example.com"));
    let capacity = map.capacity();
    scratch.keep_headers(map);
    let map = scratch.take_headers();
    assert!(map.is_empty());
    assert_eq!(map.capacity(), capacity);
    // Taken once.
    assert_eq!(scratch.take_headers().capacity(), 0);
}

#[test]
fn a_header_map_that_grew_large_is_not_kept() {
    let scratch = Scratch::default();
    scratch.keep_headers(http::HeaderMap::with_capacity(4 * MAX_KEPT_HEADERS));
    assert_eq!(scratch.take_headers().capacity(), 0);
    scratch.keep_others(Vec::with_capacity(MAX_KEPT_HEADERS + 1));
    assert_eq!(scratch.take_others().capacity(), 0);
}

#[test]
fn a_pipe_is_reused_only_once_nothing_else_holds_it() {
    let scratch = Scratch::default();
    let pipe = scratch.take_pipe();
    let other = Arc::clone(&pipe);
    scratch.keep_pipe(pipe);
    let fresh = scratch.take_pipe();
    assert!(!Arc::ptr_eq(&fresh, &other));
    let address = Arc::as_ptr(&other);
    scratch.keep_pipe(other);
    assert_eq!(Arc::as_ptr(&scratch.take_pipe()), address);
}
