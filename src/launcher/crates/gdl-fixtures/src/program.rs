//! Launcher files for the tests of the sections reader. A launcher file is its program, then the
//! bytes of each section, then a table of JSON that lists them, padded with spaces, then a trailer
//! of 48 bytes: the table's length as 8 little-endian bytes, its SHA-256 digest and
//! [`TRAILER_MAGIC`]. A signed Windows launcher carries its certificate table after all of that.
//! Here are the smallest PE and ELF programs whose headers say where they end, a stand-in for a
//! signature, the writer of sections and their table, and a file far larger than memory that
//! holds a few chunks and zeros elsewhere.

use std::io::{self, Read, Seek, SeekFrom};

use serde_json::{Value, json};
use sha2::{Digest, Sha256};

use crate::runtime::hex;
use crate::zip::{put_u16, put_u32, put_u64};

/// The last 8 bytes of a trailer, by which a reader knows that a file carries sections.
pub const TRAILER_MAGIC: [u8; 8] = *b"GDLSECT1";

/// The length of the certificate table [`fake_sign`] appends: one entry, whose own fields take 8
/// bytes, holding 16 bytes in place of a signature.
pub const FAKE_CERTIFICATE_LEN: u64 = 24;

/// The schema of the tables written here.
const TABLE_SCHEMA: u64 = 1;

/// The length of a trailer: the table's length in 8 bytes, its SHA-256 digest in 32, then the
/// magic.
const TRAILER_LEN: u64 = 48;

/// What pads a table: spaces, which JSON allows after a value.
const TABLE_PADDING: u8 = b' ';

/// What a PE's certificate table starts at a multiple of. A launcher file as long as a multiple
/// of it takes a signature with nothing between its trailer and the certificate table.
const CERTIFICATE_ALIGNMENT: u64 = 8;

/// The length of the DOS header that begins every PE, whose last field says where the PE's own
/// headers start.
const DOS_HEADER_LEN: usize = 64;

/// The magic a DOS header begins with.
const IMAGE_DOS_SIGNATURE: [u8; 2] = *b"MZ";

/// Where a DOS header keeps the offset of the PE signature, its `e_lfanew`.
const E_LFANEW_AT: usize = 0x3C;

/// The signature that begins a PE's own headers.
const IMAGE_NT_SIGNATURE: [u8; 4] = *b"PE\0\0";

/// The machine of an x86-64 program.
const IMAGE_FILE_MACHINE_AMD64: u16 = 0x8664;

/// The number of sections the tiny PE has.
const PE_SECTION_COUNT: u16 = 1;

/// Where the COFF header keeps the length of the optional header, counted from the PE signature.
const SIZE_OF_OPTIONAL_HEADER_AT: usize = 20;

/// Where the optional header begins, counted from the PE signature: after the signature's 4 bytes
/// and the COFF header's 20.
const OPTIONAL_HEADER_AT: usize = 24;

/// The magic of a PE32+ optional header, the one 64-bit programs have.
const IMAGE_NT_OPTIONAL_HDR64_MAGIC: u16 = 0x20B;

/// The length of a PE32+ optional header with all its data directories.
const PE32_PLUS_OPTIONAL_HEADER_LEN: u16 = 240;

/// Where a PE32+ optional header keeps the alignment of sections in memory, then that of their
/// raw bytes in the file.
const SECTION_ALIGNMENT_AT: usize = 32;

/// Where a PE32+ optional header keeps the size of the image in memory, then that of all the
/// headers.
const SIZE_OF_IMAGE_AT: usize = 56;

/// Where a PE32+ optional header keeps the number of its data directories.
const NUMBER_OF_RVA_AND_SIZES_AT: usize = 108;

/// Where a PE32+ optional header's data directories begin.
const DATA_DIRECTORIES_AT: usize = 112;

/// The number of data directories the PE format defines.
const IMAGE_NUMBEROF_DIRECTORY_ENTRIES: u32 = 16;

/// The length of a data directory: an address, then a size, 4 bytes each.
const DATA_DIRECTORY_LEN: usize = 8;

/// The index of the certificate table's data directory, whose address is a file offset.
const IMAGE_DIRECTORY_ENTRY_SECURITY: usize = 4;

/// The size of the tiny PE's headers, where the bytes of its section begin.
const PE_HEADERS_LEN: u32 = 0x200;

/// What the tiny PE's sections are aligned to in memory: a page.
const PE_SECTION_ALIGNMENT: u32 = 0x1000;

/// What the tiny PE's headers and the raw bytes of its sections are aligned to in the file, the
/// least the PE format allows.
const PE_FILE_ALIGNMENT: u32 = 0x200;

/// The name of the tiny PE's section, padded with zeros to 8 bytes.
const PE_SECTION_NAME: [u8; 8] = *b".text\0\0\0";

/// The length of the tiny PE's section, in the file and in memory.
const PE_SECTION_LEN: u32 = 0x200;

/// Where the tiny PE's section lies in memory, relative to the image: the first page after the
/// headers'.
const PE_SECTION_ADDRESS: u32 = PE_SECTION_ALIGNMENT;

/// The size of the tiny PE's image in memory: its headers' page and its section's.
const PE_IMAGE_SIZE: u32 = PE_SECTION_ADDRESS + PE_SECTION_ALIGNMENT;

/// The length of the tiny PE: its headers, then its section.
const PE_LEN: usize = (PE_HEADERS_LEN + PE_SECTION_LEN) as usize;

/// The revision of the certificate entries an Authenticode signature is made of: 2.0.
const WIN_CERT_REVISION_2_0: u16 = 0x0200;

/// The type of a certificate entry that holds PKCS#7 signed data, as a signature's does.
const WIN_CERT_TYPE_PKCS_SIGNED_DATA: u16 = 0x0002;

/// The length of a certificate entry's own fields: its length, its revision and its type.
const CERTIFICATE_HEADER_LEN: u64 = 8;

/// What the fake signature is made of: 0x30, the DER tag of a SEQUENCE, which PKCS#7 signed data
/// begins with.
const FAKE_SIGNATURE_BYTE: u8 = 0x30;

