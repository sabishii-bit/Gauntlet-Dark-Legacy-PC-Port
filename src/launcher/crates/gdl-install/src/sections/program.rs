//! Where a launcher file's program ends, by the program's own headers, which is where its first
//! section must begin: the end of a PE image's headers and section data, or of its COFF symbol
//! table and string table when it has them, with where a signature's certificate table lies; or
//! the furthest of an x86-64 ELF64 little-endian program's headers, segments and sections.

use std::io::{Read, Seek};

use super::{SectionsError, read_array, read_vec, u16_at, u32_at, u64_at};

/// The length of the header that begins both programs: a PE's DOS header and an ELF header.
const HEADER_LEN: u64 = 64;

/// The magic a PE's DOS header begins with.
const IMAGE_DOS_SIGNATURE: [u8; 2] = *b"MZ";

/// Where the DOS header keeps the offset of the PE signature, its `e_lfanew`.
const E_LFANEW_AT: usize = 0x3C;

/// The signature that begins a PE's own headers.
const IMAGE_NT_SIGNATURE: [u8; 4] = *b"PE\0\0";

/// The length of the PE signature and the COFF file header after it, which the optional header
/// follows.
const COFF_HEADER_LEN: u64 = 24;

/// Where the COFF header keeps the number of sections, counted from the PE signature.
const NUMBER_OF_SECTIONS_AT: usize = 6;

/// Where the COFF header keeps the offset in the file of the COFF symbol table, counted from the
/// PE signature.
const POINTER_TO_SYMBOL_TABLE_AT: usize = 12;

/// Where the COFF header keeps the number of COFF symbols, counted from the PE signature.
const NUMBER_OF_SYMBOLS_AT: usize = 16;

/// Where the COFF header keeps the length of the optional header, counted from the PE signature.
const SIZE_OF_OPTIONAL_HEADER_AT: usize = 20;

/// The most bytes a PE's optional header may take: far more than the 240 of a PE32+ one with
/// every data directory.
const MAX_OPTIONAL_HEADER: u64 = 4096;

/// The magic of a PE32 optional header.
const PE32_MAGIC: u16 = 0x10B;

/// The magic of a PE32+ optional header.
const PE32_PLUS_MAGIC: u16 = 0x20B;

/// Where a PE32 optional header keeps the number of its data directories, which follow it.
const PE32_DIRECTORY_COUNT_AT: usize = 92;

/// Where a PE32+ optional header keeps the number of its data directories, which follow it.
const PE32_PLUS_DIRECTORY_COUNT_AT: usize = 108;

/// The length of the field that holds the number of data directories.
const DIRECTORY_COUNT_LEN: usize = 4;

/// Where an optional header of either kind keeps the size of all the headers.
const SIZE_OF_HEADERS_AT: usize = 60;

/// The length of a data directory: an address, then a size, 4 bytes each.
const DATA_DIRECTORY_LEN: usize = 8;

/// The index of the certificate table among the data directories, whose address is a file offset,
/// not one in memory.
const SECURITY_DIRECTORY: usize = 4;

/// The length of one entry of a PE's section table.
const SECTION_HEADER_LEN: usize = 40;

/// Where a section's header keeps the length of its data in the file.
const SIZE_OF_RAW_DATA_AT: usize = 16;

/// Where a section's header keeps the offset of its data in the file.
const POINTER_TO_RAW_DATA_AT: usize = 20;

/// The length of one COFF symbol.
const COFF_SYMBOL_LEN: u64 = 18;

/// The length of the field that begins the COFF string table and gives its length, itself
/// included: the least a string table takes, even one whose field gives less.
const STRING_TABLE_LEN_FIELD: u64 = 4;

/// The magic every ELF file begins with.
const ELFMAG: [u8; 4] = *b"\x7fELF";

/// Where the identification bytes keep the file's class.
const EI_CLASS: usize = 4;

/// The class of a 64-bit ELF file.
const ELFCLASS64: u8 = 2;

/// Where the identification bytes keep the file's data encoding.
const EI_DATA: usize = 5;

/// The data encoding of a little-endian ELF file.
const ELFDATA2LSB: u8 = 1;

/// Where the ELF header keeps the machine the program is for.
const E_MACHINE_AT: usize = 18;

/// The machine of an x86-64 program.
const EM_X86_64: u16 = 62;

/// Where the ELF header keeps the offset of the program headers.
const E_PHOFF_AT: usize = 32;

