// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

//! Prototype: moves the binary's code onto 2 MB pages.
//!
//! The executable's `.text` is ~77 MB on 4 KB pages, and a request's hot path runs through more
//! of it than the instruction TLB covers. The kernel puts file-backed code on huge pages only with
//! `CONFIG_READ_ONLY_THP_FOR_FS`, so this copies the 2 MB-aligned part of the code into anonymous
//! memory on transparent huge pages and moves that over the original mapping. Profilers then see
//! that range as anonymous memory and cannot name its functions, so it is off unless
//! `WORKERD_HUGE_TEXT` is set.

#![allow(
    unsafe_code,
    reason = "copies the process's own code into a new mapping and moves it into place"
)]

const HUGE: usize = 2 << 20;

/// Remaps the executable's code onto huge pages if `WORKERD_HUGE_TEXT` is set. Call it first
/// thing in `main()`, while the process has one thread. Does nothing on failure.
pub fn remap_text_to_huge_pages() {
    if std::env::var_os("WORKERD_HUGE_TEXT").is_none() {
        return;
    }
    let Some((start, end)) = code_range() else {
        return;
    };
    let begin = start.next_multiple_of(HUGE);
    let finish = end / HUGE * HUGE;
    if finish <= begin {
        return;
    }
    let len = finish - begin;
    // SAFETY: the process is single-threaded, the new mapping gets the same bytes as the old one
    // before it replaces it in one mremap(), and the code running here keeps running either way.
    let ok = unsafe { remap(begin, len) };
    if !ok {
        eprintln!("WORKERD_HUGE_TEXT: could not remap the code onto huge pages");
    }
}

/// The executable's own `r-xp` mapping, from /proc/self/maps.
fn code_range() -> Option<(usize, usize)> {
    let exe = std::fs::read_link("/proc/self/exe").ok()?;
    let maps = std::fs::read_to_string("/proc/self/maps").ok()?;
    for line in maps.lines() {
        let fields: Vec<&str> = line.split_whitespace().collect();
        if fields.len() >= 6
            && fields[1].starts_with("r-x")
            && std::path::Path::new(fields[5]) == exe
        {
            let (a, b) = fields[0].split_once('-')?;
            return Some((
                usize::from_str_radix(a, 16).ok()?,
                usize::from_str_radix(b, 16).ok()?,
            ));
        }
    }
    None
}

unsafe fn remap(begin: usize, len: usize) -> bool {
    // SAFETY: plain system calls on memory this function owns, then on the code range.
    unsafe {
        let raw = libc::mmap(
            std::ptr::null_mut(),
            len + HUGE,
            libc::PROT_READ | libc::PROT_WRITE,
            libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
            -1,
            0,
        );
        if raw == libc::MAP_FAILED {
            return false;
        }
        let tmp = (raw as usize).next_multiple_of(HUGE);
        if libc::madvise(tmp as *mut libc::c_void, len, libc::MADV_HUGEPAGE) != 0 {
            return false;
        }
        std::ptr::copy_nonoverlapping(begin as *const u8, tmp as *mut u8, len);
        if libc::mprotect(
            tmp as *mut libc::c_void,
            len,
            libc::PROT_READ | libc::PROT_EXEC,
        ) != 0
        {
            return false;
        }
        let moved = libc::mremap(
            tmp as *mut libc::c_void,
            len,
            len,
            libc::MREMAP_MAYMOVE | libc::MREMAP_FIXED,
            begin as *mut libc::c_void,
        );
        moved != libc::MAP_FAILED
    }
}