/// The magic every ELF file begins with.
const ELFMAG: [u8; 4] = *b"\x7fELF";

/// The class of a 64-bit ELF file.
const ELFCLASS64: u8 = 2;

/// The data encoding of a little-endian ELF file.
const ELFDATA2LSB: u8 = 1;

/// The one version of ELF there is, in the identification bytes and in the header.
const EV_CURRENT: u8 = 1;

/// The length of the identification bytes an ELF header begins with.
const EI_NIDENT: usize = 16;

/// The type of an executable file.
const ET_EXEC: u16 = 2;

/// The machine of an x86-64 program.
const EM_X86_64: u16 = 62;

/// The length of a 64-bit ELF header.
const ELF_HEADER_LEN: u16 = 64;

/// The length of a 64-bit program header.
const PROGRAM_HEADER_LEN: u16 = 56;

/// The number of program headers the tiny ELF has.
const PROGRAM_HEADER_COUNT: u16 = 1;

/// The length of a 64-bit section header.
const ELF_SECTION_HEADER_LEN: u16 = 64;

/// The type of a program header that loads a segment.
const PT_LOAD: u32 = 1;

/// The flag of a segment that may be executed.
const PF_X: u32 = 1;

/// The flag of a segment that may be read.
const PF_R: u32 = 4;

/// Where the linkers of x86-64 Linux place an executable that is not position-independent.
const LOAD_ADDRESS: u64 = 0x40_0000;

/// The size of the pages the tiny ELF's segment is aligned to.
const PAGE_LEN: u64 = 0x1000;

/// The bytes the tiny ELF's segment loads, from the start of the file.
const LOADED_LEN: u64 = 0x200;

/// Where the tiny ELF's section headers begin: right after the bytes it loads.
const ELF_SECTION_HEADERS_AT: u64 = 0x200;

/// The number of the tiny ELF's sections: the null section every ELF file begins with, and the
/// section names.
const ELF_SECTION_COUNT: u16 = 2;

/// The index of the section that holds the section names.
const ELF_SECTION_NAMES_INDEX: u16 = 1;

/// The type of a section that holds strings.
const SHT_STRTAB: u32 = 3;

/// The section names: the null section's, which is empty, then the name of the section that holds
/// them.
const ELF_SECTION_NAMES: &[u8] = b"\0.shstrtab\0";

/// Where the name of the section that holds the section names begins among them.
const ELF_SECTION_NAMES_NAME: u32 = 1;

/// The alignment of the section names, which are bytes.
const ELF_SECTION_NAMES_ALIGNMENT: u64 = 1;

/// The length of the tiny ELF: the bytes it loads, then its section headers.
const ELF_LEN: usize =
    ELF_SECTION_HEADERS_AT as usize + ELF_SECTION_COUNT as usize * ELF_SECTION_HEADER_LEN as usize;