/// Where the ELF header keeps the offset of the section headers.
const E_SHOFF_AT: usize = 40;

/// Where the ELF header keeps the length of one program header.
const E_PHENTSIZE_AT: usize = 54;

/// Where the ELF header keeps the number of program headers.
const E_PHNUM_AT: usize = 56;

/// Where the ELF header keeps the length of one section header.
const E_SHENTSIZE_AT: usize = 58;

/// Where the ELF header keeps the number of section headers.
const E_SHNUM_AT: usize = 60;

/// The length of a 64-bit program header.
const PROGRAM_HEADER_LEN: usize = 56;

/// The length of a 64-bit section header.
const ELF_SECTION_HEADER_LEN: usize = 64;

/// The number of program headers that means the true number lies in the first section header.
const PN_XNUM: u16 = 0xFFFF;

/// Where a program header keeps the offset in the file of its segment's bytes.
const P_OFFSET_AT: usize = 8;

/// Where a program header keeps how many bytes of the file its segment takes.
const P_FILESZ_AT: usize = 32;

/// Where a section header keeps the section's type.
const SH_TYPE_AT: usize = 4;

/// Where a section header keeps the offset in the file of the section's bytes.
const SH_OFFSET_AT: usize = 24;

/// Where a section header keeps how many bytes the section takes.
const SH_SIZE_AT: usize = 32;

/// The type of a section that takes no bytes of the file, such as `.bss`.
const SHT_NOBITS: u32 = 8;

/// Where a launcher file's program ends, and where a signature's certificate table lies.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Program {
    /// Where the program's last bytes end in the file.
    pub(super) end: u64,
    /// A signed PE's certificate table, as its offset in the file and its size.
    pub(super) certificates: Option<(u64, u64)>,
}

/// Reads where the program of `source`, a file of `file_len` bytes, ends by its headers, and where
/// a PE's certificate table lies.
pub(super) fn layout<R: Read + Seek>(
    source: &mut R,
    file_len: u64,
) -> Result<Program, SectionsError> {
    if file_len < HEADER_LEN {
        return Err(SectionsError::UnknownProgram);
    }
    let header: [u8; HEADER_LEN as usize] = read_array(source, 0)?;
    if header[..IMAGE_DOS_SIGNATURE.len()] == IMAGE_DOS_SIGNATURE {
        return pe(source, file_len, &header);
    }
    let x86_64_elf = header[..ELFMAG.len()] == ELFMAG
        && header[EI_CLASS] == ELFCLASS64
        && header[EI_DATA] == ELFDATA2LSB
        && u16_at(&header, E_MACHINE_AT) == EM_X86_64;
    if x86_64_elf {
        return elf(source, file_len, &header);
    }
    Err(SectionsError::UnknownProgram)
}

