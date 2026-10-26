//! Streaming LTX encoder and decoder.

use std::collections::BTreeMap;
use std::io::Read;
use std::io::Write;

use crate::ltx::CHECKSUM_FLAG;
use crate::ltx::error::Error;
use crate::ltx::error::Result;
use crate::ltx::ltx::CHECKSUM_SIZE;
use crate::ltx::ltx::Crc64;
use crate::ltx::ltx::HEADER_SIZE;
use crate::ltx::ltx::Header;
use crate::ltx::ltx::PAGE_HEADER_FLAG_SIZE;
use crate::ltx::ltx::PAGE_HEADER_SIZE;
use crate::ltx::ltx::PageHeader;
use crate::ltx::ltx::TRAILER_SIZE;
use crate::ltx::ltx::Trailer;
use crate::ltx::ltx::checksum_page;
use crate::ltx::ltx::lock_pgno;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum DecoderState {
    Header,
    Pages,
    Close,
    Closed,
}

pub(crate) struct Decoder<R> {
    reader: R,
    state: DecoderState,
    pub(crate) header: Header,
    pub(crate) trailer: Trailer,
    hash: Crc64,
    rolling_checksum: u64,
}

impl<R: Read> Decoder<R> {
    pub(crate) fn new(reader: R) -> Self {
        Self {
            reader,
            state: DecoderState::Header,
            header: Header::default(),
            trailer: Trailer::default(),
            hash: Crc64::new(),
            rolling_checksum: 0,
        }
    }

    pub(crate) fn decode_header(&mut self) -> Result<()> {
        if self.state != DecoderState::Header {
            return Err(Error::LTXCorrupted);
        }
        let mut bytes = [0; HEADER_SIZE];
        self.reader.read_exact(&mut bytes)?;
        self.header = Header::parse(&bytes)?;
        self.header.validate()?;
        self.hash.update(&bytes);
        if !self.header.no_checksum() {
            self.rolling_checksum = CHECKSUM_FLAG;
        }
        self.state = DecoderState::Pages;
        Ok(())
    }

    pub(crate) fn decode_page(&mut self, data: &mut [u8]) -> Result<Option<PageHeader>> {
        if self.state == DecoderState::Close {
            return Ok(None);
        }
        if self.state != DecoderState::Pages || data.len() != self.header.page_size as usize {
            return Err(Error::LTXCorrupted);
        }

        let mut header_bytes = [0; PAGE_HEADER_SIZE];
        self.reader.read_exact(&mut header_bytes)?;
        let page = PageHeader::parse(&header_bytes)?;
        self.hash.update(&header_bytes);
        if page.is_zero() {
            self.state = DecoderState::Close;
            return Ok(None);
        }
        page.validate()?;

        if page.flags & PAGE_HEADER_FLAG_SIZE != 0 {
            let mut size_bytes = [0; 4];
            self.reader.read_exact(&mut size_bytes)?;
            self.hash.update(&size_bytes);
            let compressed_size = u32::from_be_bytes(size_bytes) as usize;
            if compressed_size > lz4_flex::block::get_maximum_output_size(data.len()) {
                return Err(Error::LTXCorrupted);
            }
            let mut compressed = vec![0; compressed_size];
            self.reader.read_exact(&mut compressed)?;
            let n = lz4_flex::block::decompress_into(&compressed, data)
                .map_err(|_| Error::LTXCorrupted)?;
            if n != data.len() {
                return Err(Error::LTXCorrupted);
            }
        } else {
            let mut decoder = lz4_flex::frame::FrameDecoder::new(&mut self.reader);
            decoder.read_exact(data).map_err(|_| Error::LTXCorrupted)?;
            let mut extra = [0; 1];
            if decoder.read(&mut extra).map_err(|_| Error::LTXCorrupted)? != 0 {
                return Err(Error::LTXCorrupted);
            }
        }

        self.hash.update(data);
        if self.header.is_snapshot()
            && !self.header.no_checksum()
            && page.pgno != lock_pgno(self.header.page_size)
        {
            self.rolling_checksum =
                CHECKSUM_FLAG | (self.rolling_checksum ^ checksum_page(page.pgno, data));
        }
        Ok(Some(page))
    }