/// The smallest x86-64 Windows program whose headers say where it ends: a PE32+ image of 0x400
/// bytes, 0x200 of headers, then the raw bytes of its one section, `.text`, which end the image.
/// It has no COFF symbol table, and its certificate table's data directory is empty. Only the
/// fields that say what it is, where its headers and its section lie in the file and in memory,
/// what those are aligned to and how many data directories it has are set; every other byte is
/// zero, so it has no entry point and is never run.
pub fn tiny_pe() -> Vec<u8> {
    let mut pe = Vec::with_capacity(PE_LEN);
    // The DOS header, whose last field puts the PE's own headers right after it.
    pe.extend_from_slice(&IMAGE_DOS_SIGNATURE);
    pe.resize(E_LFANEW_AT, 0);
    put_u32(&mut pe, DOS_HEADER_LEN as u32);
    // The PE signature and the COFF header: an x86-64 program of one section, with no COFF
    // symbol table, whose optional header is a PE32+ one.
    pe.extend_from_slice(&IMAGE_NT_SIGNATURE);
    put_u16(&mut pe, IMAGE_FILE_MACHINE_AMD64);
    put_u16(&mut pe, PE_SECTION_COUNT);
    put_u32(&mut pe, 0); // TimeDateStamp
    put_u32(&mut pe, 0); // PointerToSymbolTable
    put_u32(&mut pe, 0); // NumberOfSymbols
    put_u16(&mut pe, PE32_PLUS_OPTIONAL_HEADER_LEN);
    put_u16(&mut pe, 0); // Characteristics
    // The optional header: its magic, the alignments, the sizes of the image and of the headers,
    // and the number of data directories, every one of which is empty.
    let optional = pe.len();
    put_u16(&mut pe, IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    pe.resize(optional + SECTION_ALIGNMENT_AT, 0);
    put_u32(&mut pe, PE_SECTION_ALIGNMENT);
    put_u32(&mut pe, PE_FILE_ALIGNMENT);
    pe.resize(optional + SIZE_OF_IMAGE_AT, 0);
    put_u32(&mut pe, PE_IMAGE_SIZE);
    put_u32(&mut pe, PE_HEADERS_LEN);
    pe.resize(optional + NUMBER_OF_RVA_AND_SIZES_AT, 0);
    put_u32(&mut pe, IMAGE_NUMBEROF_DIRECTORY_ENTRIES);
    pe.resize(optional + usize::from(PE32_PLUS_OPTIONAL_HEADER_LEN), 0);
    // The section's header: its name, where it lies in memory, then where its raw bytes lie in
    // the file, right after the headers.
    pe.extend_from_slice(&PE_SECTION_NAME);
    put_u32(&mut pe, PE_SECTION_LEN); // VirtualSize
    put_u32(&mut pe, PE_SECTION_ADDRESS); // VirtualAddress
    put_u32(&mut pe, PE_SECTION_LEN); // SizeOfRawData
    put_u32(&mut pe, PE_HEADERS_LEN); // PointerToRawData
    // The rest of the section's header, the padding of the headers and the section's bytes.
    pe.resize(PE_LEN, 0);
    pe
}

/// Lays a signature out on `pe` as an Authenticode signer does, with nothing signed: zero bytes up
/// to a multiple of 8, then a certificate table of one entry, [`FAKE_CERTIFICATE_LEN`] bytes long
/// and of the revision and type a signature's has, which ends the file, with the certificate
/// table's data directory set to it. Returns the number of zero bytes put before the table. A
/// signer also rewrites the checksum in the headers, which no reader of sections looks at. Panics
/// unless `pe` is a PE32+ program with that data directory.
pub fn fake_sign(pe: &mut Vec<u8>) -> u64 {
    let directory = certificate_directory(pe);
    let unsigned = pe.len();
    pe.resize(unsigned.next_multiple_of(CERTIFICATE_ALIGNMENT as usize), 0);
    let padding = pe.len() - unsigned;
    let offset = u32::try_from(pe.len())
        .expect("the certificate table's data directory addresses it in 32 bits");
    put_u32(pe, FAKE_CERTIFICATE_LEN as u32);
    put_u16(pe, WIN_CERT_REVISION_2_0);
    put_u16(pe, WIN_CERT_TYPE_PKCS_SIGNED_DATA);
    let signature = (FAKE_CERTIFICATE_LEN - CERTIFICATE_HEADER_LEN) as usize;
    pe.resize(pe.len() + signature, FAKE_SIGNATURE_BYTE);
    // The directory's address, then its size.
    set_u32(pe, directory, offset);
    set_u32(
        pe,
        directory + size_of::<u32>(),
        FAKE_CERTIFICATE_LEN as u32,
    );
    padding as u64
}

/// Where `pe` keeps its certificate table's data directory, found through its headers as a signer
/// finds it. Panics unless `pe` is a PE32+ program with that directory.
fn certificate_directory(pe: &[u8]) -> usize {
    assert_eq!(
        header_field(pe, 0),
        IMAGE_DOS_SIGNATURE,
        "a PE begins with a DOS header"
    );
    let headers = u32::from_le_bytes(header_field(pe, E_LFANEW_AT)) as usize;
    assert_eq!(
        header_field(pe, headers),
        IMAGE_NT_SIGNATURE,
        "the PE signature lies where the DOS header says"
    );
    let optional = headers + OPTIONAL_HEADER_AT;
    let optional_len = u16::from_le_bytes(header_field(pe, headers + SIZE_OF_OPTIONAL_HEADER_AT));
    let magic = u16::from_le_bytes(header_field(pe, optional));
    assert_eq!(
        magic, IMAGE_NT_OPTIONAL_HDR64_MAGIC,
        "only a PE32+ program is signed here"
    );
    let directories = u32::from_le_bytes(header_field(pe, optional + NUMBER_OF_RVA_AND_SIZES_AT));
    let directory = DATA_DIRECTORIES_AT + IMAGE_DIRECTORY_ENTRY_SECURITY * DATA_DIRECTORY_LEN;
    assert!(
        directories as usize > IMAGE_DIRECTORY_ENTRY_SECURITY
            && directory + DATA_DIRECTORY_LEN <= usize::from(optional_len),
        "this PE has no data directory for a certificate table"
    );
    optional + directory
}

/// The `N` bytes at `at` of a program's headers. Panics when they run past the program.
fn header_field<const N: usize>(program: &[u8], at: usize) -> [u8; N] {
    program
        .get(at..at + N)
        .and_then(|field| field.try_into().ok())
        .unwrap_or_else(|| panic!("the headers run past the program's {} bytes", program.len()))
}

/// Overwrites the little-endian field of 32 bits at `at`.
fn set_u32(bytes: &mut [u8], at: usize, value: u32) {
    bytes[at..at + size_of::<u32>()].copy_from_slice(&value.to_le_bytes());
}

/// The smallest x86-64 Linux program whose headers say where it ends: a 64-bit little-endian ELF
/// executable of 0x280 bytes, with one segment that loads its first 0x200 bytes, then the headers
/// of its two sections, which end the file: the null section, and the section names, which lie
/// just before them. It has no entry point and is never run.
pub fn tiny_elf() -> Vec<u8> {
    let mut elf = Vec::with_capacity(ELF_LEN);
    // The identification bytes: the magic, a 64-bit little-endian file of the one version, for
    // the System V ABI, whose number is 0.
    elf.extend_from_slice(&ELFMAG);
    elf.extend_from_slice(&[ELFCLASS64, ELFDATA2LSB, EV_CURRENT]);
    elf.resize(EI_NIDENT, 0);
    // The rest of the ELF header: an x86-64 executable with no entry point, its program header
    // right after this header, its section headers after the bytes it loads.
    put_u16(&mut elf, ET_EXEC);
    put_u16(&mut elf, EM_X86_64);
    put_u32(&mut elf, u32::from(EV_CURRENT));
    put_u64(&mut elf, 0); // e_entry
    put_u64(&mut elf, u64::from(ELF_HEADER_LEN)); // e_phoff
    put_u64(&mut elf, ELF_SECTION_HEADERS_AT); // e_shoff
    put_u32(&mut elf, 0); // e_flags: x86-64 defines none
    put_u16(&mut elf, ELF_HEADER_LEN);
    put_u16(&mut elf, PROGRAM_HEADER_LEN);
    put_u16(&mut elf, PROGRAM_HEADER_COUNT);
    put_u16(&mut elf, ELF_SECTION_HEADER_LEN);
    put_u16(&mut elf, ELF_SECTION_COUNT);
    put_u16(&mut elf, ELF_SECTION_NAMES_INDEX);
    // The one program header: a segment that loads the file's first bytes, to be read and
    // executed, at an address page-aligned as its offset is.
    put_u32(&mut elf, PT_LOAD);
    put_u32(&mut elf, PF_R | PF_X);
    put_u64(&mut elf, 0); // p_offset
    put_u64(&mut elf, LOAD_ADDRESS); // p_vaddr
    put_u64(&mut elf, LOAD_ADDRESS); // p_paddr
    put_u64(&mut elf, LOADED_LEN); // p_filesz
    put_u64(&mut elf, LOADED_LEN); // p_memsz
    put_u64(&mut elf, PAGE_LEN); // p_align
    // Zeros, then the section names, ending where the section headers begin.
    let names = ELF_SECTION_HEADERS_AT as usize - ELF_SECTION_NAMES.len();
    elf.resize(names, 0);
    elf.extend_from_slice(ELF_SECTION_NAMES);
    // The null section's header, all zeros.
    elf.resize(elf.len() + usize::from(ELF_SECTION_HEADER_LEN), 0);
    // The header of the section names: strings with no flags and no address, named among
    // themselves.
    put_u32(&mut elf, ELF_SECTION_NAMES_NAME); // sh_name
    put_u32(&mut elf, SHT_STRTAB); // sh_type
    put_u64(&mut elf, 0); // sh_flags
    put_u64(&mut elf, 0); // sh_addr
    put_u64(&mut elf, names as u64); // sh_offset
    put_u64(&mut elf, ELF_SECTION_NAMES.len() as u64); // sh_size
    put_u32(&mut elf, 0); // sh_link
    put_u32(&mut elf, 0); // sh_info
    put_u64(&mut elf, ELF_SECTION_NAMES_ALIGNMENT); // sh_addralign
    put_u64(&mut elf, 0); // sh_entsize
    elf
}

/// Appends `sections` to `program` as a launcher file carries them: the bytes of each where the
/// one before it ends, the first where the program does; then their table, JSON that records the
/// program's length and each section's name, offset in the file, size and SHA-256 digest, padded
/// with spaces so that the file's length comes to `residue` modulo 8; then the table's trailer. A
/// residue of 0 is how a launcher file is written, so that a signature needs no padding before
/// its certificate table; the others make the unaligned files a reader must take as well.
/// Returns each section's name, offset, size and digest, in the order given.
pub fn append_sections(
    program: &mut Vec<u8>,
    sections: &[(&str, &[u8])],
    residue: u64,
) -> Vec<(String, u64, u64, [u8; 32])> {
    append_sections_at(program, 0, sections, residue)
}

/// Appends `sections`, their table and its trailer to `bytes` as [`append_sections`] appends them
/// to a program, where `bytes` hold a launcher file from its offset `start` on, so that sections
/// can follow a program too large to hold, to be read back through a [`SparseFile`]. The program
/// is everything before the first section: the first `start` bytes of the file and the bytes held.
pub fn append_sections_at(
    bytes: &mut Vec<u8>,
    start: u64,
    sections: &[(&str, &[u8])],
    residue: u64,
) -> Vec<(String, u64, u64, [u8; 32])> {
    assert!(
        residue < CERTIFICATE_ALIGNMENT,
        "a residue is a length modulo {CERTIFICATE_ALIGNMENT}, not {residue}"
    );
    let end = |bytes: &[u8]| start + bytes.len() as u64;
    let program_length = end(bytes);
    let mut listed = Vec::with_capacity(sections.len());
    for &(name, data) in sections {
        let sha256: [u8; 32] = Sha256::digest(data).into();
        listed.push((name.to_owned(), end(bytes), data.len() as u64, sha256));
        bytes.extend_from_slice(data);
    }
    let entries: Vec<Value> = listed
        .iter()
        .map(|(name, offset, size, sha256)| {
            json!({"name": name, "offset": offset, "size": size, "sha256": hex(sha256)})
        })
        .collect();
    let mut table = json!({
        "schema": TABLE_SCHEMA,
        "programLength": program_length,
        "sections": entries,
    })
    .to_string()
    .into_bytes();
    let unpadded = end(bytes) + table.len() as u64 + TRAILER_LEN;
    let padding = (residue + CERTIFICATE_ALIGNMENT - unpadded % CERTIFICATE_ALIGNMENT)
        % CERTIFICATE_ALIGNMENT;
    table.resize(table.len() + padding as usize, TABLE_PADDING);
    bytes.extend(table_and_trailer(&table));
    listed
}

/// `table` followed by its trailer: its length as 8 little-endian bytes, its SHA-256 digest, then
/// [`TRAILER_MAGIC`]. A table a test writes itself, well formed or not, is closed this way.
pub fn table_and_trailer(table: &[u8]) -> Vec<u8> {
    let mut tail = Vec::with_capacity(table.len() + TRAILER_LEN as usize);
    tail.extend_from_slice(table);
    put_u64(&mut tail, table.len() as u64);
    tail.extend_from_slice(&Sha256::digest(table));
    tail.extend_from_slice(&TRAILER_MAGIC);
    tail
}

/// A file of `len` bytes that holds `chunks` and zeros everywhere else, read through `Read` and
/// `Seek` without the memory its length would take, so that a test can place sections past 4 GiB.
/// Where chunks overlap, the later one is read; their bytes past `len` never are.
#[derive(Clone, Debug)]
pub struct SparseFile {
    /// The file's length.
    pub len: u64,
    /// Each chunk's offset in the file and its bytes.
    pub chunks: Vec<(u64, Vec<u8>)>,
    /// Where the next read begins.
    pub at: u64,
}

impl Read for SparseFile {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let read = (buf.len() as u64).min(self.len.saturating_sub(self.at)) as usize;
        let end = self.at + read as u64;
        buf[..read].fill(0);
        for (start, bytes) in &self.chunks {
            let from = self.at.max(*start);
            let to = end.min(start.saturating_add(bytes.len() as u64));
            if from < to {
                let into = (from - self.at) as usize..(to - self.at) as usize;
                buf[into].copy_from_slice(&bytes[(from - start) as usize..(to - start) as usize]);
            }
        }
        self.at = end;
        Ok(read)
    }
}

