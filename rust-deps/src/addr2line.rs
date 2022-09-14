use addr2line::gimli::{self, EndianSlice, RunTimeEndian};
use addr2line::Context;
use object::{self, SymbolMap, SymbolMapName};
use object::Object;
use object::ObjectSection;
use ffi::SourceFrame;
use std::fs::File;
use typed_arena::Arena;
use std::borrow::Cow;
use std::os::unix::io::AsRawFd;
use std::mem::ManuallyDrop;

#[cxx::bridge(namespace = "workerd::addr2line")]
mod ffi {
    struct SourceFrame {
        filename: String,
        line: u32,
        function: String,
    }

    extern "Rust" {
        type Module;

        fn load(module: &str) -> Result<Box<Module>>;
        fn symbolicate(module: &Module, addresses: &[u64]) -> Result<Vec<SourceFrame>>;
        fn symbolicate_first(module: &Module, addresses: &[u64]) -> Result<SourceFrame>;
    }
}

fn load_file_section<'input, 'arena, Endian: gimli::Endianity>(
    id: gimli::SectionId,
    file: &object::File<'input>,
    endian: Endian,
    arena_data: &'arena Arena<Cow<'input, [u8]>>,
) -> Result<gimli::EndianSlice<'arena, Endian>, object::read::Error> {
    let name = id.name();
    match file.section_by_name(name) {
        Some(section) => match section.uncompressed_data()? {
            Cow::Borrowed(b) => Ok(gimli::EndianSlice::new(b, endian)),
            Cow::Owned(b) => Ok(gimli::EndianSlice::new(arena_data.alloc(b.into()), endian)),
        },
        None => Ok(gimli::EndianSlice::new(&[], endian)),
    }
}

fn mmap_entire_file_read_only(file: File) -> std::io::Result<(*const u8, usize)> {
    let len = file.metadata()?.len();
    let fd = file.as_raw_fd();

    if len == 0 {
        // Normally the OS would catch this, but it segfaults under QEMU.
        return Err(std::io::Error::new(
            std::io::ErrorKind::InvalidInput,
            "memory map must have a non-zero length",
        ));
    }

    unsafe {
        let ptr = libc::mmap(
            std::ptr::null_mut(),
            len as libc::size_t,
            libc::PROT_READ,
            libc::MAP_SHARED,
            fd,
            0 as libc::off_t,
        );

        if ptr == libc::MAP_FAILED {
            Err(std::io::Error::last_os_error())
        } else {
            Ok((ptr as *const u8, len as usize))
        }
    }
}

pub struct Module {
    arena_data: *mut Arena<Cow<'static, [u8]>>,
    mmap_ptr: *const u8,
    mmap_len: usize,
    object: ManuallyDrop<object::File<'static>>,
    symbols: ManuallyDrop<SymbolMap<SymbolMapName<'static>>>,
    context: ManuallyDrop<Context<EndianSlice<'static, RunTimeEndian>>>,
}

impl Drop for Module {
    fn drop(&mut self) {
        unsafe {
            ManuallyDrop::drop(&mut self.context);
            ManuallyDrop::drop(&mut self.symbols);
            ManuallyDrop::drop(&mut self.object);
            drop(Box::from_raw(self.arena_data));
            assert!(
                libc::munmap(
                    self.mmap_ptr as *mut libc::c_void,
                    self.mmap_len as libc::size_t
                ) == 0,
                "unable to unmap mmap: {}",
                std::io::Error::last_os_error()
            );
        }
    }
}