    pub(crate) fn close(&mut self) -> Result<()> {
        if self.state == DecoderState::Closed {
            return Ok(());
        }
        if self.state != DecoderState::Close {
            return Err(Error::LTXCorrupted);
        }

        let mut remaining = Vec::new();
        self.reader.read_to_end(&mut remaining)?;
        if remaining.len() < 8 + TRAILER_SIZE {
            return Err(Error::LTXCorrupted);
        }

        let trailer_offset = remaining.len() - TRAILER_SIZE;
        let size_offset = trailer_offset - 8;
        let index_size = u64::from_be_bytes(
            remaining[size_offset..trailer_offset]
                .try_into()
                .map_err(|_| Error::LTXCorrupted)?,
        ) as usize;
        if index_size != size_offset {
            return Err(Error::LTXCorrupted);
        }
        verify_page_index(&remaining[..size_offset])?;

        self.trailer = Trailer::parse(&remaining[trailer_offset..])?;
        self.trailer.validate(self.header)?;
        self.hash
            .update(&remaining[..remaining.len() - CHECKSUM_SIZE]);
        if CHECKSUM_FLAG | self.hash.sum64() != self.trailer.file_checksum {
            return Err(Error::ChecksumMismatch);
        }
        if self.header.is_snapshot()
            && !self.header.no_checksum()
            && self.rolling_checksum != self.trailer.post_apply_checksum
        {
            return Err(Error::ChecksumMismatch);
        }

        self.state = DecoderState::Closed;
        Ok(())
    }
}

pub(crate) struct Encoder<W> {
    pub(crate) writer: W,
    pub(crate) header: Header,
    pub(crate) trailer: Trailer,
    hash: Crc64,
    index: BTreeMap<u32, (u64, u64)>,
    bytes_written: u64,
    previous_page_number: u32,
    header_written: bool,
    closed: bool,
}

impl<W: Write> Encoder<W> {
    pub(crate) fn new(writer: W) -> Self {
        Self {
            writer,
            header: Header::default(),
            trailer: Trailer::default(),
            hash: Crc64::new(),
            index: BTreeMap::new(),
            bytes_written: 0,
            previous_page_number: 0,
            header_written: false,
            closed: false,
        }
    }

    pub(crate) fn encode_header(&mut self, header: Header) -> Result<()> {
        if self.header_written || self.closed {
            return Err(Error::LTXCorrupted);
        }
        header.validate()?;
        self.header = header;
        let bytes = header.marshal();
        self.write_hashed(&bytes)?;
        self.header_written = true;
        Ok(())
    }

    pub(crate) fn encode_page(&mut self, page: PageHeader, data: &[u8]) -> Result<()> {
        if !self.header_written
            || self.closed
            || page.pgno > self.header.commit
            || data.len() != self.header.page_size as usize
        {
            return Err(Error::LTXCorrupted);
        }
        page.validate()?;
        let lock_page = lock_pgno(self.header.page_size);
        if page.pgno == lock_page {
            return Err(Error::LTXCorrupted);
        }

        if self.header.is_snapshot() {
            if self.previous_page_number == 0 && page.pgno != 1 {
                return Err(Error::LTXCorrupted);
            }
            let expected = if self.previous_page_number == lock_page - 1 {
                self.previous_page_number + 2
            } else {
                self.previous_page_number + 1
            };
            if self.previous_page_number != 0 && page.pgno != expected {
                return Err(Error::LTXCorrupted);
            }
        } else if self.previous_page_number >= page.pgno {
            return Err(Error::LTXCorrupted);
        }

        let offset = self.bytes_written;
        if page.flags != 0 {
            return Err(Error::LTXCorrupted);
        }
        self.write_hashed(&page.marshal())?;
        // Each page is an LZ4 frame of independent 64 KiB blocks with a
        // content checksum, as the pre-v0.5.2 Go encoder writes; the decoder
        // also reads v0.5.2's sized blocks.
        let frame_info = lz4_flex::frame::FrameInfo::new()
            .block_size(lz4_flex::frame::BlockSize::Max64KB)
            .block_mode(lz4_flex::frame::BlockMode::Independent)
            .content_checksum(true);
        let mut encoder = lz4_flex::frame::FrameEncoder::with_frame_info(frame_info, Vec::new());
        encoder.write_all(data)?;
        let compressed = encoder.finish().map_err(|_| Error::LTXCorrupted)?;
        self.writer.write_all(&compressed)?;
        self.bytes_written += compressed.len() as u64;
        self.hash.update(data);

        self.previous_page_number = page.pgno;
        self.index
            .insert(page.pgno, (offset, self.bytes_written - offset));
        Ok(())
    }