impl Seek for SparseFile {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let at = match to {
            SeekFrom::Start(at) => Some(at),
            SeekFrom::End(delta) => self.len.checked_add_signed(delta),
            SeekFrom::Current(delta) => self.at.checked_add_signed(delta),
        };
        self.at = at.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "a seek before the start of the file or past 2^64 bytes",
            )
        })?;
        Ok(self.at)
    }
}

#[cfg(test)]
mod tests {
    use std::io::{self, Read, Seek, SeekFrom};
    use std::panic;

    use serde_json::{Value, json};
    use sha2::{Digest, Sha256};

    use super::{
        FAKE_CERTIFICATE_LEN, SparseFile, TRAILER_MAGIC, append_sections, append_sections_at,
        fake_sign, table_and_trailer, tiny_elf, tiny_pe,
    };
    use crate::runtime::hex;

    /// The SHA-256 digest of no bytes, as Python's hashlib writes it.
    const EMPTY_SHA256: &str = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    /// Where the tiny PE's headers keep its certificate table's data directory: index 4 of the
    /// data directories, which begin 112 bytes into the PE32+ optional header, at 0x58.
    const CERTIFICATE_DIRECTORY: usize = 0x58 + 112 + 4 * 8;

    fn field<const N: usize>(bytes: &[u8], at: usize) -> [u8; N] {
        bytes[at..at + N].try_into().unwrap()
    }

