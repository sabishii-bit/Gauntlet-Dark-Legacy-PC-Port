//! The sections a launcher file carries after its program: the game's runtime zip and, in a Linux
//! launcher file, the window program, each listed with its SHA-256 digest in a table of JSON at the
//! file's end. The table's trailer is found before a signature's certificate table, so a launcher
//! file reads the same signed or not; and every byte from the program's end to the trailer belongs
//! to a section or to the table, so that a digest covers each one without a signature.
//!
//! The file is laid out as its program; then each section's bytes, the first where the program
//! ends and each other where the one before it ends; then the table, padded with spaces; then a
//! trailer of 48 bytes: the table's length in 8 little-endian bytes, its SHA-256 digest and
//! `GDLSECT1`. Up to 7 zero bytes may follow the trailer, which a signer pads the file with before
//! its certificate table, and which removing the signature leaves behind.

mod program;
mod view;

use std::fmt;
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::ops::{Range, RangeInclusive};
use std::path::{Path, PathBuf};

use gdl_files::{BLOCK_LEN, Cancelled, Job};
use serde_json::{Map, Value};
use sha2::{Digest, Sha256};

use crate::payload::{Payload, PayloadError, SHA256_LEN, sha256_of};

pub use view::SectionView;

/// The section that holds the game's runtime zip, which every launcher file carries.
pub const RUNTIME_SECTION: &str = "runtime.zip";

/// The section that holds the window program, which a Linux launcher file carries for its entry
/// program to start.
pub const WINDOW_SECTION: &str = "launcher";

/// The most bytes a section may hold.
pub const MAX_SECTION_LEN: u64 = 2 << 30;

/// The most bytes the table may take, its padding included.
pub const MAX_TABLE_LEN: u64 = 64 << 10;

/// The most characters a section's name may have.
pub const MAX_SECTION_NAME: usize = 64;

/// The last 8 bytes of the trailer, by which a file is known to carry sections.
const MAGIC: [u8; 8] = *b"GDLSECT1";

/// The trailer's length.
const TRAILER_LEN: u64 = 48;

/// Where the trailer keeps the table's length, as 8 little-endian bytes.
const TRAILER_TABLE_LEN: usize = 0;

/// Where the trailer keeps the table's SHA-256 digest.
const TRAILER_DIGEST: Range<usize> = 8..40;

/// Where the trailer keeps [`MAGIC`].
const TRAILER_MAGIC: Range<usize> = 40..48;

/// The schema of the tables this launcher reads.
const TABLE_SCHEMA: u64 = 1;

/// What a PE's certificate table starts at a multiple of.
const CERT_ALIGNMENT: u64 = 8;

/// The most zero bytes that may lie between the trailer and the certificate table, or the end of a
/// file without one: a signer pads the file to a multiple of [`CERT_ALIGNMENT`] before its
/// certificate table, and removing the signature leaves that padding behind.
const MAX_CERT_PADDING: u64 = 7;

/// The characters a section's name may have: printable ASCII, from the space to the tilde.
const NAME_CHARACTERS: RangeInclusive<u8> = b' '..=b'~';

/// The path on Linux of the running program's own file, whatever path the file has now.
#[cfg(target_os = "linux")]
const OWN_EXECUTABLE: &str = "/proc/self/exe";

/// One section of a launcher file, as its table lists it.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Section {
    /// The section's name.
    pub name: String,
    /// Where its bytes begin in the file.
    pub offset: u64,
    /// How many bytes it holds.
    pub size: u64,
    /// The SHA-256 digest of its bytes.
    pub sha256: [u8; SHA256_LEN],
}

/// The sections a launcher file carries, found and their table checked, read from the source they
/// were found in.
#[derive(Debug)]
pub struct Sections<R> {
    /// The launcher file.
    source: R,
    /// Where the program ends and the first section begins.
    program_length: u64,
    /// The sections, in the order of the file.
    sections: Vec<Section>,
}

impl Sections<File> {
    /// Finds the sections of the running program's own file, as [`of_path`](Self::of_path) does:
    /// on Linux of `/proc/self/exe`, which is the running program's file whatever path it has now,
    /// and elsewhere of the path the program was started from. A program calls this once, as it
    /// starts, and keeps what it gives back: once an update has renamed the launcher aside, its
    /// old path names the new launcher.
    pub fn of_running_launcher() -> Result<Option<Sections<File>>, SectionsError> {
        Self::of_path(&own_executable()?)
    }

    /// Opens the file at `path` and finds its sections, as [`find`](Sections::find) does. The
    /// file is opened as std opens any file to read it, sharing reading, writing and deletion with
    /// other handles: on Windows a handle that did not share deletion would keep the file from
    /// being renamed, and an update renames the running launcher aside while that launcher's
    /// sections are held open.
    pub fn of_path(path: &Path) -> Result<Option<Sections<File>>, SectionsError> {
        Sections::find(File::open(path)?)
    }

    /// Finds the sections `file` carries, as [`find`](Sections::find) does, and refuses a file
    /// that carries none.
    pub fn of_file(file: File) -> Result<Sections<File>, SectionsError> {
        Sections::find(file)?.ok_or(SectionsError::NoSections)
    }
}

impl<R> Sections<R> {
    /// The sections, in the order of the file.
    pub fn sections(&self) -> &[Section] {
        &self.sections
    }

    /// Where the program ends and the first section begins, as the program's headers say and the
    /// table records.
    pub fn program_length(&self) -> u64 {
        self.program_length
    }
}

impl<R: Read + Seek> Sections<R> {
    /// Finds the sections `source` carries and checks their table, refusing it for the first rule
    /// it breaks. The program must be a PE, or an x86-64 ELF64 little-endian program, whose headers
    /// say where it ends; a PE's certificate table must start at a multiple of 8, at or after the
    /// program's end, and end the file. The trailer must end where the certificate table starts,
    /// or the file ends, less up to 7 zero bytes, and give a table of at most [`MAX_TABLE_LEN`]
    /// bytes between the program and itself, with the table's digest. The table must be JSON of
    /// schema 1 that records the program's length as the headers give it, and lists each
    /// section's name, of 1 to [`MAX_SECTION_NAME`] characters of printable ASCII and unlike any
    /// other's, its SHA-256 digest in lower-case hexadecimal, a size of at most
    /// [`MAX_SECTION_LEN`] bytes, and an offset where the program or the section before it ends;
    /// and the table must start where the last section ends. Keys the table does not know, and
    /// sections of names this launcher does not know, are allowed. The sections' own bytes are
    /// read by [`verify`](Self::verify).
    ///
    /// Gives back `None` for a file without a trailer: a program that carries no sections.
    pub fn find(mut source: R) -> Result<Option<Sections<R>>, SectionsError> {
        let file_len = source.seek(SeekFrom::End(0))?;
        let program = program::layout(&mut source, file_len)?;
        let data_end = match program.certificates {
            Some((offset, size)) => certificate_table(offset, size, program.end, file_len)?,
            None => file_len,
        };
        let trailer_end = data_end - trailing_zeros(&mut source, program.end, data_end)?;
        let Some(trailer_start) = trailer_end
            .checked_sub(TRAILER_LEN)
            .filter(|start| *start >= program.end)
        else {
            return Ok(None);
        };
        let trailer: [u8; TRAILER_LEN as usize] = read_array(&mut source, trailer_start)?;
        if trailer[TRAILER_MAGIC] != MAGIC {
            return Ok(None);
        }
        let table_len = u64_at(&trailer, TRAILER_TABLE_LEN);
        if table_len == 0 || table_len > MAX_TABLE_LEN || table_len > trailer_start - program.end {
            return Err(SectionsError::TableLength { len: table_len });
        }
        let table_start = trailer_start - table_len;
        let table = read_vec(&mut source, table_start, table_len as usize)?;
        if Sha256::digest(&table)[..] != trailer[TRAILER_DIGEST] {
            return Err(SectionsError::TableHash);
        }
        let table = Table::parse(&table)?;
        if table.program_length != program.end {
            return Err(SectionsError::ProgramLength {
                recorded: table.program_length,
                actual: program.end,
            });
        }
        let sections = place(table.sections, program.end, table_start)?;
        Ok(Some(Sections {
            source,
            program_length: program.end,
            sections,
        }))
    }

    /// Reads every section, at most [`BLOCK_LEN`] bytes at a time, and refuses the file for the
    /// first section whose bytes do not have the digest the table gives it. Before each block it
    /// looks at the job's cancel, and after it tells the job's progress how many of all the
    /// sections' bytes are done.
    pub fn verify(&mut self, job: &Job<'_>) -> Result<(), SectionsError> {
        let total: u64 = self.sections.iter().map(|section| section.size).sum();
        let mut block =
            vec![0; usize::try_from(total).map_or(BLOCK_LEN, |total| total.min(BLOCK_LEN))];
        let mut done = 0;
        for section in &self.sections {
            self.source.seek(SeekFrom::Start(section.offset))?;
            let mut hasher = Sha256::new();
            let mut left = section.size;
            while left > 0 {
                job.cancel
                    .check()
                    .map_err(|Cancelled| SectionsError::Cancelled)?;
                let len = block.len().min(usize::try_from(left).unwrap_or(usize::MAX));
                self.source.read_exact(&mut block[..len])?;
                hasher.update(&block[..len]);
                left -= len as u64;
                done += len as u64;
                job.progress.advance(done, total, None);
            }
            if hasher.finalize()[..] != section.sha256 {
                return Err(SectionsError::SectionDigest {
                    name: section.name.as_str().into(),
                });
            }
        }
        Ok(())
    }