pub fn load(module: &str) -> anyhow::Result<Box<Module>> {
    let file = File::open(module)?;

    let (mmap_ptr, mmap_len) = mmap_entire_file_read_only(file)?;
    let arena = Box::leak(Box::new(Arena::new())) as *mut _;
    let arena_ref: &'static _ = unsafe { &*arena };

    // mmap lasts until module is dropped
    let mmap_ref: &'static [u8] = unsafe { std::slice::from_raw_parts(mmap_ptr, mmap_len) };

    let object = object::File::parse(mmap_ref)?;
    let endian = if object.is_little_endian() {
        gimli::RunTimeEndian::Little
    } else {
        gimli::RunTimeEndian::Big
    };

    let symbols = object.symbol_map();

    let dwarf = gimli::Dwarf::load(
        |id| load_file_section(id, &object, endian, arena_ref),
        |_| Ok(gimli::EndianSlice::new(&[], endian))
    )?;

    let context = Context::from_dwarf(dwarf)?;

    Ok(Box::new(Module {
        arena_data: arena,
        mmap_ptr: mmap_ptr,
        mmap_len: mmap_len,
        object: ManuallyDrop::new(object),
        symbols: ManuallyDrop::new(symbols),
        context: ManuallyDrop::new(context),
    }))
}

pub fn symbolicate(module: &Module, addresses: &[u64]) -> anyhow::Result<Vec<SourceFrame>> {
    let symbols = &module.symbols;
    let context = &module.context;

    let mut src_frames = Vec::new();

    for &addr in addresses {
        let mut found_something = false;
        if let Ok(mut frames) = context.find_frames(addr) {
            while let Some(frame) = frames.next().unwrap_or(None) {
                found_something = true;
                let func = frame
                    .function
                    .and_then(|f| f.demangle().ok().map(|s| s.into()))
                    .unwrap_or_else(|| "??".to_owned());
                let (line, file) = frame
                    .location
                    .map(|l| (l.line, l.file))
                    .unwrap_or_else(|| (None, None));
                src_frames.push(SourceFrame {
                    function: func,
                    line: line.unwrap_or(0),
                    filename: file.unwrap_or("??").to_owned(),
                });
            }
        }

        if !found_something {
            let loc = context.find_location(addr);
            let (line, file) = loc
                .unwrap_or(None)
                .map(|loc| (loc.line.unwrap_or(0), loc.file.unwrap_or("??")))
                .unwrap_or((0, "??"));
            let function = symbols
                .get(addr)
                .map(|x| x.name())
                .unwrap_or("??")
                .to_owned();
            src_frames.push(SourceFrame {
                function: function,
                line: line,
                filename: file.to_owned(),
            });
        }
    }

    Ok(src_frames)
}

pub fn symbolicate_first(module: &Module, addresses: &[u64]) -> anyhow::Result<SourceFrame> {
    // This returns the source frame for just one address. Unlike `symbolicate`, we try to
    // symbolicate just the first address that succeeds in some non-trivial way. This means that
    // we skip frames that fail to resolve a location/function.
    let symbols = &module.symbols;
    let context = &module.context;

    for &addr in addresses {
        if let Ok(mut frames) = context.find_frames(addr) {
            while let Some(frame) = frames.next().unwrap_or(None) {
                let func = frame
                    .function
                    .and_then(|f| f.demangle().ok().map(|s| s.into()));
                let (line, file) = frame
                    .location
                    .map(|l| (l.line, l.file))
                    .unwrap_or_else(|| (None, None));
                if func.is_some() || (file.is_some() && line.is_some()) {
                    return Ok(SourceFrame {
                        function: func.unwrap_or_else(|| "??".to_owned()),
                        line: line.unwrap_or(0),
                        filename: file.unwrap_or("??").to_owned(),
                    })
                }
            }
        }
    }

    // Realistically I don't think we'd ever end up hitting this for loop below without getting
    // to the error state since the above will succeed if we get the location of the address.
    // This is here purely as a defensive sanity check.
    for &addr in addresses {
        let loc = context.find_location(addr);
        let (line, file) = loc
            .unwrap_or(None)
            .map(|loc| (loc.line, loc.file))
            .unwrap_or((None, None));
        let function = symbols
            .get(addr)
            .map(|x| x.name());
        if function.is_some() || file.is_some() {
            return Ok(SourceFrame {
                function: function.unwrap_or("??").to_owned(),
                line: line.unwrap_or(0),
                filename: file.unwrap_or("??").to_owned(),
            });
        }
    }

    Err(anyhow::Error::new(std::io::Error::new(std::io::ErrorKind::NotFound, "Couldn't symbolicate")))
}