    fn u16_at(bytes: &[u8], at: usize) -> u16 {
        u16::from_le_bytes(field(bytes, at))
    }

    fn u32_at(bytes: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(field(bytes, at))
    }

    fn u64_at(bytes: &[u8], at: usize) -> u64 {
        u64::from_le_bytes(field(bytes, at))
    }

    fn sha256(bytes: &[u8]) -> [u8; 32] {
        Sha256::digest(bytes).into()
    }

    /// Bytes that are not all alike, as a runtime zip's are not.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// Where a PE32+ program ends by its headers: at the end of its headers or of its furthest
    /// section's raw bytes. It must have no COFF symbol table, which would lie after them.
    fn pe_end(pe: &[u8]) -> u64 {
        assert_eq!(pe[..2], *b"MZ");
        let headers = u32_at(pe, 0x3C) as usize;
        assert_eq!(pe[headers..headers + 4], *b"PE\0\0");
        let sections = usize::from(u16_at(pe, headers + 6));
        let symbols = (u32_at(pe, headers + 12), u32_at(pe, headers + 16));
        assert_eq!(symbols, (0, 0), "a COFF symbol table");
        let optional = headers + 24;
        assert_eq!(u16_at(pe, optional), 0x20B, "a PE32+ optional header");
        let table = optional + usize::from(u16_at(pe, headers + 20));
        (0..sections)
            .map(|index| table + 40 * index)
            .filter(|&header| u32_at(pe, header + 16) != 0)
            .map(|header| u64::from(u32_at(pe, header + 20)) + u64::from(u32_at(pe, header + 16)))
            .fold(u64::from(u32_at(pe, optional + 60)), u64::max)
    }

    /// Where a 64-bit little-endian ELF program ends by its headers: at the furthest of its own
    /// header, its tables of program and section headers, and the file bytes of its segments and
    /// of its sections that have any.
    fn elf_extent(elf: &[u8]) -> u64 {
        let (programs, sections) = (u64_at(elf, 32), u64_at(elf, 40));
        let (program_count, section_count) =
            (u64::from(u16_at(elf, 56)), u64::from(u16_at(elf, 60)));
        let segments = (0..program_count).map(|index| {
            let header = (programs + 56 * index) as usize;
            u64_at(elf, header + 8) + u64_at(elf, header + 32)
        });
        let contents = (0..section_count)
            .map(|index| (sections + 64 * index) as usize)
            // A section of type 8, SHT_NOBITS, takes no bytes of the file.
            .filter(|&header| u32_at(elf, header + 4) != 8)
            .map(|header| u64_at(elf, header + 24) + u64_at(elf, header + 32));
        [
            64,
            programs + 56 * program_count,
            sections + 64 * section_count,
        ]
        .into_iter()
        .chain(segments)
        .chain(contents)
        .max()
        .unwrap()
    }

    /// The table a launcher file ends in, as JSON, where it begins and how many spaces pad it,
    /// once its trailer is checked: its length, its digest and the magic.
    fn table_of(file: &[u8]) -> (Value, usize, usize) {
        let trailer = file.len() - 48;
        assert_eq!(file[trailer + 40..], *b"GDLSECT1");
        let start = trailer - usize::try_from(u64_at(file, trailer)).unwrap();
        let table = &file[start..trailer];
        assert_eq!(file[trailer + 8..trailer + 40], sha256(table));
        let padding = table.iter().rev().take_while(|&&byte| byte == b' ').count();
        // The JSON object, then spaces only, which are JSON too, so the table parses as it is.
        assert_eq!(table[table.len() - padding - 1], b'}');
        (serde_json::from_slice(table).unwrap(), start, padding)
    }