    /// The section named `name`, to be read as a file of its own. Its bytes are not checked here;
    /// [`verify`](Self::verify) checks them.
    pub fn section(&mut self, name: &str) -> Result<SectionView<'_, R>, SectionsError> {
        let Some(section) = self.sections.iter().find(|section| section.name == name) else {
            return Err(SectionsError::MissingSection { name: name.into() });
        };
        Ok(SectionView::new(
            &mut self.source,
            section.offset,
            section.size,
        ))
    }

    /// The runtime zip of the section [`RUNTIME_SECTION`], held to the payload's rules as
    /// [`Payload::open`] holds one.
    pub fn runtime(&mut self) -> Result<Payload<SectionView<'_, R>>, PayloadError> {
        Payload::open(self.section(RUNTIME_SECTION)?)
    }
}

/// The path of the running program's own file.
#[cfg(target_os = "linux")]
fn own_executable() -> io::Result<PathBuf> {
    Ok(PathBuf::from(OWN_EXECUTABLE))
}

/// The path of the running program's own file.
#[cfg(not(target_os = "linux"))]
fn own_executable() -> io::Result<PathBuf> {
    std::env::current_exe()
}

/// Checks where a PE's certificate table lies, at `offset` for `size` bytes, and gives back where
/// it starts, which is where the bytes appended to the program end.
fn certificate_table(
    offset: u64,
    size: u64,
    program_end: u64,
    file_len: u64,
) -> Result<u64, SectionsError> {
    if !offset.is_multiple_of(CERT_ALIGNMENT) {
        return Err(SectionsError::CertificatesUnaligned);
    }
    if offset.checked_add(size) != Some(file_len) {
        return Err(SectionsError::CertificatesNotAtEnd);
    }
    if offset < program_end {
        return Err(SectionsError::CertificatesInsideImage);
    }
    Ok(offset)
}

/// How many zero bytes end the bytes before `data_end`: at most [`MAX_CERT_PADDING`], and none
/// that belong to the program. The trailer's last byte is not zero, so they are padding.
fn trailing_zeros<R: Read + Seek>(
    source: &mut R,
    program_end: u64,
    data_end: u64,
) -> io::Result<u64> {
    let probe = MAX_CERT_PADDING.min(data_end - program_end);
    let tail = read_vec(source, data_end - probe, probe as usize)?;
    Ok(tail.iter().rev().take_while(|byte| **byte == 0).count() as u64)
}

/// Holds the sections the table lists to the rules, in order, and gives them back: each with a
/// name a section may have and no other section has, a digest in lower-case hexadecimal, a size of
/// at most [`MAX_SECTION_LEN`], and its bytes where the program or the section before it ends and
/// before the table at `table_start`; and the table right after the last.
fn place(
    listed: Vec<Listed>,
    program_end: u64,
    table_start: u64,
) -> Result<Vec<Section>, SectionsError> {
    let mut sections: Vec<Section> = Vec::with_capacity(listed.len());
    let mut end = program_end;
    for entry in listed {
        let taken = sections.iter().any(|section| section.name == entry.name);
        if taken || !is_section_name(&entry.name) {
            return Err(SectionsError::SectionName {
                name: entry.name.into(),
            });
        }
        let Some(sha256) = sha256_of(&entry.sha256) else {
            return Err(SectionsError::SectionDigestText {
                name: entry.name.into(),
            });
        };
        if entry.size > MAX_SECTION_LEN {
            return Err(SectionsError::SectionTooLarge {
                name: entry.name.into(),
            });
        }
        if entry.offset != end {
            return Err(
                entry.misplaced("does not start where the program or the section before it ends")
            );
        }
        end = match entry.offset.checked_add(entry.size) {
            Some(next) if next <= table_start => next,
            _ => return Err(entry.misplaced("overlaps the table")),
        };
        sections.push(Section {
            name: entry.name,
            offset: entry.offset,
            size: entry.size,
            sha256,
        });
    }
    if end != table_start {
        return Err(SectionsError::TableNotAfterSections);
    }
    Ok(sections)
}

/// Whether `name` is one a section may have: 1 to [`MAX_SECTION_NAME`] characters, each in
/// [`NAME_CHARACTERS`].
fn is_section_name(name: &str) -> bool {
    (1..=MAX_SECTION_NAME).contains(&name.len())
        && name.bytes().all(|byte| NAME_CHARACTERS.contains(&byte))
}

/// The table as its JSON gives it, before its sections are held to the rules.
struct Table {
    /// Where the table's writer found the program to end.
    program_length: u64,
    /// The sections it lists, in order.
    sections: Vec<Listed>,
}