    pub(crate) fn close(&mut self, post_apply_checksum: u64) -> Result<()> {
        if !self.header_written || self.closed {
            return Err(Error::LTXCorrupted);
        }

        self.write_hashed(&[0; PAGE_HEADER_SIZE])?;
        let index_offset = self.bytes_written;
        let mut index_bytes = Vec::new();
        for (&page_number, &(offset, size)) in &self.index {
            write_uvarint(&mut index_bytes, page_number as u64);
            write_uvarint(&mut index_bytes, offset);
            write_uvarint(&mut index_bytes, size);
        }
        write_uvarint(&mut index_bytes, 0);
        self.write_hashed(&index_bytes)?;
        self.write_hashed(&(self.bytes_written - index_offset).to_be_bytes())?;

        self.trailer.post_apply_checksum = post_apply_checksum;
        self.hash.update(&post_apply_checksum.to_be_bytes());
        self.trailer.file_checksum = CHECKSUM_FLAG | self.hash.sum64();
        self.trailer.validate(self.header)?;
        // An empty database's checksum, unless the file carries none.
        if self.header.commit == 0
            && !self.header.no_checksum()
            && post_apply_checksum != CHECKSUM_FLAG
        {
            return Err(Error::LTXCorrupted);
        }
        self.writer.write_all(&self.trailer.marshal())?;
        self.bytes_written += TRAILER_SIZE as u64;
        self.closed = true;
        Ok(())
    }

    fn write_hashed(&mut self, bytes: &[u8]) -> Result<()> {
        self.writer.write_all(bytes)?;
        self.hash.update(bytes);
        self.bytes_written += bytes.len() as u64;
        Ok(())
    }
}

fn verify_page_index(bytes: &[u8]) -> Result<()> {
    decode_page_index(bytes).map(|_| ())
}

/// Decodes a page index into its `(pgno, frame_offset, frame_size)` entries.
/// The index is varint triples terminated by a `pgno == 0` marker; each entry
/// locates one page's whole frame (header plus LZ4 body) at `frame_offset` in
/// the file, `frame_size` bytes long. Paged restore builds its page map from
/// these, so it can range-read one frame without the object.
pub(crate) fn decode_page_index(bytes: &[u8]) -> Result<Vec<(u32, u64, u64)>> {
    let mut position = 0;
    let mut entries = Vec::new();
    loop {
        let page_number = read_uvarint(bytes, &mut position)?;
        if page_number == 0 {
            break;
        }
        let offset = read_uvarint(bytes, &mut position)?;
        let size = read_uvarint(bytes, &mut position)?;
        entries.push((page_number as u32, offset, size));
    }
    if position != bytes.len() {
        return Err(Error::LTXCorrupted);
    }
    Ok(entries)
}

fn read_uvarint(bytes: &[u8], position: &mut usize) -> Result<u64> {
    let mut value = 0u64;
    let mut shift = 0u32;
    loop {
        let byte = *bytes.get(*position).ok_or(Error::LTXCorrupted)?;
        *position += 1;
        if byte < 0x80 {
            if shift >= 64 || (shift == 63 && byte > 1) {
                return Err(Error::LTXCorrupted);
            }
            return Ok(value | (u64::from(byte) << shift));
        }
        value |= u64::from(byte & 0x7f) << shift;
        shift += 7;
        if shift >= 70 {
            return Err(Error::LTXCorrupted);
        }
    }
}

fn write_uvarint(bytes: &mut Vec<u8>, mut value: u64) {
    while value >= 0x80 {
        bytes.push(value as u8 | 0x80);
        value >>= 7;
    }
    bytes.push(value as u8);
}