    #[test]
    fn tiny_programs_parse() {
        let pe = tiny_pe();
        assert_eq!(pe.len(), 0x400);
        // The PE signature right after the DOS header's 64 bytes, then the COFF header of an
        // x86-64 program of one section, whose PE32+ optional header has 16 data directories.
        assert_eq!(pe[..2], *b"MZ");
        assert_eq!(u32_at(&pe, 0x3C), 0x40);
        assert_eq!(pe[0x40..0x44], *b"PE\0\0");
        assert_eq!(u16_at(&pe, 0x44), 0x8664);
        assert_eq!(u16_at(&pe, 0x46), 1);
        assert_eq!(u16_at(&pe, 0x54), 240);
        assert_eq!(u16_at(&pe, 0x58), 0x20B);
        assert_eq!(u32_at(&pe, 0x58 + 108), 16);
        // The headers take 0x200 bytes, and the section's raw bytes the 0x200 after them.
        assert_eq!(u32_at(&pe, 0x58 + 60), 0x200);
        let section = 0x58 + 240;
        assert_eq!(pe[section..section + 8], *b".text\0\0\0");
        assert_eq!(u32_at(&pe, section + 16), 0x200);
        assert_eq!(u32_at(&pe, section + 20), 0x200);
        assert_eq!(pe_end(&pe), 0x400);
        // In memory the headers take a page and the section the next: its size and address, the
        // alignments and the image's size agree as the PE format wants them to.
        let (in_memory, in_file) = (u32_at(&pe, 0x58 + 32), u32_at(&pe, 0x58 + 36));
        assert_eq!((in_memory, in_file), (0x1000, 0x200));
        let (size, address) = (u32_at(&pe, section + 8), u32_at(&pe, section + 12));
        assert_eq!((size, address), (0x200, 0x1000));
        assert!(address.is_multiple_of(in_memory));
        assert!(address >= u32_at(&pe, 0x58 + 60).next_multiple_of(in_memory));
        for field in [0x58 + 60, section + 16, section + 20] {
            assert!(u32_at(&pe, field).is_multiple_of(in_file), "{field:#x}");
        }
        let image = (address + size).next_multiple_of(in_memory);
        assert_eq!(u32_at(&pe, 0x58 + 56), image);
        assert_eq!(image, 0x2000);
        // Nothing is signed.
        assert_eq!(u64_at(&pe, CERTIFICATE_DIRECTORY), 0);
        // Every other byte is zero.
        let mut rest = pe.clone();
        for (at, len) in [
            (0, 2),
            (0x3C, 4),
            (0x40, 4),
            (0x44, 2),
            (0x46, 2),
            (0x54, 2),
            (0x58, 2),
            (0x58 + 32, 8),
            (0x58 + 56, 8),
            (0x58 + 108, 4),
            (section, 8),
            (section + 8, 16),
        ] {
            rest[at..at + len].fill(0);
        }
        assert!(rest.iter().all(|&byte| byte == 0));

        let elf = tiny_elf();
        assert_eq!(elf.len(), 0x280);
        // A 64-bit little-endian ELF file of version 1 for the System V ABI: an x86-64
        // executable with no entry point and no flags.
        assert_eq!(elf[..16], *b"\x7fELF\x02\x01\x01\0\0\0\0\0\0\0\0\0");
        assert_eq!((u16_at(&elf, 16), u16_at(&elf, 18)), (2, 62));
        assert_eq!(u32_at(&elf, 20), 1);
        assert_eq!(u64_at(&elf, 24), 0);
        assert_eq!(u32_at(&elf, 48), 0);
        // Its program header right after this header, its section headers at 0x200: the header's
        // own length, then one program header and two section headers of their 64-bit lengths,
        // the second holding the section names.
        assert_eq!((u64_at(&elf, 32), u64_at(&elf, 40)), (64, 0x200));
        let counts: Vec<u16> = (52..64).step_by(2).map(|at| u16_at(&elf, at)).collect();
        assert_eq!(counts, [64, 56, 1, 64, 2, 1]);
        // The segment loads the first 0x200 bytes, to be read and executed, at 0x400000,
        // aligned to a page.
        assert_eq!((u32_at(&elf, 64), u32_at(&elf, 68)), (1, 5));
        let segment: Vec<u64> = (72..120).step_by(8).map(|at| u64_at(&elf, at)).collect();
        assert_eq!(segment, [0, 0x40_0000, 0x40_0000, 0x200, 0x200, 0x1000]);
        // The null section's header is all zeros. The next holds strings with no flags and no
        // address, aligned to a byte, which end where the section headers begin and name it.
        assert!(elf[0x200..0x240].iter().all(|&byte| byte == 0));
        let names = 0x240;
        assert_eq!(u32_at(&elf, names + 4), 3);
        assert_eq!((u64_at(&elf, names + 8), u64_at(&elf, names + 16)), (0, 0));
        let offset = usize::try_from(u64_at(&elf, names + 24)).unwrap();
        let size = usize::try_from(u64_at(&elf, names + 32)).unwrap();
        assert_eq!(offset + size, 0x200);
        assert_eq!((u32_at(&elf, names + 40), u32_at(&elf, names + 44)), (0, 0));
        assert_eq!((u64_at(&elf, names + 48), u64_at(&elf, names + 56)), (1, 0));
        let strings = &elf[offset..offset + size];
        assert_eq!(strings, b"\0.shstrtab\0");
        let own = usize::try_from(u32_at(&elf, names)).unwrap();
        let name = strings[own..].split(|&byte| byte == 0).next().unwrap();
        assert_eq!(name, b".shstrtab");
        // Between the program header and the names, nothing.
        assert!(elf[120..offset].iter().all(|&byte| byte == 0));
        assert_eq!(elf_extent(&elf), 0x280);
    }

    #[test]
    fn a_fake_signature_pads_to_8_and_ends_the_file() {
        assert_eq!(FAKE_CERTIFICATE_LEN, 24);
        for residue in 0..8 {
            let mut pe = tiny_pe();
            // Bytes that stand for sections, leaving the file `residue` past a multiple of 8.
            pe.resize(pe.len() + residue, 0xCC);
            let unsigned = pe.clone();
            let padding = fake_sign(&mut pe);
            let case = format!("residue {residue}");
            assert_eq!(padding, (8 - residue as u64) % 8, "{case}");
            let table = unsigned.len() + padding as usize;
            assert!(table.is_multiple_of(8), "{case}");
            assert!(
                pe[unsigned.len()..table].iter().all(|&byte| byte == 0),
                "{case}"
            );
            // One certificate entry ends the file: its length, revision 2.0, the type of PKCS#7
            // signed data, then what stands for the signature.
            assert_eq!(pe.len(), table + 24, "{case}");
            assert_eq!(u32_at(&pe, table), 24, "{case}");
            assert_eq!(u16_at(&pe, table + 4), 0x0200, "{case}");
            assert_eq!(u16_at(&pe, table + 6), 0x0002, "{case}");
            assert!(pe[table + 8..].iter().all(|&byte| byte == 0x30), "{case}");
            // The data directory gives its offset and size. Nothing else in the file changed,
            // so the image ends where it did, before the table.
            let directory = (
                u32_at(&pe, CERTIFICATE_DIRECTORY),
                u32_at(&pe, CERTIFICATE_DIRECTORY + 4),
            );
            assert_eq!(directory, (table as u32, 24), "{case}");
            let mut before = pe[..unsigned.len()].to_vec();
            before[CERTIFICATE_DIRECTORY..CERTIFICATE_DIRECTORY + 8].fill(0);
            assert_eq!(before, unsigned, "{case}");
            assert_eq!(pe_end(&pe), 0x400, "{case}");
        }

        // A launcher file written at a multiple of 8 takes its certificate table straight after
        // its trailer.
        let mut file = tiny_pe();
        append_sections(&mut file, &[("runtime.zip", b"zip")], 0);
        let trailer_end = file.len();
        assert_eq!(fake_sign(&mut file), 0);
        assert_eq!(u32_at(&file, CERTIFICATE_DIRECTORY) as usize, trailer_end);
        assert_eq!(file.len(), trailer_end + 24);
    }