impl Table {
    /// Reads the table's JSON: an object of schema [`TABLE_SCHEMA`], whose schema is judged before
    /// anything else it holds, so that a later schema of another shape is reported as one, with
    /// the program's length as an unsigned integer and its sections as an array. Keys it does not
    /// know are left alone.
    fn parse(bytes: &[u8]) -> Result<Table, SectionsError> {
        let table = serde_json::from_slice(bytes).map_err(table_json)?;
        let Value::Object(mut table) = table else {
            return Err(table_json("the table is not a JSON object"));
        };
        let schema = unsigned(&table, "schema")?;
        if schema != TABLE_SCHEMA {
            return Err(SectionsError::Schema { found: schema });
        }
        let program_length = unsigned(&table, "programLength")?;
        let Some(Value::Array(sections)) = table.remove("sections") else {
            return Err(table_json(r#""sections" is not an array"#));
        };
        let sections = sections
            .into_iter()
            .map(Listed::parse)
            .collect::<Result<_, _>>()?;
        Ok(Table {
            program_length,
            sections,
        })
    }
}

/// A section as the table lists it, before it is held to the rules.
struct Listed {
    /// Its name.
    name: String,
    /// Where it begins in the file.
    offset: u64,
    /// How many bytes it holds.
    size: u64,
    /// Its digest, as the table writes it.
    sha256: String,
}

impl Listed {
    /// Reads one section of the table's JSON: an object whose name and digest are strings and
    /// whose offset and size are unsigned integers. Keys it does not know are left alone.
    fn parse(entry: Value) -> Result<Listed, SectionsError> {
        let Value::Object(mut entry) = entry else {
            return Err(table_json("a section is not a JSON object"));
        };
        Ok(Listed {
            name: text(&mut entry, "name")?,
            offset: unsigned(&entry, "offset")?,
            size: unsigned(&entry, "size")?,
            sha256: text(&mut entry, "sha256")?,
        })
    }

    /// The refusal of this section, which does not lie where it must, for the reason `reason`
    /// gives.
    fn misplaced(self, reason: &'static str) -> SectionsError {
        SectionsError::SectionPlace {
            name: self.name.into(),
            offset: self.offset,
            size: self.size,
            reason,
        }
    }
}

/// The unsigned integer under `key`.
fn unsigned(object: &Map<String, Value>, key: &str) -> Result<u64, SectionsError> {
    object
        .get(key)
        .and_then(Value::as_u64)
        .ok_or_else(|| table_json(format_args!("{key:?} is not an unsigned integer")))
}

/// The string under `key`, taken out of `object`.
fn text(object: &mut Map<String, Value>, key: &str) -> Result<String, SectionsError> {
    match object.remove(key) {
        Some(Value::String(text)) => Ok(text),
        _ => Err(table_json(format_args!("{key:?} is not a string"))),
    }
}

/// The refusal of a table whose JSON is not one this launcher reads, for the reason `detail`
/// gives.
fn table_json(detail: impl fmt::Display) -> SectionsError {
    SectionsError::TableJson {
        detail: detail.to_string().into(),
    }
}

/// The `N` bytes of `source` at `at`.
fn read_array<const N: usize, R: Read + Seek>(source: &mut R, at: u64) -> io::Result<[u8; N]> {
    let mut bytes = [0; N];
    source.seek(SeekFrom::Start(at))?;
    source.read_exact(&mut bytes)?;
    Ok(bytes)
}

/// The `len` bytes of `source` at `at`.
fn read_vec<R: Read + Seek>(source: &mut R, at: u64, len: usize) -> io::Result<Vec<u8>> {
    let mut bytes = vec![0; len];
    source.seek(SeekFrom::Start(at))?;
    source.read_exact(&mut bytes)?;
    Ok(bytes)
}

/// The little-endian field of 16 bits at `at`.
fn u16_at(bytes: &[u8], at: usize) -> u16 {
    u16::from_le_bytes([bytes[at], bytes[at + 1]])
}

/// The little-endian field of 32 bits at `at`.
fn u32_at(bytes: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
}

/// The little-endian field of 64 bits at `at`.
fn u64_at(bytes: &[u8], at: usize) -> u64 {
    let mut field = [0; 8];
    field.copy_from_slice(&bytes[at..at + 8]);
    u64::from_le_bytes(field)
}

/// Why a launcher file's sections are refused: the rule the file breaks, and the values that break
/// it, as technical detail for logs. [`SectionsError::rule`] names the rule.
#[derive(Debug, thiserror::Error)]
pub enum SectionsError {
    /// The file could not be read.
    #[error("the launcher file cannot be read: {0}")]
    Io(#[from] io::Error),
    /// The file is neither a PE nor an x86-64 ELF64 little-endian program.
    #[error("not a PE or ELF program")]
    UnknownProgram,
    /// A PE's headers do not say where it ends.
    #[error("{detail}")]
    PeHeaders {
        /// What is wrong with them.
        detail: &'static str,
    },
    /// An ELF program's headers do not say where it ends.
    #[error("{detail}")]
    ElfHeaders {
        /// What is wrong with them.
        detail: &'static str,
    },
    /// The certificate table does not start at a multiple of 8.
    #[error("the certificate table is not quadword-aligned")]
    CertificatesUnaligned,
    /// Bytes follow the certificate table, or it runs past the file.
    #[error("the certificate table does not end the file")]
    CertificatesNotAtEnd,
    /// The certificate table starts before the program ends.
    #[error("the certificate table starts inside the image")]
    CertificatesInsideImage,
    /// The trailer gives a table of no bytes, of more than [`MAX_TABLE_LEN`], or of more than lie
    /// between the program and the trailer.
    #[error("table length {len} out of bounds")]
    TableLength {
        /// The length the trailer gives.
        len: u64,
    },
    /// The table's bytes do not have the digest the trailer gives.
    #[error("the table's SHA-256 does not match the trailer")]
    TableHash,
    /// The table is not JSON of the shape this launcher reads.
    #[error("table JSON: {detail}")]
    TableJson {
        /// What is wrong with it.
        detail: Box<str>,
    },
    /// The table is of a schema this launcher does not read.
    #[error("table schema {found} is not 1")]
    Schema {
        /// The table's schema.
        found: u64,
    },
    /// The table records another length of the program than its headers give.
    #[error("the table records program length {recorded}, the program ends at {actual}")]
    ProgramLength {
        /// The length the table records.
        recorded: u64,
        /// Where the program's headers say it ends.
        actual: u64,
    },
    /// A section's name is empty, too long, not printable ASCII, or another section's.
    #[error("section name {name:?} is empty, too long, not printable ASCII or repeated")]
    SectionName {
        /// The name.
        name: Box<str>,
    },
    /// A section's digest is not 64 lower-case hexadecimal digits.
    #[error("section {name:?}: bad sha256")]
    SectionDigestText {
        /// The section's name.
        name: Box<str>,
    },
    /// A section holds more than [`MAX_SECTION_LEN`] bytes.
    #[error("section {name:?} is larger than 2 GiB")]
    SectionTooLarge {
        /// The section's name.
        name: Box<str>,
    },
    /// A section does not start where the program or the section before it ends, or runs into
    /// the table.
    #[error("section {name:?} at [{offset}, +{size}) {reason}")]
    SectionPlace {
        /// The section's name.
        name: Box<str>,
        /// Where the table says it begins.
        offset: u64,
        /// How many bytes the table says it holds.
        size: u64,
        /// Where it should lie and does not.
        reason: &'static str,
    },
    /// Bytes lie between the last section, or the program, and the table.
    #[error("the table does not start where the last section ends")]
    TableNotAfterSections,
    /// A section's bytes do not have the digest the table gives them.
    #[error("section {name:?}: SHA-256 mismatch")]
    SectionDigest {
        /// The section's name.
        name: Box<str>,
    },
    /// The file carries no section of the name asked for.
    #[error("no section {name:?}")]
    MissingSection {
        /// The name asked for.
        name: Box<str>,
    },
    /// The file carries no sections at all.
    #[error("the file carries no sections")]
    NoSections,
    /// The job was asked to stop before every section was read.
    #[error("cancelled")]
    Cancelled,
}

impl SectionsError {
    /// The rule the file breaks, as a stable camelCase name for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            SectionsError::Io(_) => "io",
            SectionsError::UnknownProgram => "unknownProgram",
            SectionsError::PeHeaders { .. } => "peHeaders",
            SectionsError::ElfHeaders { .. } => "elfHeaders",
            SectionsError::CertificatesUnaligned => "certificatesUnaligned",
            SectionsError::CertificatesNotAtEnd => "certificatesNotAtEnd",
            SectionsError::CertificatesInsideImage => "certificatesInsideImage",
            SectionsError::TableLength { .. } => "tableLength",
            SectionsError::TableHash => "tableHash",
            SectionsError::TableJson { .. } => "tableJson",
            SectionsError::Schema { .. } => "schema",
            SectionsError::ProgramLength { .. } => "programLength",
            SectionsError::SectionName { .. } => "sectionName",
            SectionsError::SectionDigestText { .. } => "sectionDigestText",
            SectionsError::SectionTooLarge { .. } => "sectionTooLarge",
            SectionsError::SectionPlace { .. } => "sectionPlace",
            SectionsError::TableNotAfterSections => "tableNotAfterSections",
            SectionsError::SectionDigest { .. } => "sectionDigest",
            SectionsError::MissingSection { .. } => "missingSection",
            SectionsError::NoSections => "noSections",
            SectionsError::Cancelled => "cancelled",
        }
    }
}

#[cfg(test)]
mod tests {
    use std::error::Error;
    use std::fs::{self, File};
    use std::io::{self, Cursor, Read, Seek, SeekFrom};
    use std::ops::Range;
    use std::panic;
    use std::sync::Mutex;

    use gdl_files::{
        BLOCK_LEN, Cancel, Job, NoProgress, PhaseId, Progress, RelPath, rename_noreplace,
    };
    use gdl_fixtures::{
        FAKE_CERTIFICATE_LEN, RuntimeFiles, SparseFile, append_sections, append_sections_at,
        fake_sign, table_and_trailer, tiny_elf, tiny_pe,
    };
    use serde_json::{Value, json};
    use sha2::{Digest, Sha256};

    use super::{
        MAX_SECTION_LEN, MAX_SECTION_NAME, MAX_TABLE_LEN, RUNTIME_SECTION, Section, Sections,
        SectionsError, WINDOW_SECTION,
    };
    use crate::PayloadError;

    /// The platforms a release is built for, as the release scripts name them.
    const WINDOWS: &str = "windows-x64";
    const LINUX: &str = "linux-x64";

    /// The version the test runtimes carry, and that of a newer one.
    const VERSION: &str = "1.0.0-beta";
    const NEWER: &str = "1.0.1";

    /// Where the tiny PE and the tiny ELF program end.
    const PE_END: u64 = 0x400;
    const ELF_END: u64 = 0x280;

    /// Where the tiny PE keeps its certificate table's data directory: index 4 of the data
    /// directories, which begin 112 bytes into its PE32+ optional header, at 0x58.
    const CERTIFICATE_DIRECTORY: usize = 0x58 + 112 + 4 * 8;

    /// The trailer's length.
    const TRAILER: usize = 48;

    /// The window program the tests' launcher files carry beside their runtime.
    const WINDOW: &[u8] = b"the window program";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// A job nobody asks to stop and nobody follows.
    fn quiet(cancel: &Cancel) -> Job<'_> {
        Job {
            cancel,
            progress: &NoProgress,
        }
    }

    /// The sections `file` carries, found and their table checked; a file that carries none is
    /// refused, as [`Sections::of_file`] refuses it.
    fn open(file: Vec<u8>) -> Result<Sections<Cursor<Vec<u8>>>, SectionsError> {
        Sections::find(Cursor::new(file))?.ok_or(SectionsError::NoSections)
    }

    /// The same, with every section's bytes read and checked.
    fn open_and_verify(file: Vec<u8>) -> Result<Sections<Cursor<Vec<u8>>>, SectionsError> {
        let mut sections = open(file)?;
        sections.verify(&quiet(&Cancel::default()))?;
        Ok(sections)
    }