/// Reads where a PE ends: at the furthest of the end of its headers, the end of each section's
/// data in the file, and, when it has them, the end of its COFF symbol table and of the string
/// table after it.
fn pe<R: Read + Seek>(source: &mut R, file_len: u64, dos: &[u8]) -> Result<Program, SectionsError> {
    let signature = u64::from(u32_at(dos, E_LFANEW_AT));
    if signature + COFF_HEADER_LEN > file_len {
        return Err(SectionsError::UnknownProgram);
    }
    let coff: [u8; COFF_HEADER_LEN as usize] = read_array(source, signature)?;
    if coff[..IMAGE_NT_SIGNATURE.len()] != IMAGE_NT_SIGNATURE {
        return Err(SectionsError::UnknownProgram);
    }
    let section_count = usize::from(u16_at(&coff, NUMBER_OF_SECTIONS_AT));
    let optional_len = u16_at(&coff, SIZE_OF_OPTIONAL_HEADER_AT);
    let optional_at = signature + COFF_HEADER_LEN;
    let sections_at = optional_at + u64::from(optional_len);
    let headers_end = sections_at + (section_count * SECTION_HEADER_LEN) as u64;
    if u64::from(optional_len) > MAX_OPTIONAL_HEADER || headers_end > file_len {
        return Err(pe_headers("PE headers run past the file"));
    }
    let optional = read_vec(source, optional_at, usize::from(optional_len))?;
    let count_at = match optional.get(..2).map(|magic| u16_at(magic, 0)) {
        Some(PE32_MAGIC) => PE32_DIRECTORY_COUNT_AT,
        Some(PE32_PLUS_MAGIC) => PE32_PLUS_DIRECTORY_COUNT_AT,
        _ => return Err(pe_headers("unknown optional header magic")),
    };
    let directories_at = count_at + DIRECTORY_COUNT_LEN;
    if optional.len() < directories_at {
        return Err(pe_headers("optional header too short"));
    }
    let directory_count = u32_at(&optional, count_at);
    let security = directories_at + SECURITY_DIRECTORY * DATA_DIRECTORY_LEN;
    let certificates = if u64::from(directory_count) > SECURITY_DIRECTORY as u64
        && optional.len() >= security + DATA_DIRECTORY_LEN
    {
        let offset = u64::from(u32_at(&optional, security));
        let size = u64::from(u32_at(&optional, security + 4));
        (size != 0).then_some((offset, size))
    } else {
        None
    };
    let mut end = u64::from(u32_at(&optional, SIZE_OF_HEADERS_AT));
    let headers = read_vec(source, sections_at, section_count * SECTION_HEADER_LEN)?;
    for header in headers.as_chunks::<SECTION_HEADER_LEN>().0 {
        let raw_size = u64::from(u32_at(header, SIZE_OF_RAW_DATA_AT));
        let raw_offset = u64::from(u32_at(header, POINTER_TO_RAW_DATA_AT));
        if raw_size != 0 {
            end = end.max(raw_offset + raw_size);
        }
    }
    let symbols = u64::from(u32_at(&coff, POINTER_TO_SYMBOL_TABLE_AT));
    if symbols != 0 {
        end = end.max(string_table_end(source, file_len, symbols, &coff)?);
    }
    if end > file_len {
        return Err(pe_headers("PE sections run past the file"));
    }
    Ok(Program { end, certificates })
}

/// Where the COFF string table ends that follows the symbol table at `symbols`: its first 4
/// bytes give its length, themselves included. A length under 4, which some tools write for an
/// empty string table, is taken as the 4 bytes of the field itself.
fn string_table_end<R: Read + Seek>(
    source: &mut R,
    file_len: u64,
    symbols: u64,
    coff: &[u8],
) -> Result<u64, SectionsError> {
    let strings_at = symbols + u64::from(u32_at(coff, NUMBER_OF_SYMBOLS_AT)) * COFF_SYMBOL_LEN;
    if strings_at + STRING_TABLE_LEN_FIELD > file_len {
        return Err(pe_headers("the COFF symbol table runs past the file"));
    }
    let len: [u8; STRING_TABLE_LEN_FIELD as usize] = read_array(source, strings_at)?;
    let end = strings_at + u64::from(u32::from_le_bytes(len)).max(STRING_TABLE_LEN_FIELD);
    if end > file_len {
        return Err(pe_headers("the COFF string table runs past the file"));
    }
    Ok(end)
}

/// Reads where an x86-64 ELF64 little-endian program ends: at the furthest of its own header, its
/// tables of program and section headers, and the bytes of the file its segments and its sections
/// take, a section that takes none aside.
fn elf<R: Read + Seek>(
    source: &mut R,
    file_len: u64,
    header: &[u8],
) -> Result<Program, SectionsError> {
    let program_count = u16_at(header, E_PHNUM_AT);
    let section_count = u16_at(header, E_SHNUM_AT);
    let sections_at = u64_at(header, E_SHOFF_AT);
    if program_count == PN_XNUM {
        return Err(elf_headers(
            "the number of ELF program headers lies in the first section header",
        ));
    }
    if section_count == 0 && sections_at != 0 {
        return Err(elf_headers(
            "the number of ELF section headers lies in the first section header",
        ));
    }
    let program_entry = usize::from(u16_at(header, E_PHENTSIZE_AT));
    let section_entry = usize::from(u16_at(header, E_SHENTSIZE_AT));
    if (program_count != 0 && program_entry != PROGRAM_HEADER_LEN)
        || (section_count != 0 && section_entry != ELF_SECTION_HEADER_LEN)
    {
        return Err(elf_headers(
            "the ELF header entries are not of ELF64's lengths",
        ));
    }
    let programs = table(
        source,
        file_len,
        u64_at(header, E_PHOFF_AT),
        program_count,
        PROGRAM_HEADER_LEN,
    )?;
    let sections = table(
        source,
        file_len,
        sections_at,
        section_count,
        ELF_SECTION_HEADER_LEN,
    )?;
    let mut end = HEADER_LEN.max(programs.end).max(sections.end);
    for program in programs.headers.as_chunks::<PROGRAM_HEADER_LEN>().0 {
        let segment = u64_at(program, P_OFFSET_AT);
        end = end.max(range_end(segment, u64_at(program, P_FILESZ_AT), file_len)?);
    }
    for section in sections.headers.as_chunks::<ELF_SECTION_HEADER_LEN>().0 {
        if u32_at(section, SH_TYPE_AT) != SHT_NOBITS {
            let bytes = u64_at(section, SH_OFFSET_AT);
            end = end.max(range_end(bytes, u64_at(section, SH_SIZE_AT), file_len)?);
        }
    }
    Ok(Program {
        end,
        certificates: None,
    })
}