    #[test]
    fn what_cannot_be_written_panics() {
        // A fake signature needs a PE32+ program whose headers hold a certificate table's data
        // directory.
        let mut dos = tiny_pe();
        dos[..2].copy_from_slice(b"ZM");
        let mut signature = tiny_pe();
        signature[0x40..0x44].copy_from_slice(b"PX\0\0");
        let mut pe32 = tiny_pe();
        pe32[0x58..0x5A].copy_from_slice(&0x10Bu16.to_le_bytes());
        let mut few = tiny_pe();
        few[0x58 + 108..0x58 + 112].copy_from_slice(&4u32.to_le_bytes());
        let mut short = tiny_pe();
        short[0x54..0x56].copy_from_slice(&(112 + 4 * 8u16).to_le_bytes());
        let mut moved = tiny_pe();
        moved[0x3C..0x40].copy_from_slice(&0x44u32.to_le_bytes());
        for (case, program) in [
            ("an ELF program", tiny_elf()),
            ("no bytes", Vec::new()),
            ("no DOS signature", dos),
            ("no PE signature", signature),
            ("a PE32 program", pe32),
            ("four data directories", few),
            ("an optional header without the directory", short),
            ("a PE signature not where the DOS header says", moved),
            ("headers cut short", tiny_pe()[..0x80].to_vec()),
        ] {
            let signed = panic::catch_unwind(move || {
                let mut program = program;
                fake_sign(&mut program)
            });
            assert!(signed.is_err(), "{case}");
        }
        // A residue is a length modulo 8.
        let appended = panic::catch_unwind(|| append_sections(&mut tiny_elf(), &[], 8));
        assert!(appended.is_err());
    }

    #[test]
    fn sections_follow_the_program_back_to_back_then_their_table() {
        let runtime = noise(1001);
        let window = b"the window program".to_vec();
        for (kind, program, end) in [
            ("PE", tiny_pe(), pe_end(&tiny_pe())),
            ("ELF", tiny_elf(), elf_extent(&tiny_elf())),
        ] {
            assert_eq!(end, program.len() as u64, "{kind}");
            for residue in 0..8 {
                let case = format!("{kind}, residue {residue}");
                let mut file = program.clone();
                let sections: [(&str, &[u8]); 2] =
                    [("runtime.zip", &runtime), ("launcher", &window)];
                let listed = append_sections(&mut file, &sections, residue);
                // Each section where the one before it ends, the first where the program does.
                let after = end + 1001;
                let size = window.len() as u64;
                assert_eq!(
                    listed,
                    [
                        ("runtime.zip".to_owned(), end, 1001, sha256(&runtime)),
                        ("launcher".to_owned(), after, size, sha256(&window)),
                    ],
                    "{case}"
                );
                assert_eq!(file[..program.len()], program, "{case}");
                assert_eq!(file[end as usize..after as usize], runtime, "{case}");
                assert_eq!(
                    file[after as usize..(after + size) as usize],
                    window,
                    "{case}"
                );
                // The table where the last section ends, padded with fewer than 8 spaces to
                // bring the file to its residue, then its trailer.
                assert_eq!(file.len() as u64 % 8, residue, "{case}");
                let (table, start, padding) = table_of(&file);
                assert_eq!(start as u64, after + size, "{case}");
                assert!(padding < 8, "{case}");
                let expected = json!({
                    "schema": 1,
                    "programLength": end,
                    "sections": [
                        {"name": "runtime.zip", "offset": end, "size": 1001,
                         "sha256": hex(&sha256(&runtime))},
                        {"name": "launcher", "offset": after, "size": size,
                         "sha256": hex(&sha256(&window))},
                    ],
                });
                assert_eq!(table, expected, "{case}");
            }
        }
    }

    #[test]
    fn tables_carry_any_name_and_no_section_at_all() {
        // Names a reader refuses still reach it as the test wrote them, through JSON's escapes.
        let names = [
            "quote\"d",
            "back\\slash",
            "tab\tand\nline",
            "\u{1}",
            "\u{e9}",
            "",
            "runtime.zip",
        ];
        let sections: Vec<(&str, &[u8])> = names.iter().map(|name| (*name, &b""[..])).collect();
        let mut file = tiny_elf();
        let listed = append_sections(&mut file, &sections, 0);
        assert_eq!(listed.len(), names.len());
        let (table, start, _) = table_of(&file);
        // Empty sections take no bytes, so each starts where the program ends.
        assert_eq!(start, 0x280);
        for (index, name) in names.iter().enumerate() {
            let section = &table["sections"][index];
            assert_eq!(section["name"], *name, "{name:?}");
            assert_eq!(section["offset"], 0x280, "{name:?}");
            assert_eq!(section["size"], 0, "{name:?}");
            assert_eq!(section["sha256"], EMPTY_SHA256, "{name:?}");
            assert_eq!(listed[index].0, *name);
        }

        // Without sections, the table lists none and starts where the program ends.
        let mut file = tiny_pe();
        assert!(append_sections(&mut file, &[], 0).is_empty());
        let (table, start, _) = table_of(&file);
        assert_eq!(start, 0x400);
        assert_eq!(
            table,
            json!({"schema": 1, "programLength": 0x400, "sections": []})
        );
    }