    /// The rule `file` is refused for, sections and all read, or "accepted".
    fn rule_of(file: Vec<u8>) -> &'static str {
        match open_and_verify(file) {
            Ok(_) => "accepted",
            Err(error) => error.rule(),
        }
    }

    /// The sections as the fixtures' writer lists them.
    fn as_sections(listed: Vec<(String, u64, u64, [u8; 32])>) -> Vec<Section> {
        listed
            .into_iter()
            .map(|(name, offset, size, sha256)| Section {
                name,
                offset,
                size,
                sha256,
            })
            .collect()
    }

    /// A launcher file of `program`, with the standard runtime of `platform` and the window
    /// program as its sections, its length `residue` past a multiple of 8; and its sections.
    fn launcher(program: Vec<u8>, platform: &str, residue: u64) -> (Vec<u8>, Vec<Section>) {
        let mut file = program;
        let runtime = RuntimeFiles::standard(platform, VERSION).zip();
        let sections: [(&str, &[u8]); 2] = [(RUNTIME_SECTION, &runtime), (WINDOW_SECTION, WINDOW)];
        let listed = append_sections(&mut file, &sections, residue);
        (file, as_sections(listed))
    }

    /// Checks what a launcher file [`launcher`] wrote holds: the window program as it was
    /// written, and the standard runtime of `platform`, which keeps to the payload's rules.
    fn check_contents<R: Read + Seek>(sections: &mut Sections<R>, platform: &str) {
        let mut window = Vec::new();
        let mut view = sections.section(WINDOW_SECTION).unwrap();
        view.read_to_end(&mut window).unwrap();
        assert_eq!(window, WINDOW);
        let mut payload = sections.runtime().unwrap();
        assert_eq!(payload.build_info().version, VERSION);
        assert_eq!(payload.build_info().platform.as_str(), platform);
        payload.verify(&quiet(&Cancel::default())).unwrap();
    }

    fn sha256(bytes: &[u8]) -> [u8; 32] {
        Sha256::digest(bytes).into()
    }

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|byte| format!("{byte:02x}")).collect()
    }

    /// Bytes that are not all alike, as a runtime zip's are not.
    fn noise(len: usize) -> Vec<u8> {
        (0..len).map(|index| (index * 151 % 251) as u8).collect()
    }

    /// A table written by hand, as a writer records it: the program's length and each section's
    /// name, offset, size and digest as text.
    fn table(program_length: u64, sections: &[(&str, u64, u64, &str)]) -> Vec<u8> {
        let sections: Vec<Value> = sections
            .iter()
            .map(|(name, offset, size, sha256)| {
                json!({"name": name, "offset": offset, "size": size, "sha256": sha256})
            })
            .collect();
        let table = json!({"schema": 1, "programLength": program_length, "sections": sections});
        serde_json::to_vec(&table).unwrap()
    }

    /// `program`, then `appended`, then `table` closed by its trailer.
    fn written(program: Vec<u8>, appended: &[u8], table: &[u8]) -> Vec<u8> {
        let mut file = program;
        file.extend_from_slice(appended);
        file.extend(table_and_trailer(table));
        file
    }

    /// Sets the tiny PE's certificate table's data directory.
    fn set_certificates(pe: &mut [u8], offset: u32, size: u32) {
        let directory = CERTIFICATE_DIRECTORY;
        pe[directory..directory + 4].copy_from_slice(&offset.to_le_bytes());
        pe[directory + 4..directory + 8].copy_from_slice(&size.to_le_bytes());
    }

    /// The ELF header of a program that ends at `end`, which is all a test needs to hold of it:
    /// no program headers, and one section header, of the null section, in the 64 bytes before
    /// `end`, where a file that holds nothing else reads zeros.
    fn elf_header_ending_at(end: u64) -> Vec<u8> {
        let mut header = tiny_elf()[..64].to_vec();
        header[40..48].copy_from_slice(&(end - 64).to_le_bytes()); // e_shoff
        header[56..58].copy_from_slice(&0u16.to_le_bytes()); // e_phnum
        header[60..62].copy_from_slice(&1u16.to_le_bytes()); // e_shnum
        header[62..64].copy_from_slice(&0u16.to_le_bytes()); // e_shstrndx
        header
    }

    #[test]
    fn reads_an_elf_at_every_residue() {
        for residue in 0..8 {
            let (file, listed) = launcher(tiny_elf(), LINUX, residue);
            assert_eq!(file.len() as u64 % 8, residue);
            let mut sections =
                open_and_verify(file).unwrap_or_else(|error| panic!("residue {residue}: {error}"));
            assert_eq!(sections.program_length(), ELF_END, "residue {residue}");
            assert_eq!(sections.sections(), listed, "residue {residue}");
            assert_eq!(sections.sections()[0].offset, ELF_END);
            check_contents(&mut sections, LINUX);
        }
    }

    #[test]
    fn reads_a_pe_at_every_residue_signed_and_unsigned() {
        for residue in 0..8 {
            let (unsigned, listed) = launcher(tiny_pe(), WINDOWS, residue);
            let mut signed = unsigned.clone();
            let padding = fake_sign(&mut signed);
            assert_eq!(padding, (8 - residue) % 8);
            assert_eq!(
                signed.len() as u64,
                unsigned.len() as u64 + padding + FAKE_CERTIFICATE_LEN
            );
            for (kind, file) in [("unsigned", unsigned), ("signed", signed)] {
                let case = format!("{kind}, residue {residue}");
                let mut sections =
                    open_and_verify(file).unwrap_or_else(|error| panic!("{case}: {error}"));
                assert_eq!(sections.program_length(), PE_END, "{case}");
                assert_eq!(sections.sections(), listed, "{case}");
                check_contents(&mut sections, WINDOWS);
            }
        }
    }

    #[test]
    fn a_signed_file_ends_in_its_certificate_not_its_trailer() {
        let (mut file, listed) = launcher(tiny_pe(), WINDOWS, 0);
        let unsigned = file.len();
        assert_eq!(file[unsigned - 8..], *b"GDLSECT1");
        // Written at a multiple of 8, the file takes its certificate table straight after its
        // trailer, and ends in the certificate table.
        assert_eq!(fake_sign(&mut file), 0);
        assert_eq!(file.len() as u64, unsigned as u64 + FAKE_CERTIFICATE_LEN);
        assert_ne!(file[file.len() - 8..], *b"GDLSECT1");
        let sections = Sections::find(Cursor::new(file)).unwrap().unwrap();
        assert_eq!(sections.sections(), listed);
    }

    #[test]
    fn tolerates_zeros_left_by_removing_a_signature() {
        let (mut file, listed) = launcher(tiny_pe(), WINDOWS, 3);
        file.extend_from_slice(&[0; 5]);
        assert_eq!(open_and_verify(file).unwrap().sections(), listed);

        // What removing a signature leaves at every residue: the signer's zeros, and the
        // certificate table's data directory cleared.
        for residue in 0..8 {
            let (unsigned, listed) = launcher(tiny_pe(), WINDOWS, residue);
            let mut file = unsigned.clone();
            let padding = fake_sign(&mut file);
            file.truncate(unsigned.len() + padding as usize);
            set_certificates(&mut file, 0, 0);
            assert_eq!(file[..unsigned.len()], unsigned[..], "residue {residue}");
            let sections =
                open_and_verify(file).unwrap_or_else(|error| panic!("residue {residue}: {error}"));
            assert_eq!(sections.sections(), listed, "residue {residue}");
        }

        // Up to 7 zeros after the trailer, after a PE or an ELF program; past them, the trailer
        // is not where a reader looks for it.
        for (program, platform) in [(tiny_pe(), WINDOWS), (tiny_elf(), LINUX)] {
            let (file, listed) = launcher(program, platform, 0);
            for zeros in 0..=7 {
                let mut padded = file.clone();
                padded.resize(file.len() + zeros, 0);
                let sections = open_and_verify(padded)
                    .unwrap_or_else(|error| panic!("{platform}, {zeros} zeros: {error}"));
                assert_eq!(sections.sections(), listed, "{platform}, {zeros} zeros");
            }
            let mut padded = file.clone();
            padded.resize(file.len() + 8, 0);
            assert!(Sections::find(Cursor::new(padded)).unwrap().is_none());
        }
    }

    #[test]
    fn refuses_data_after_the_certificate_table() {
        let (mut file, _) = launcher(tiny_pe(), WINDOWS, 0);
        fake_sign(&mut file);
        open_and_verify(file.clone()).unwrap();
        file.extend_from_slice(b"appended after signing");
        assert_eq!(rule_of(file), "certificatesNotAtEnd");
    }

    #[test]
    fn refuses_a_misplaced_certificate_table() {
        let (mut file, _) = launcher(tiny_pe(), WINDOWS, 0);
        fake_sign(&mut file);
        let len = u32::try_from(file.len()).unwrap();
        let size = FAKE_CERTIFICATE_LEN as u32;
        let table = len - size;
        let image = PE_END as u32;
        for (what, offset, size, rule) in [
            ("as it was signed", table, size, "accepted"),
            ("unaligned", table + 1, size - 1, "certificatesUnaligned"),
            (
                "at a multiple of 4 only",
                table + 4,
                size - 4,
                "certificatesUnaligned",
            ),
            ("past the file", table, size + 8, "certificatesNotAtEnd"),
            ("short of the file", table, size - 8, "certificatesNotAtEnd"),
            (
                "as long as 32 bits go",
                table,
                u32::MAX,
                "certificatesNotAtEnd",
            ),
            (
                "inside the image",
                0x200,
                len - 0x200,
                "certificatesInsideImage",
            ),
            ("from the image's end", image, len - image, "noSections"),
        ] {
            let mut changed = file.clone();
            set_certificates(&mut changed, offset, size);
            assert_eq!(rule_of(changed), rule, "{what}");
        }
        // A size of nought means no certificate table, wherever the directory points: the reader
        // then looks before the file's end, which the certificate entry takes.
        let mut changed = file.clone();
        set_certificates(&mut changed, table + 1, 0);
        assert_eq!(rule_of(changed), "noSections");
    }

    #[test]
    fn refuses_a_changed_byte_between_trailer_and_certificates() {
        let (unsigned, _) = launcher(tiny_pe(), WINDOWS, 5);
        let mut signed = unsigned.clone();
        assert_eq!(fake_sign(&mut signed), 3);
        open_and_verify(signed.clone()).unwrap();
        for at in unsigned.len()..unsigned.len() + 3 {
            let mut changed = signed.clone();
            changed[at] = 1;
            assert_eq!(rule_of(changed), "noSections", "byte {at}");
        }
    }

    #[test]
    fn refuses_a_changed_section_or_table() {
        let mut file = tiny_pe();
        append_sections(&mut file, &[(RUNTIME_SECTION, &[1; 64])], 0);
        open_and_verify(file.clone()).unwrap();
        let mut section = file.clone();
        section[PE_END as usize + 10] ^= 1;
        match open_and_verify(section) {
            Err(SectionsError::SectionDigest { name }) => assert_eq!(&*name, RUNTIME_SECTION),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
        let table_byte = file.len() - TRAILER - 3;
        file[table_byte] ^= 1;
        assert_eq!(rule_of(file), "tableHash");
    }

    #[test]
    fn refuses_a_section_inside_the_program() {
        let pe = tiny_pe();
        let digest = hex(&sha256(&pe[0x300..0x303]));
        let file = written(
            pe,
            b"",
            &table(PE_END, &[(RUNTIME_SECTION, 0x300, 3, &digest)]),
        );
        match open(file) {
            Err(error @ SectionsError::SectionPlace { .. }) => assert_eq!(
                error.to_string(),
                "section \"runtime.zip\" at [768, +3) does not start where the program or the \
                 section before it ends"
            ),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    #[test]
    fn offsets_above_4_gib_read_back() {
        // 4.5 GiB and 3 bytes of program, of which only its header is held.
        let program_length = (9 << 29) + 3;
        let mut tail = Vec::new();
        let sections: [(&str, &[u8]); 2] = [(RUNTIME_SECTION, b"zip"), (WINDOW_SECTION, b"elf")];
        let listed = as_sections(append_sections_at(&mut tail, program_length, &sections, 0));
        assert!(listed[0].offset > u64::from(u32::MAX));
        let file = SparseFile {
            len: program_length + tail.len() as u64,
            chunks: vec![
                (0, elf_header_ending_at(program_length)),
                (program_length, tail),
            ],
            at: 0,
        };
        let mut found = Sections::find(file).unwrap().unwrap();
        assert_eq!(found.program_length(), program_length);
        assert_eq!(found.sections(), listed);
        found.verify(&quiet(&Cancel::default())).unwrap();
        let mut window = Vec::new();
        let mut view = found.section(WINDOW_SECTION).unwrap();
        view.read_to_end(&mut window).unwrap();
        assert_eq!(window, b"elf");
    }

    #[test]
    fn refuses_a_section_above_2_gib() {
        assert_eq!(MAX_SECTION_LEN, 2 * 1024 * 1024 * 1024);
        let digest = "0".repeat(64);
        for (size, accepted) in [(MAX_SECTION_LEN, true), (MAX_SECTION_LEN + 1, false)] {
            // The program, then a section of `size` zeros, which the file need not hold.
            let listed = table(ELF_END, &[(RUNTIME_SECTION, ELF_END, size, &digest)]);
            let tail = table_and_trailer(&listed);
            let file = SparseFile {
                len: ELF_END + size + tail.len() as u64,
                chunks: vec![(0, tiny_elf()), (ELF_END + size, tail)],
                at: 0,
            };
            match (Sections::find(file), accepted) {
                (Ok(Some(found)), true) => assert_eq!(found.sections()[0].size, size),
                (Err(SectionsError::SectionTooLarge { name }), false) => {
                    assert_eq!(&*name, RUNTIME_SECTION);
                }
                (other, _) => panic!("{size}: {:?}", other.map(|found| found.is_some())),
            }
        }
    }

    #[test]
    fn refuses_a_gap_before_the_first_section() {
        let digest = hex(&sha256(b"zip"));
        let at_end = table(PE_END, &[(RUNTIME_SECTION, PE_END, 3, &digest)]);
        assert_eq!(rule_of(written(tiny_pe(), b"zip", &at_end)), "accepted");
        let past = table(PE_END, &[(RUNTIME_SECTION, PE_END + 1, 3, &digest)]);
        match open(written(tiny_pe(), b"\xCCzip", &past)) {
            Err(SectionsError::SectionPlace { name, offset, .. }) => {
                assert_eq!((&*name, offset), (RUNTIME_SECTION, PE_END + 1));
            }
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    #[test]
    fn refuses_a_gap_between_sections() {
        let (zip, elf) = (hex(&sha256(b"zip")), hex(&sha256(b"elf")));
        let placed = |second: u64| {
            table(
                ELF_END,
                &[
                    (RUNTIME_SECTION, ELF_END, 3, &zip),
                    (WINDOW_SECTION, second, 3, &elf),
                ],
            )
        };
        let together = written(tiny_elf(), b"zipelf", &placed(ELF_END + 3));
        assert_eq!(rule_of(together), "accepted");
        match open(written(tiny_elf(), b"zip\xCCelf", &placed(ELF_END + 4))) {
            Err(SectionsError::SectionPlace { name, offset, .. }) => {
                assert_eq!((&*name, offset), (WINDOW_SECTION, ELF_END + 4));
            }
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
        // Nor may a section overlap the one before it.
        let overlapping = written(tiny_elf(), b"zipel", &placed(ELF_END + 2));
        assert_eq!(rule_of(overlapping), "sectionPlace");
    }

    #[test]
    fn refuses_a_gap_before_the_table() {
        let digest = hex(&sha256(b"zip"));
        let listed = table(PE_END, &[(RUNTIME_SECTION, PE_END, 3, &digest)]);
        let gap = written(tiny_pe(), b"zip\xCC", &listed);
        assert_eq!(rule_of(gap), "tableNotAfterSections");
        // Without sections, the table follows the program itself.
        let none = table(PE_END, &[]);
        assert_eq!(rule_of(written(tiny_pe(), b"", &none)), "accepted");
        let gap = written(tiny_pe(), b"\xCC", &none);
        assert_eq!(rule_of(gap), "tableNotAfterSections");
        // A section that runs into the table overlaps it.
        let long = table(PE_END, &[(RUNTIME_SECTION, PE_END, 4, &digest)]);
        match open(written(tiny_pe(), b"zip", &long)) {
            Err(error @ SectionsError::SectionPlace { .. }) => assert_eq!(
                error.to_string(),
                r#"section "runtime.zip" at [1024, +4) overlaps the table"#
            ),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    #[test]
    fn refuses_a_wrong_program_length() {
        let digest = hex(&sha256(b"zip"));
        for (program, end) in [(tiny_pe(), PE_END), (tiny_elf(), ELF_END)] {
            for recorded in [end - 1, end + 1, 0] {
                let listed = table(recorded, &[(RUNTIME_SECTION, end, 3, &digest)]);
                match open(written(program.clone(), b"zip", &listed)) {
                    Err(SectionsError::ProgramLength {
                        recorded: found,
                        actual,
                    }) => assert_eq!((found, actual), (recorded, end)),
                    other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
                }
            }
        }
        // The program's length is what its headers give, not where its sections were put.
        let mut file = tiny_elf();
        file.push(0xCC);
        append_sections(&mut file, &[(RUNTIME_SECTION, b"zip")], 0);
        assert_eq!(rule_of(file), "programLength");
    }

    #[test]
    fn refuses_an_unknown_program() {
        let mut elf32 = tiny_elf();
        elf32[4] = 1;
        let mut big_endian = tiny_elf();
        big_endian[5] = 2;
        let mut arm = tiny_elf();
        arm[18..20].copy_from_slice(&183u16.to_le_bytes());
        let mut dos = tiny_pe();
        dos[0x40..0x44].copy_from_slice(b"PX\0\0");
        let text = b"not a program, though long enough to hold a header of 64 bytes".to_vec();
        for (what, program) in [
            ("text", text),
            ("a 32-bit ELF program", elf32),
            ("a big-endian ELF program", big_endian),
            ("an ELF program for another machine", arm),
            ("a DOS program", dos),
        ] {
            let mut file = program;
            append_sections(&mut file, &[(RUNTIME_SECTION, b"zip")], 0);
            assert_eq!(rule_of(file), "unknownProgram", "{what}");
        }
        assert_eq!(rule_of(Vec::new()), "unknownProgram");
    }

    #[test]
    fn an_unsectioned_program_has_no_sections() {
        let mut signed = tiny_pe();
        fake_sign(&mut signed);
        let mut overlaid = tiny_pe();
        overlaid.extend_from_slice(&noise(100));
        let mut padded = tiny_elf();
        padded.extend_from_slice(&[0; 7]);
        for (what, program) in [
            ("a PE", tiny_pe()),
            ("an ELF program", tiny_elf()),
            ("a signed PE", signed),
            ("a PE with other data after it", overlaid),
            ("an ELF program and zeros", padded),
        ] {
            let found = Sections::find(Cursor::new(program)).unwrap();
            assert!(found.is_none(), "{what}");
        }

        // From a file on disk: by its path none are found, and the file itself is refused.
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("program");
        fs::write(&path, tiny_elf()).unwrap();
        assert!(Sections::of_path(&path).unwrap().is_none());
        match Sections::of_file(File::open(&path).unwrap()) {
            Err(error @ SectionsError::NoSections) => assert_eq!(error.rule(), "noSections"),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
        // A file that carries sections gives them both ways.
        let (file, listed) = launcher(tiny_elf(), LINUX, 0);
        fs::write(&path, file).unwrap();
        assert_eq!(
            Sections::of_path(&path).unwrap().unwrap().sections(),
            listed
        );
        let mut found = Sections::of_file(File::open(&path).unwrap()).unwrap();
        assert_eq!(found.sections(), listed);
        found.verify(&quiet(&Cancel::default())).unwrap();
        // A file that is not there cannot be read.
        match Sections::of_path(&dir.path().join("missing")) {
            Err(error @ SectionsError::Io(_)) => {
                let source = error.source().unwrap().downcast_ref::<io::Error>().unwrap();
                assert_eq!(source.kind(), io::ErrorKind::NotFound);
            }
            other => panic!("{:?}", other.map(|found| found.is_some())),
        }
    }

    #[test]
    fn the_running_program_carries_no_sections() {
        // The test program itself, as its linker wrote it, is a program whose headers say it
        // ends where its file does, which is where a launcher file's first section is put.
        let mut own = File::open(super::own_executable().unwrap()).unwrap();
        let len = own.metadata().unwrap().len();
        let program = super::program::layout(&mut own, len).unwrap();
        assert_eq!((program.end, program.certificates), (len, None));
        assert!(Sections::of_running_launcher().unwrap().is_none());
    }

    #[test]
    fn refuses_a_table_above_64_kib() {
        assert_eq!(MAX_TABLE_LEN, 64 * 1024);
        let digest = hex(&sha256(b"zip"));
        for (len, rule) in [
            (MAX_TABLE_LEN, "accepted"),
            (MAX_TABLE_LEN + 1, "tableLength"),
        ] {
            let mut listed = table(ELF_END, &[(RUNTIME_SECTION, ELF_END, 3, &digest)]);
            listed.resize(usize::try_from(len).unwrap(), b' ');
            assert_eq!(rule_of(written(tiny_elf(), b"zip", &listed)), rule, "{len}");
        }
        // Nor may the table be empty, or longer than lies between the program and the trailer;
        // a table that reaches back to the program's end exactly is read.
        match open(written(tiny_elf(), b"", b"")) {
            Err(SectionsError::TableLength { len }) => assert_eq!(len, 0),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
        let file = written(tiny_elf(), b"", &table(ELF_END, &[]));
        assert_eq!(rule_of(file.clone()), "accepted");
        let mut longer = file;
        let at = longer.len() - TRAILER;
        let len = u64::from_le_bytes(longer[at..at + 8].try_into().unwrap());
        longer[at..at + 8].copy_from_slice(&(len + 1).to_le_bytes());
        match open(longer) {
            Err(SectionsError::TableLength { len: found }) => assert_eq!(found, len + 1),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    #[test]
    fn refuses_schema_2_and_a_missing_program_length() {
        let digest = hex(&sha256(b"zip"));
        let section =
            json!({"name": RUNTIME_SECTION, "offset": ELF_END, "size": 3, "sha256": digest});
        let of = |schema: Value, length: Value| json!({"schema": schema, "programLength": length, "sections": [section]});
        let unknown = json!({"schema": 1, "programLength": ELF_END, "writer": "payload.py",
            "sections": [{"name": RUNTIME_SECTION, "offset": ELF_END, "size": 3,
                          "sha256": digest, "compression": null}]});
        for (what, table, rule) in [
            ("schema 1", of(json!(1), json!(ELF_END)), "accepted"),
            ("schema 2", of(json!(2), json!(ELF_END)), "schema"),
            (
                "schema 2 of another shape",
                json!({"schema": 2, "parts": 3}),
                "schema",
            ),
            ("schema 0", of(json!(0), json!(ELF_END)), "schema"),
            (
                "no program length",
                json!({"schema": 1, "sections": [section]}),
                "tableJson",
            ),
            (
                "a program length that is text",
                of(json!(1), json!("640")),
                "tableJson",
            ),
            (
                "a negative program length",
                of(json!(1), json!(-1)),
                "tableJson",
            ),
            (
                "no schema",
                json!({"programLength": ELF_END, "sections": [section]}),
                "tableJson",
            ),
            (
                "a schema that is text",
                of(json!("1"), json!(ELF_END)),
                "tableJson",
            ),
            (
                "a schema that is no integer",
                of(json!(1.5), json!(ELF_END)),
                "tableJson",
            ),
            (
                "no sections",
                json!({"schema": 1, "programLength": ELF_END}),
                "tableJson",
            ),
            (
                "sections that are an object",
                json!({"schema": 1, "programLength": ELF_END, "sections": {"0": section}}),
                "tableJson",
            ),
            ("an array", json!([1, ELF_END]), "tableJson"),
            (
                "a section that is text",
                json!({"schema": 1, "programLength": ELF_END, "sections": [RUNTIME_SECTION]}),
                "tableJson",
            ),
            (
                "unknown keys, in the table and in a section",
                unknown,
                "accepted",
            ),
        ] {
            let bytes = serde_json::to_vec(&table).unwrap();
            assert_eq!(rule_of(written(tiny_elf(), b"zip", &bytes)), rule, "{what}");
        }
        // A section without each of its fields, or holding one of another type.
        for key in ["name", "offset", "size", "sha256"] {
            let mut missing = section.clone();
            missing.as_object_mut().unwrap().remove(key);
            let mut retyped = section.clone();
            retyped[key] = json!([]);
            for entry in [missing, retyped] {
                let table = json!({"schema": 1, "programLength": ELF_END, "sections": [entry]});
                let bytes = serde_json::to_vec(&table).unwrap();
                assert_eq!(
                    rule_of(written(tiny_elf(), b"zip", &bytes)),
                    "tableJson",
                    "{key}"
                );
            }
        }
        // Bytes that are not JSON, JSON followed by more than spaces, and a byte-order mark.
        for bytes in [&b"not JSON"[..], b"{\"schema\": 1} x", b"\xEF\xBB\xBF{}"] {
            assert_eq!(rule_of(written(tiny_elf(), b"", bytes)), "tableJson");
        }
    }

    #[test]
    fn refuses_bad_section_names_and_digest_text() {
        assert_eq!(MAX_SECTION_NAME, 64);
        let digest = hex(&sha256(b"zip"));
        let longest = "n".repeat(MAX_SECTION_NAME);
        let too_long = "n".repeat(MAX_SECTION_NAME + 1);
        for (name, rule) in [
            (longest.as_str(), "accepted"),
            ("a name of printable ASCII: ~!", "accepted"),
            ("", "sectionName"),
            (too_long.as_str(), "sectionName"),
            ("tab\tbetween", "sectionName"),
            ("line\n", "sectionName"),
            ("\u{7f}", "sectionName"),
            ("\u{e9}", "sectionName"),
        ] {
            let listed = table(ELF_END, &[(name, ELF_END, 3, &digest)]);
            assert_eq!(
                rule_of(written(tiny_elf(), b"zip", &listed)),
                rule,
                "{name:?}"
            );
        }
        // Of two sections of one name the second is refused; names that differ in case differ.
        let both = |second: &str| {
            table(
                ELF_END,
                &[
                    (RUNTIME_SECTION, ELF_END, 3, &digest),
                    (second, ELF_END + 3, 3, &digest),
                ],
            )
        };
        match open(written(tiny_elf(), b"zipzip", &both(RUNTIME_SECTION))) {
            Err(SectionsError::SectionName { name }) => assert_eq!(&*name, RUNTIME_SECTION),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
        let cased = written(tiny_elf(), b"zipzip", &both("Runtime.zip"));
        assert_eq!(rule_of(cased), "accepted");

        // A digest is 64 lower-case hexadecimal digits.
        let upper = digest.to_uppercase();
        let long = format!("{digest}0");
        let other = format!("g{}", &digest[1..]);
        for text in [&upper, &digest[..63], &long, &other, ""] {
            let listed = table(ELF_END, &[(RUNTIME_SECTION, ELF_END, 3, text)]);
            match open(written(tiny_elf(), b"zip", &listed)) {
                Err(SectionsError::SectionDigestText { name }) => {
                    assert_eq!(&*name, RUNTIME_SECTION, "{text:?}");
                }
                other => panic!(
                    "{text:?}: {:?}",
                    other.map(|found| found.sections().to_vec())
                ),
            }
        }
    }

    #[test]
    fn keeps_unknown_sections_under_their_hash() {
        let notes = b"notes a later launcher reads".to_vec();
        let runtime = RuntimeFiles::standard(LINUX, VERSION).zip();
        let sections: [(&str, &[u8]); 3] = [
            (RUNTIME_SECTION, &runtime),
            ("notes", &notes),
            (WINDOW_SECTION, WINDOW),
        ];
        let mut file = tiny_elf();
        let listed = as_sections(append_sections(&mut file, &sections, 0));
        let mut found = open_and_verify(file.clone()).unwrap();
        assert_eq!(found.sections(), listed);
        let mut read = Vec::new();
        found
            .section("notes")
            .unwrap()
            .read_to_end(&mut read)
            .unwrap();
        assert_eq!(read, notes);
        check_contents(&mut found, LINUX);
        // Its bytes are checked like any other's.
        let at = usize::try_from(listed[1].offset).unwrap() + 2;
        file[at] ^= 1;
        match open_and_verify(file) {
            Err(SectionsError::SectionDigest { name }) => assert_eq!(&*name, "notes"),
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    #[test]
    fn runtime_needs_runtime_zip() {
        // A file of the window program alone, and one whose runtime is named in another case.
        for name in [WINDOW_SECTION, "Runtime.zip"] {
            let mut file = tiny_elf();
            append_sections(&mut file, &[(name, WINDOW)], 0);
            let mut found = open_and_verify(file).unwrap();
            match found.runtime() {
                Err(error @ PayloadError::Sections(SectionsError::MissingSection { .. })) => {
                    assert_eq!(error.rule(), "missingSection");
                    assert_eq!(error.to_string(), r#"no section "runtime.zip""#);
                    assert!(error.source().is_none());
                }
                other => panic!("{name}: {:?}", other.map(|payload| payload.entries().len())),
            }
            match found.section("nothing") {
                Err(SectionsError::MissingSection { name }) => assert_eq!(&*name, "nothing"),
                other => panic!("{:?}", other.map(|_| "a section")),
            }
        }
        // A runtime section that holds no zip is refused by the payload's rules.
        let mut file = tiny_elf();
        append_sections(&mut file, &[(RUNTIME_SECTION, b"not a zip")], 0);
        let mut found = open_and_verify(file).unwrap();
        match found.runtime() {
            Err(error) => assert_eq!(error.rule(), "archive"),
            Ok(payload) => panic!("{} entries", payload.entries().len()),
        }
    }

    #[test]
    fn the_coff_symbol_table_counts_as_program() {
        // Two symbols at the image's end, then a string table: its 4-byte length, then a name.
        let mut pe = tiny_pe();
        pe[0x4C..0x50].copy_from_slice(&(PE_END as u32).to_le_bytes()); // PointerToSymbolTable
        pe[0x50..0x54].copy_from_slice(&2u32.to_le_bytes()); // NumberOfSymbols
        pe.extend_from_slice(&[0; 2 * 18]);
        let names = b"a_long_symbol_name\0";
        pe.extend_from_slice(&(4 + names.len() as u32).to_le_bytes());
        pe.extend_from_slice(names);
        let end = pe.len() as u64;
        let mut file = pe.clone();
        let listed = as_sections(append_sections(&mut file, &[(RUNTIME_SECTION, b"zip")], 0));
        let found = open_and_verify(file).unwrap();
        assert_eq!(found.program_length(), end);
        assert_eq!(found.sections(), listed);
        // A table that leaves the symbols out of the program is refused.
        let digest = hex(&sha256(b"zip"));
        let short = table(PE_END, &[(RUNTIME_SECTION, end, 3, &digest)]);
        match open(written(pe, b"zip", &short)) {
            Err(SectionsError::ProgramLength { recorded, actual }) => {
                assert_eq!((recorded, actual), (PE_END, end));
            }
            other => panic!("{:?}", other.map(|sections| sections.sections().to_vec())),
        }
    }

    /// Progress that writes down each step it is told, and asks its job to stop at the first when
    /// it holds the job's cancel.
    #[derive(Default)]
    struct Recorded {
        /// Each step's bytes done and total, and whether it named an item.
        steps: Mutex<Vec<(u64, u64, bool)>>,
        /// The cancel of the job to stop.
        stopping: Option<Cancel>,
    }

    impl Recorded {
        fn steps(&self) -> Vec<(u64, u64, bool)> {
            self.steps.lock().unwrap().clone()
        }
    }

    impl Progress for Recorded {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, done: u64, total: u64, item: Option<&RelPath>) {
            self.steps
                .lock()
                .unwrap()
                .push((done, total, item.is_some()));
            if let Some(cancel) = &self.stopping {
                cancel.cancel();
            }
        }
    }

    /// The bytes of the runtime section [`with_large_section`] writes: three blocks and a few.
    const LARGE: usize = 3 * BLOCK_LEN + 5;

    /// A launcher file whose runtime section holds [`LARGE`] bytes, then the window program.
    fn with_large_section() -> Vec<u8> {
        let large = noise(LARGE);
        let mut file = tiny_elf();
        let sections: [(&str, &[u8]); 2] = [(RUNTIME_SECTION, &large), (WINDOW_SECTION, WINDOW)];
        append_sections(&mut file, &sections, 0);
        file
    }

    #[test]
    fn verify_stops_when_cancelled() {
        let mut found = open(with_large_section()).unwrap();
        let total = (LARGE + WINDOW.len()) as u64;
        let cancel = Cancel::default();
        let progress = Recorded {
            stopping: Some(cancel.clone()),
            ..Recorded::default()
        };
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        match found.verify(&job) {
            Err(error @ SectionsError::Cancelled) => {
                assert_eq!(
                    (error.rule(), error.to_string()),
                    ("cancelled", "cancelled".into())
                );
            }
            other => panic!("{other:?}"),
        }
        // Asked to stop as it told its first block, it read no other.
        assert_eq!(progress.steps(), [(BLOCK_LEN as u64, total, false)]);
        found.verify(&job).unwrap_err();
        assert_eq!(progress.steps().len(), 1);
        // The file is as it was: a job nobody stops reads it whole.
        found.verify(&quiet(&Cancel::default())).unwrap();
    }

    #[test]
    fn verify_advances_in_blocks() {
        let mut found = open(with_large_section()).unwrap();
        let total = (LARGE + WINDOW.len()) as u64;
        let block = BLOCK_LEN as u64;
        let progress = Recorded::default();
        let cancel = Cancel::default();
        let job = Job {
            cancel: &cancel,
            progress: &progress,
        };
        found.verify(&job).unwrap();
        // A whole block at a time, then what is left of the runtime, then the window program,
        // each step out of the bytes of every section, naming no item.
        let expected: Vec<(u64, u64, bool)> = [block, 2 * block, 3 * block, LARGE as u64, total]
            .into_iter()
            .map(|done| (done, total, false))
            .collect();
        assert_eq!(progress.steps(), expected);
    }

    /// A reader of a file that fails every read starting inside `bad`, as a disk does that cannot
    /// read part of a file.
    struct Unreadable {
        file: Cursor<Vec<u8>>,
        bad: Range<u64>,
    }

    impl Read for Unreadable {
        fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
            if self.bad.contains(&self.file.position()) {
                return Err(io::Error::other("unreadable sector"));
            }
            self.file.read(buffer)
        }
    }

    impl Seek for Unreadable {
        fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
            self.file.seek(to)
        }
    }

    #[test]
    fn a_reader_that_fails_is_an_io_error() {
        let (file, listed) = launcher(tiny_pe(), WINDOWS, 0);
        let unreadable = |bad: Range<u64>| Unreadable {
            file: Cursor::new(file.clone()),
            bad,
        };
        let is_io = |error: &SectionsError| match error {
            SectionsError::Io(source) => source.to_string() == "unreadable sector",
            _ => false,
        };
        // The headers cannot be read.
        match Sections::find(unreadable(0..1)) {
            Err(error) => assert!(is_io(&error), "{error}"),
            Ok(found) => panic!("{:?}", found.map(|found| found.sections().to_vec())),
        }
        // The runtime's first byte cannot be read: found, but not verified.
        let mut found = Sections::find(unreadable(PE_END..PE_END + 1))
            .unwrap()
            .unwrap();
        let error = found.verify(&quiet(&Cancel::default())).unwrap_err();
        assert!(is_io(&error), "{error}");
        assert_eq!(error.rule(), "io");
        // The runtime's end record cannot be read: the payload's reader fails.
        let end = listed[0].offset + listed[0].size;
        let mut found = Sections::find(unreadable(end - 22..end)).unwrap().unwrap();
        match found.runtime() {
            Err(error @ PayloadError::Io(_)) => {
                assert_eq!(error.source().unwrap().to_string(), "unreadable sector");
            }
            other => panic!("{:?}", other.map(|payload| payload.entries().len())),
        }
    }

    #[test]
    fn the_first_rule_broken_is_reported() {
        let (mut signed, _) = launcher(tiny_pe(), WINDOWS, 0);
        fake_sign(&mut signed);
        let size = FAKE_CERTIFICATE_LEN as u32;
        let certificates = u32::try_from(signed.len()).unwrap() - size;
        let with_certificates = |offset: u32| {
            let mut file = signed.clone();
            set_certificates(&mut file, offset, size);
            file
        };
        let digest = hex(&sha256(b"zip"));
        let wrong = hex(&sha256(b"zap"));
        let big = MAX_SECTION_LEN + 1;
        let after_elf = |table: &[u8]| written(tiny_elf(), b"zip", table);
        let changed = |table: &[u8], change: &dyn Fn(&mut Vec<u8>, usize)| {
            let mut file = after_elf(table);
            let trailer = file.len() - TRAILER;
            change(&mut file, trailer);
            file
        };
        for (what, file, rule) in [
            (
                "an unaligned certificate table that does not end the file",
                with_certificates(certificates + 1),
                "certificatesUnaligned",
            ),
            (
                "a certificate table inside the image that does not end the file",
                with_certificates(0x200),
                "certificatesNotAtEnd",
            ),
            (
                "a table too long, whose digest is wrong",
                changed(&table(ELF_END, &[]), &|file, at| {
                    file[at..at + 8].copy_from_slice(&(MAX_TABLE_LEN + 1).to_le_bytes());
                    file[at + 8] ^= 1;
                }),
                "tableLength",
            ),
            (
                "a table that is not JSON, whose digest is wrong",
                changed(b"not JSON", &|file, at| file[at + 8] ^= 1),
                "tableHash",
            ),
            (
                "a table of schema 2 that records another program length",
                after_elf(br#"{"schema": 2, "programLength": 1, "sections": []}"#),
                "schema",
            ),
            (
                "a section of another shape in a table of another program length",
                after_elf(br#"{"schema": 1, "programLength": 1, "sections": [{"name": 7}]}"#),
                "tableJson",
            ),
            (
                "another program length and a bad section name",
                after_elf(&table(1, &[("", ELF_END, 3, &digest)])),
                "programLength",
            ),
            (
                "a bad name and a bad digest",
                after_elf(&table(ELF_END, &[("", ELF_END, 3, "x")])),
                "sectionName",
            ),
            (
                "a bad digest and a section too large",
                after_elf(&table(ELF_END, &[(RUNTIME_SECTION, ELF_END, big, "x")])),
                "sectionDigestText",
            ),
            (
                "a section too large, out of place",
                after_elf(&table(ELF_END, &[(RUNTIME_SECTION, 0, big, &digest)])),
                "sectionTooLarge",
            ),
            (
                "a section out of place before one with a bad name",
                after_elf(&table(
                    ELF_END,
                    &[(RUNTIME_SECTION, 0, 3, &digest), ("", ELF_END, 3, &digest)],
                )),
                "sectionPlace",
            ),
            (
                "a gap before the table after a section that is not its digest's",
                after_elf(&table(ELF_END, &[(RUNTIME_SECTION, ELF_END, 2, &wrong)])),
                "tableNotAfterSections",
            ),
        ] {
            assert_eq!(rule_of(file), rule, "{what}");
        }
    }

    #[test]
    fn the_launcher_file_can_be_renamed_while_its_sections_are_held() {
        let (program, platform) = if cfg!(windows) {
            (tiny_pe(), WINDOWS)
        } else {
            (tiny_elf(), LINUX)
        };
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("GauntletDarkLegacy-Launcher.exe");
        let aside = dir.path().join("retired-launcher.exe");
        let (original, _) = launcher(program.clone(), platform, 0);
        fs::write(&path, &original).unwrap();
        let mut held = Sections::of_path(&path).unwrap().unwrap();

        // Renamed aside while its sections are held, as an update renames the running launcher,
        // and another launcher file written in its place.
        rename_noreplace(&path, &aside).unwrap();
        let mut newer = program;
        let runtime = RuntimeFiles::standard(platform, NEWER).zip();
        append_sections(&mut newer, &[(RUNTIME_SECTION, &runtime)], 0);
        fs::write(&path, &newer).unwrap();

        // The handle still reads the file it was opened on, which the path no longer names.
        held.verify(&quiet(&Cancel::default())).unwrap();
        check_contents(&mut held, platform);
        let mut fresh = Sections::of_path(&path).unwrap().unwrap();
        assert_eq!(fresh.runtime().unwrap().build_info().version, NEWER);
        assert_eq!(fs::read(&aside).unwrap(), original);
    }

    #[cfg(windows)]
    #[test]
    fn a_handle_without_share_delete_blocks_the_rename() {
        use std::fs::OpenOptions;
        use std::os::windows::fs::OpenOptionsExt;

        use gdl_files::retry_locked;

        /// The share mode of a handle that lets others read the file, and nothing more.
        const FILE_SHARE_READ: u32 = 0x1;
        /// What Windows refuses a rename with while another handle does not share deletion.
        const ERROR_SHARING_VIOLATION: i32 = 32;

        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("GauntletDarkLegacy-Launcher.exe");
        let aside = dir.path().join("retired-launcher.exe");
        let (file, _) = launcher(tiny_pe(), WINDOWS, 0);
        fs::write(&path, file).unwrap();
        let held = retry_locked(|| {
            OpenOptions::new()
                .read(true)
                .share_mode(FILE_SHARE_READ)
                .open(&path)
        })
        .unwrap();
        let error = fs::rename(&path, &aside).unwrap_err();
        assert_eq!(
            error.raw_os_error(),
            Some(ERROR_SHARING_VIOLATION),
            "{error}"
        );
        assert!(path.exists() && !aside.exists());
        // Let go of, the file is renamed.
        drop(held);
        retry_locked(|| fs::rename(&path, &aside)).unwrap();
        assert!(!path.exists() && aside.exists());
    }

    /// The sectioned files the property tests break: an unsigned PE, an ELF program and a signed
    /// PE, each with where its program ends and where the bytes the sections' rules cover end.
    fn small_files() -> Vec<(&'static str, Vec<u8>, usize, usize)> {
        let (runtime, window) = (noise(40), noise(24));
        let sections: [(&str, &[u8]); 2] = [(RUNTIME_SECTION, &runtime), (WINDOW_SECTION, &window)];
        let sectioned = |program: Vec<u8>| {
            let mut file = program;
            append_sections(&mut file, &sections, 5);
            file
        };
        let pe = sectioned(tiny_pe());
        let elf = sectioned(tiny_elf());
        let mut signed = pe.clone();
        fake_sign(&mut signed);
        let certificates = signed.len() - FAKE_CERTIFICATE_LEN as usize;
        let (pe_len, elf_len) = (pe.len(), elf.len());
        vec![
            ("an unsigned PE", pe, PE_END as usize, pe_len),
            ("an ELF program", elf, ELF_END as usize, elf_len),
            ("a signed PE", signed, PE_END as usize, certificates),
        ]
    }

    #[test]
    fn every_byte_flip_after_the_program_is_refused() {
        let mut wrong = Vec::new();
        let mut refused = 0;
        for (kind, file, program_end, covered) in small_files() {
            open_and_verify(file.clone()).unwrap_or_else(|error| panic!("{kind}: {error}"));
            for at in program_end..covered {
                for bit in 0..u8::BITS {
                    let mut flipped = file.clone();
                    flipped[at] ^= 1 << bit;
                    match panic::catch_unwind(move || open_and_verify(flipped)) {
                        Ok(Err(_)) => refused += 1,
                        Ok(Ok(_)) => wrong.push(format!("{kind}: byte {at}, bit {bit}: accepted")),
                        Err(_) => wrong.push(format!("{kind}: byte {at}, bit {bit}: panicked")),
                    }
                }
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
        eprintln!("{refused} flips refused");
    }

    #[test]
    fn no_flip_or_truncation_panics() {
        // A flip in the program's headers may move where the program ends, or leave it where it
        // was; one in the certificate table is the signature's to find. Neither may panic, and
        // every file cut short is refused or carries no sections.
        let mut wrong = Vec::new();
        for (kind, file, _, _) in small_files() {
            for at in 0..file.len() {
                for bit in 0..u8::BITS {
                    let mut flipped = file.clone();
                    flipped[at] ^= 1 << bit;
                    if panic::catch_unwind(move || open_and_verify(flipped)).is_err() {
                        wrong.push(format!("{kind}: byte {at}, bit {bit}: panicked"));
                    }
                }
            }
            for len in 0..file.len() {
                let prefix = file[..len].to_vec();
                match panic::catch_unwind(move || open_and_verify(prefix)) {
                    Ok(Err(_)) => {}
                    Ok(Ok(_)) => wrong.push(format!("{kind}: the first {len} bytes: accepted")),
                    Err(_) => wrong.push(format!("{kind}: the first {len} bytes: panicked")),
                }
            }
        }
        assert!(wrong.is_empty(), "{}", wrong.join("\n"));
    }

    #[test]
    fn sections_rules_are_stable() {
        let name = || -> Box<str> { RUNTIME_SECTION.into() };
        for (error, rule, shown) in [
            (
                SectionsError::Io(io::Error::other("unreadable sector")),
                "io",
                "the launcher file cannot be read: unreadable sector",
            ),
            (
                SectionsError::UnknownProgram,
                "unknownProgram",
                "not a PE or ELF program",
            ),
            (
                SectionsError::PeHeaders {
                    detail: "PE headers run past the file",
                },
                "peHeaders",
                "PE headers run past the file",
            ),
            (
                SectionsError::ElfHeaders {
                    detail: "ELF ranges run past the file",
                },
                "elfHeaders",
                "ELF ranges run past the file",
            ),
            (
                SectionsError::CertificatesUnaligned,
                "certificatesUnaligned",
                "the certificate table is not quadword-aligned",
            ),
            (
                SectionsError::CertificatesNotAtEnd,
                "certificatesNotAtEnd",
                "the certificate table does not end the file",
            ),
            (
                SectionsError::CertificatesInsideImage,
                "certificatesInsideImage",
                "the certificate table starts inside the image",
            ),
            (
                SectionsError::TableLength { len: 65_537 },
                "tableLength",
                "table length 65537 out of bounds",
            ),
            (
                SectionsError::TableHash,
                "tableHash",
                "the table's SHA-256 does not match the trailer",
            ),
            (
                SectionsError::TableJson {
                    detail: "what was found".into(),
                },
                "tableJson",
                "table JSON: what was found",
            ),
            (
                SectionsError::Schema { found: 2 },
                "schema",
                "table schema 2 is not 1",
            ),
            (
                SectionsError::ProgramLength {
                    recorded: 1024,
                    actual: 1060,
                },
                "programLength",
                "the table records program length 1024, the program ends at 1060",
            ),
            (
                SectionsError::SectionName { name: name() },
                "sectionName",
                r#"section name "runtime.zip" is empty, too long, not printable ASCII or repeated"#,
            ),
            (
                SectionsError::SectionDigestText { name: name() },
                "sectionDigestText",
                r#"section "runtime.zip": bad sha256"#,
            ),
            (
                SectionsError::SectionTooLarge { name: name() },
                "sectionTooLarge",
                r#"section "runtime.zip" is larger than 2 GiB"#,
            ),
            (
                SectionsError::SectionPlace {
                    name: name(),
                    offset: 1025,
                    size: 3,
                    reason: "overlaps the table",
                },
                "sectionPlace",
                r#"section "runtime.zip" at [1025, +3) overlaps the table"#,
            ),
            (
                SectionsError::TableNotAfterSections,
                "tableNotAfterSections",
                "the table does not start where the last section ends",
            ),
            (
                SectionsError::SectionDigest { name: name() },
                "sectionDigest",
                r#"section "runtime.zip": SHA-256 mismatch"#,
            ),
            (
                SectionsError::MissingSection { name: name() },
                "missingSection",
                r#"no section "runtime.zip""#,
            ),
            (
                SectionsError::NoSections,
                "noSections",
                "the file carries no sections",
            ),
            (SectionsError::Cancelled, "cancelled", "cancelled"),
        ] {
            assert_eq!(error.rule(), rule, "{error}");
            assert_eq!(error.to_string(), shown, "{rule}");
            match &error {
                SectionsError::Io(_) => {
                    assert_eq!(error.source().unwrap().to_string(), "unreadable sector");
                }
                _ => assert!(error.source().is_none(), "{rule}"),
            }
        }
        assert!(size_of::<SectionsError>() <= MOST_ERROR_BYTES);
    }
}