/// A table of an ELF program's headers, read, and where it ends in the file.
struct HeaderTable {
    /// The headers' bytes.
    headers: Vec<u8>,
    /// Where the table ends.
    end: u64,
}

/// Reads the `count` headers of `len` bytes each at `at`, which must lie within the file.
fn table<R: Read + Seek>(
    source: &mut R,
    file_len: u64,
    at: u64,
    count: u16,
    len: usize,
) -> Result<HeaderTable, SectionsError> {
    let bytes = usize::from(count) * len;
    let end = range_end(at, bytes as u64, file_len)?;
    let headers = if bytes == 0 {
        Vec::new()
    } else {
        read_vec(source, at, bytes)?
    };
    Ok(HeaderTable { headers, end })
}

/// Where the `len` bytes from `offset` end, which must lie within the file.
fn range_end(offset: u64, len: u64, file_len: u64) -> Result<u64, SectionsError> {
    offset
        .checked_add(len)
        .filter(|end| *end <= file_len)
        .ok_or_else(|| elf_headers("ELF ranges run past the file"))
}

/// The refusal of a PE whose headers do not say where it ends, for the reason `detail` gives.
fn pe_headers(detail: &'static str) -> SectionsError {
    SectionsError::PeHeaders { detail }
}

/// The refusal of an ELF program whose headers do not say where it ends, for the reason `detail`
/// gives.
fn elf_headers(detail: &'static str) -> SectionsError {
    SectionsError::ElfHeaders { detail }
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;

    use gdl_fixtures::{FAKE_CERTIFICATE_LEN, fake_sign, tiny_elf, tiny_pe};

    use super::{Program, SectionsError, layout};

    /// Where the tiny PE's optional header begins, after its DOS header, its PE signature and its
    /// COFF header.
    const OPTIONAL: usize = 0x58;

    /// Where the tiny PE's section table begins: after its PE32+ optional header of 240 bytes.
    const SECTIONS: usize = OPTIONAL + 240;

    /// Where the tiny ELF program's header of the section names begins: the second of its section
    /// headers, which begin at 0x200.
    const NAMES_HEADER: usize = 0x240;

    fn layout_of(file: &[u8]) -> Result<Program, SectionsError> {
        layout(&mut Cursor::new(file), file.len() as u64)
    }

    /// Where `file`'s program ends.
    fn end_of(file: &[u8]) -> u64 {
        layout_of(file)
            .unwrap_or_else(|error| panic!("{error}"))
            .end
    }

    /// Why `file`'s program is refused, as its rule and its text.
    fn refusal_of(file: &[u8]) -> (&'static str, String) {
        match layout_of(file) {
            Err(error) => (error.rule(), error.to_string()),
            Ok(program) => panic!("{program:?}"),
        }
    }

    fn put_u16(file: &mut [u8], at: usize, value: u16) {
        file[at..at + 2].copy_from_slice(&value.to_le_bytes());
    }

    fn put_u32(file: &mut [u8], at: usize, value: u32) {
        file[at..at + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn put_u64(file: &mut [u8], at: usize, value: u64) {
        file[at..at + 8].copy_from_slice(&value.to_le_bytes());
    }

    #[test]
    fn tiny_programs_end_where_their_headers_say() {
        let unsigned = Program {
            end: 0x400,
            certificates: None,
        };
        assert_eq!(layout_of(&tiny_pe()).unwrap(), unsigned);
        let mut signed = tiny_pe();
        fake_sign(&mut signed);
        let certificates = Some((0x400, FAKE_CERTIFICATE_LEN));
        let program = layout_of(&signed).unwrap();
        assert_eq!((program.end, program.certificates), (0x400, certificates));
        let elf = Program {
            end: 0x280,
            certificates: None,
        };
        assert_eq!(layout_of(&tiny_elf()).unwrap(), elf);
        // What follows the program does not move where it ends.
        let mut longer = tiny_elf();
        longer.resize(0x1000, 0xCC);
        assert_eq!(end_of(&longer), 0x280);
    }

    #[test]
    fn a_pe_ends_at_its_furthest_section_or_its_headers() {
        // A second section whose data lies after the first's.
        let mut two = tiny_pe();
        put_u16(&mut two, 0x46, 2);
        put_u32(&mut two, SECTIONS + 40 + 16, 0x200); // SizeOfRawData
        put_u32(&mut two, SECTIONS + 40 + 20, 0x400); // PointerToRawData
        two.resize(0x600, 0);
        assert_eq!(end_of(&two), 0x600);
        // A section that holds no data in the file counts for nothing, wherever it says it is.
        let mut empty = two.clone();
        put_u32(&mut empty, SECTIONS + 40 + 16, 0);
        put_u32(&mut empty, SECTIONS + 40 + 20, 0x10_0000);
        assert_eq!(end_of(&empty), 0x400);
        // Headers that end after every section.
        let mut headers = tiny_pe();
        put_u32(&mut headers, OPTIONAL + 60, 0x500);
        headers.resize(0x500, 0);
        assert_eq!(end_of(&headers), 0x500);
        // An optional header of the most bytes taken, the section table after it, ending the
        // file.
        let mut largest = tiny_pe();
        put_u16(&mut largest, 0x54, 4096);
        let table = OPTIONAL + 4096;
        largest.resize(table + 40, 0);
        largest.copy_within(SECTIONS..SECTIONS + 40, table);
        assert_eq!(end_of(&largest), 0x400);
        // An optional header that ends with its count of data directories, holding none of them:
        // its certificate table's directory is not there to read.
        let mut shortest = tiny_pe();
        put_u16(&mut shortest, 0x54, 112);
        shortest.copy_within(SECTIONS..SECTIONS + 40, OPTIONAL + 112);
        let program = layout_of(&shortest).unwrap();
        assert_eq!((program.end, program.certificates), (0x400, None));
        // One that ends inside the certificate table's directory, its offset held and not its
        // size: there is no certificate table to read either.
        let mut partial = tiny_pe();
        put_u16(&mut partial, 0x54, 148);
        put_u32(&mut partial, OPTIONAL + 144, 0x400);
        partial.copy_within(SECTIONS..SECTIONS + 40, OPTIONAL + 148);
        let program = layout_of(&partial).unwrap();
        assert_eq!((program.end, program.certificates), (0x400, None));
    }

    #[test]
    fn a_pe32_keeps_its_directories_where_pe32_does() {
        // The tiny PE given a PE32 optional header's magic: its count of data directories and the
        // directories themselves lie 16 bytes earlier than a PE32+ one's.
        let mut pe32 = tiny_pe();
        put_u16(&mut pe32, OPTIONAL, 0x10B);
        put_u32(&mut pe32, OPTIONAL + 108, 0);
        assert_eq!(layout_of(&pe32).unwrap().certificates, None);
        put_u32(&mut pe32, OPTIONAL + 92, 16);
        let offset = u32::try_from(pe32.len()).unwrap();
        pe32.resize(pe32.len() + 24, 0x30);
        put_u32(&mut pe32, OPTIONAL + 96 + 4 * 8, offset);
        put_u32(&mut pe32, OPTIONAL + 96 + 4 * 8 + 4, 24);
        let program = layout_of(&pe32).unwrap();
        assert_eq!(
            (program.end, program.certificates),
            (0x400, Some((0x400, 24)))
        );
        // With four data directories it has no certificate table's.
        put_u32(&mut pe32, OPTIONAL + 92, 4);
        assert_eq!(layout_of(&pe32).unwrap().certificates, None);
    }

    #[test]
    fn the_coff_string_table_takes_at_least_its_length_field() {
        // One symbol after the image, then the string table, which ends the file.
        for (field, len) in [(0, 4), (3, 4), (4, 4), (9, 9)] {
            let mut pe = tiny_pe();
            put_u32(&mut pe, 0x4C, 0x400); // PointerToSymbolTable
            put_u32(&mut pe, 0x50, 1); // NumberOfSymbols
            pe.resize(0x400 + 18, 0);
            pe.extend_from_slice(&u32::to_le_bytes(field));
            pe.resize(0x400 + 18 + len, 0);
            assert_eq!(end_of(&pe), pe.len() as u64, "a length field of {field}");
        }
        // A symbol table and string table within the image end with it.
        let mut pe = tiny_pe();
        put_u32(&mut pe, 0x4C, 0x300);
        put_u32(&mut pe, 0x50, 3);
        assert_eq!(end_of(&pe), 0x400);
        // Symbols that begin in the image and reach past it, then a string table of its length
        // alone.
        put_u32(&mut pe, 0x4C, 0x3F0);
        put_u32(&mut pe, 0x50, 9);
        pe.resize(0x3F0 + 9 * 18 + 4, 0);
        assert_eq!(end_of(&pe), pe.len() as u64);
    }

    #[test]
    fn pe_headers_that_do_not_say_where_it_ends_are_refused() {
        let changed = |change: &dyn Fn(&mut Vec<u8>)| {
            let mut pe = tiny_pe();
            change(&mut pe);
            pe
        };
        let past = "PE headers run past the file";
        for (what, pe, detail) in [
            (
                "sections past the file",
                changed(&|pe| put_u16(pe, 0x46, 0xFFFF)),
                past,
            ),
            (
                "an optional header too long",
                changed(&|pe| put_u16(pe, 0x54, 4097)),
                past,
            ),
            (
                "another magic",
                changed(&|pe| put_u16(pe, OPTIONAL, 0x10C)),
                "unknown optional header magic",
            ),
            (
                "no optional header",
                changed(&|pe| put_u16(pe, 0x54, 0)),
                "unknown optional header magic",
            ),
            (
                "an optional header without its count of data directories",
                changed(&|pe| put_u16(pe, 0x54, 111)),
                "optional header too short",
            ),
            (
                "a section's data past the file",
                changed(&|pe| put_u32(pe, SECTIONS + 16, 0x201)),
                "PE sections run past the file",
            ),
            (
                "headers longer than the file",
                changed(&|pe| put_u32(pe, OPTIONAL + 60, 0x401)),
                "PE sections run past the file",
            ),
            (
                "symbols past the file",
                changed(&|pe| {
                    put_u32(pe, 0x4C, 0x400);
                    put_u32(pe, 0x50, 1);
                    pe.resize(0x400 + 18 + 3, 0);
                }),
                "the COFF symbol table runs past the file",
            ),
            (
                "a string table past the file",
                changed(&|pe| {
                    put_u32(pe, 0x4C, 0x400);
                    put_u32(pe, 0x50, 1);
                    pe.resize(0x400 + 18, 0);
                    pe.extend_from_slice(&9u32.to_le_bytes());
                    pe.resize(0x400 + 18 + 8, 0);
                }),
                "the COFF string table runs past the file",
            ),
        ] {
            assert_eq!(refusal_of(&pe), ("peHeaders", detail.to_owned()), "{what}");
        }
    }

    #[test]
    fn what_is_neither_a_pe_nor_an_x86_64_elf_is_unknown() {
        let changed = |program: Vec<u8>, change: &dyn Fn(&mut Vec<u8>)| {
            let mut program = program;
            change(&mut program);
            program
        };
        for (what, file) in [
            ("nothing", Vec::new()),
            ("a PE cut short of its DOS header", tiny_pe()[..63].to_vec()),
            (
                "an ELF program cut short of its header",
                tiny_elf()[..63].to_vec(),
            ),
            (
                "text",
                b"not a program, though long enough to hold a header of 64".repeat(2),
            ),
            (
                "a DOS header whose PE signature lies past the file",
                changed(tiny_pe(), &|pe| put_u32(pe, 0x3C, 0x400 - 23)),
            ),
            (
                "a DOS header without a PE signature",
                changed(tiny_pe(), &|pe| pe[0x43] = 1),
            ),
            (
                "a 32-bit ELF program",
                changed(tiny_elf(), &|elf| elf[4] = 1),
            ),
            (
                "a big-endian ELF program",
                changed(tiny_elf(), &|elf| elf[5] = 2),
            ),
            (
                "an ELF program for another machine",
                changed(tiny_elf(), &|elf| put_u16(elf, 18, 183)),
            ),
            ("no ELF magic", changed(tiny_elf(), &|elf| elf[3] = b'G')),
        ] {
            let (rule, _) = refusal_of(&file);
            assert_eq!(rule, "unknownProgram", "{what}");
        }
        // The PE signature may lie at the very end of the file, with its COFF header.
        let mut last = tiny_pe();
        put_u32(&mut last, 0x3C, 0x400 - 24);
        assert_eq!(refusal_of(&last).0, "unknownProgram");
        last[0x400 - 24..0x400 - 20].copy_from_slice(b"PE\0\0");
        assert_eq!(refusal_of(&last).0, "peHeaders");
    }

    #[test]
    fn an_elf_program_ends_at_its_furthest_range() {
        // The section names grown past the section headers, to the file's new end.
        let mut names = tiny_elf();
        names.resize(0x290, 0);
        let names_at = u64::from_le_bytes(
            names[NAMES_HEADER + 24..NAMES_HEADER + 32]
                .try_into()
                .unwrap(),
        );
        put_u64(&mut names, NAMES_HEADER + 32, 0x290 - names_at);
        assert_eq!(end_of(&names), 0x290);
        // A section that takes no bytes of the file counts for nothing.
        let mut nobits = names.clone();
        put_u32(&mut nobits, NAMES_HEADER + 4, 8);
        put_u64(&mut nobits, NAMES_HEADER + 32, 1 << 40);
        assert_eq!(end_of(&nobits), 0x280);
        // The segment grown to the file's end.
        let mut segment = tiny_elf();
        segment.resize(0x290, 0);
        put_u64(&mut segment, 64 + 32, 0x290);
        assert_eq!(end_of(&segment), 0x290);
        // The program headers' table past everything else.
        let mut programs = tiny_elf();
        programs.resize(0x300, 0);
        programs.copy_within(64..120, 0x300 - 56);
        put_u64(&mut programs, 32, 0x300 - 56);
        assert_eq!(end_of(&programs), 0x300);
        // With no headers but its own, an ELF header ends the program; counts of nought need no
        // entry lengths.
        let mut bare = tiny_elf();
        for at in [32, 40] {
            put_u64(&mut bare, at, 0);
        }
        for at in [54, 56, 58, 60] {
            put_u16(&mut bare, at, 0);
        }
        assert_eq!(end_of(&bare), 64);
    }

    #[test]
    fn elf_headers_that_do_not_say_where_it_ends_are_refused() {
        let changed = |change: &dyn Fn(&mut Vec<u8>)| {
            let mut elf = tiny_elf();
            change(&mut elf);
            elf
        };
        let ranges = "ELF ranges run past the file";
        let entries = "the ELF header entries are not of ELF64's lengths";
        for (what, elf, detail) in [
            (
                "a count of program headers kept elsewhere",
                changed(&|elf| put_u16(elf, 56, 0xFFFF)),
                "the number of ELF program headers lies in the first section header",
            ),
            (
                "a count of section headers kept elsewhere",
                changed(&|elf| put_u16(elf, 60, 0)),
                "the number of ELF section headers lies in the first section header",
            ),
            (
                "program headers of 64 bytes",
                changed(&|elf| put_u16(elf, 54, 64)),
                entries,
            ),
            (
                "section headers of 40 bytes",
                changed(&|elf| put_u16(elf, 58, 40)),
                entries,
            ),
            (
                "section headers past the file",
                changed(&|elf| put_u64(elf, 40, 0x201)),
                ranges,
            ),
            (
                "program headers past 2^64",
                changed(&|elf| put_u64(elf, 32, u64::MAX - 8)),
                ranges,
            ),
            (
                "a segment past the file",
                changed(&|elf| put_u64(elf, 64 + 32, 0x281)),
                ranges,
            ),
            (
                "a segment past 2^64",
                changed(&|elf| put_u64(elf, 64 + 8, u64::MAX)),
                ranges,
            ),
            (
                "section names past the file",
                changed(&|elf| put_u64(elf, NAMES_HEADER + 32, 0x100)),
                ranges,
            ),
            (
                "section names past 2^64",
                changed(&|elf| put_u64(elf, NAMES_HEADER + 24, u64::MAX)),
                ranges,
            ),
            (
                "program headers, none of them, past the file",
                changed(&|elf| {
                    put_u16(elf, 56, 0);
                    put_u64(elf, 32, 0x281);
                }),
                ranges,
            ),
        ] {
            assert_eq!(
                refusal_of(&elf),
                ("elfHeaders", detail.to_owned()),
                "{what}"
            );
        }
    }
}