    #[test]
    fn sections_can_follow_a_program_too_large_to_hold() {
        // 4.5 GiB and 3 bytes of program, of which only the start is held.
        let program_length = (9 << 29) + 3;
        let mut tail = Vec::new();
        let sections: [(&str, &[u8]); 2] = [("runtime.zip", b"zip"), ("launcher", b"elf")];
        let listed = append_sections_at(&mut tail, program_length, &sections, 0);
        assert_eq!(
            listed,
            [
                ("runtime.zip".to_owned(), program_length, 3, sha256(b"zip")),
                ("launcher".to_owned(), program_length + 3, 3, sha256(b"elf")),
            ]
        );
        assert!(listed[0].1 > u64::from(u32::MAX));
        assert_eq!(tail[..6], *b"zipelf");
        let len = program_length + tail.len() as u64;
        assert!(len.is_multiple_of(8));
        let (table, start, _) = table_of(&tail);
        assert_eq!(start, 6);
        assert_eq!(table["programLength"], program_length);
        assert_eq!(table["sections"][0]["offset"], program_length);
        assert_eq!(table["sections"][1]["offset"], program_length + 3);
        // Read back as a reader reads a file: the trailer at its end, a section at its offset.
        let mut file = SparseFile {
            len,
            chunks: vec![(0, tiny_elf()), (program_length, tail.clone())],
            at: 0,
        };
        assert_eq!(file.seek(SeekFrom::End(-48)).unwrap(), len - 48);
        let mut trailer = [0; 48];
        file.read_exact(&mut trailer).unwrap();
        assert_eq!(trailer[..], tail[tail.len() - 48..]);
        file.seek(SeekFrom::Start(listed[1].1)).unwrap();
        let mut section = [0; 3];
        file.read_exact(&mut section).unwrap();
        assert_eq!(section, *b"elf");

        // The bytes held before the sections are the end of the program.
        let mut held = vec![7; 5];
        let listed = append_sections_at(&mut held, 100, &sections[..1], 3);
        assert_eq!(listed[0].1, 105);
        assert_eq!(held[..8], *b"\x07\x07\x07\x07\x07zip");
        assert_eq!((100 + held.len()) % 8, 3);
        assert_eq!(table_of(&held).0["programLength"], 105);

        // From offset 0, it appends as append_sections does.
        let (mut one, mut other) = (tiny_pe(), tiny_pe());
        let listed = append_sections(&mut one, &sections, 5);
        assert_eq!(listed, append_sections_at(&mut other, 0, &sections, 5));
        assert_eq!(one, other);
    }

    #[test]
    fn a_written_table_is_closed_by_its_trailer() {
        assert_eq!(TRAILER_MAGIC, *b"GDLSECT1");
        for table in [&br#"{"schema": 2}  "#[..], b"not JSON", b""] {
            let tail = table_and_trailer(table);
            let len = table.len();
            assert_eq!(tail.len(), len + 48);
            assert_eq!(tail[..len], *table);
            assert_eq!(u64_at(&tail, len), len as u64);
            assert_eq!(tail[len + 8..len + 40], sha256(table));
            assert_eq!(tail[len + 40..], *b"GDLSECT1");
        }
        assert_eq!(hex(&table_and_trailer(b"")[8..40]), EMPTY_SHA256);
    }

    #[test]
    fn sparse_files_hold_their_chunks_and_zeros_elsewhere() {
        let chunks = vec![
            (10, b"abc".to_vec()),
            (11, b"Z".to_vec()),
            (98, b"xyz".to_vec()),
        ];
        let mut file = SparseFile {
            len: 100,
            chunks,
            at: 0,
        };
        // The later chunk is read where two overlap, and nothing past the end.
        let mut expected = vec![0; 100];
        expected[10..13].copy_from_slice(b"aZc");
        expected[98..].copy_from_slice(b"xy");
        let mut whole = Vec::new();
        assert_eq!(file.read_to_end(&mut whole).unwrap(), 100);
        assert_eq!(whole, expected);
        assert_eq!(file.at, 100);
        // Reads of any length from any place agree with it, whatever they straddle.
        for start in 0..=100 {
            for len in [0, 1, 2, 3, 5, 64, 200] {
                assert_eq!(file.seek(SeekFrom::Start(start)).unwrap(), start);
                let mut buffer = vec![0xEE; len];
                let read = file.read(&mut buffer).unwrap();
                let start = start as usize;
                let case = format!("{len} bytes from {start}");
                assert_eq!(read, len.min(100 - start), "{case}");
                assert_eq!(buffer[..read], expected[start..start + read], "{case}");
                assert!(buffer[read..].iter().all(|&byte| byte == 0xEE), "{case}");
                assert_eq!(file.at, (start + read) as u64, "{case}");
            }
        }
        // Seeks from the end and from where it is; past the end there is nothing to read.
        assert_eq!(file.seek(SeekFrom::End(0)).unwrap(), 100);
        assert_eq!(file.seek(SeekFrom::End(-90)).unwrap(), 10);
        assert_eq!(file.seek(SeekFrom::Current(1)).unwrap(), 11);
        let mut two = [0; 2];
        file.read_exact(&mut two).unwrap();
        assert_eq!(two, *b"Zc");
        assert_eq!(file.seek(SeekFrom::Current(1000)).unwrap(), 1013);
        assert_eq!(file.read(&mut two).unwrap(), 0);
        // No seek goes before the start or past 2^64 bytes, and one refused moves nothing.
        for to in [
            SeekFrom::End(-101),
            SeekFrom::Current(-1014),
            SeekFrom::Current(i64::MIN),
        ] {
            let error = file.seek(to).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{to:?}");
            assert_eq!(file.at, 1013, "{to:?}");
        }
        assert_eq!(file.seek(SeekFrom::Start(u64::MAX)).unwrap(), u64::MAX);
        let error = file.seek(SeekFrom::Current(1)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        assert_eq!(file.read(&mut two).unwrap(), 0);

        // A file larger than memory holds what lies past 4 GiB.
        let far = 9 << 29;
        let mut file = SparseFile {
            len: 5 << 30,
            chunks: vec![(far, b"far".to_vec())],
            at: 0,
        };
        assert_eq!(file.seek(SeekFrom::End(0)).unwrap(), 5 << 30);
        file.seek(SeekFrom::Start(far - 1)).unwrap();
        let mut around = [0xEE; 5];
        file.read_exact(&mut around).unwrap();
        assert_eq!(around, *b"\0far\0");
    }
}
