//! Extracting a Legends expansion's tree from a dump into a new folder that a developer or the
//! parity gate names, outside every installation, as the headless program's `--extract` runs it.
//! The output's place is judged before the dump is read, and the dump identified and checked
//! before anything is made; only then is a staging made beside the output, the tree written into
//! it and put in place by one rename that never replaces anything. A refused, failed or cancelled
//! extraction leaves the output's folder as it was, and the dump is only ever read.
//!
//! An installation is known by the names its folder holds, looked for in the folders above the
//! output's path as it is given and above the path the system gives for the output's folder,
//! which reaches past a drive letter mapped to a folder. Neither reaches above a shared folder's
//! own place, on whichever machine shares it, nor above the folder a Linux bind mount maps: an
//! output reached through one is not known to lie in an installation.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::slice;

use gdl_extract::{
    Expansion, ExtractError, Strictness, Trust, check_dreamcast, check_n64, extract_dreamcast,
    extract_n64,
};
use gdl_files::{
    FilesError, Job, Journal, MAX_COMPONENT_CHARS, NameError, NameRule, NameRules, RelPath,
    Staging, absolute_lexical, is_absent, no_links, strip_verbatim,
};
use gdl_install::{STATE_DIR, alpha_markers};
use gdl_media::{Console, DumpRefusal, MediaError, N64Rom, Source, Verdict, identify};

/// The rules an output's name is held to: any characters a component may hold, since a developer
/// chooses it, but never the name of the launcher's state folder, in any letter case, which would
/// make the output's folder look like an installation's.
pub const OUTPUT_NAMES: NameRules = NameRules {
    max_path_chars: MAX_COMPONENT_CHARS,
    ascii_only: false,
    reserved_first: &[STATE_DIR],
};

/// The prefix of the staging an N64 extraction makes beside its output.
const N64_STAGING_PREFIX: &str = ".gdl-extract-n64-";

/// The prefix of the staging a Dreamcast extraction makes beside its output.
const DREAMCAST_STAGING_PREFIX: &str = ".gdl-extract-dreamcast-";

/// How a refusal names the console of a dump whose content tells none.
const NO_CONSOLE: &str = "no console the launcher knows";

/// The prefix of the staging an extraction of `expansion` makes beside its output, before the
/// staging's random letters: a name of the launcher's own, which tells what was being extracted
/// when a run that was killed leaves its staging behind.
pub fn staging_prefix(expansion: Expansion) -> &'static str {
    match expansion {
        Expansion::N64 => N64_STAGING_PREFIX,
        Expansion::Dreamcast => DREAMCAST_STAGING_PREFIX,
    }
}

/// How strictly an extraction holds its dump, as its caller chooses.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StrictnessChoice {
    /// As the extraction scripts hold a dump.
    Script,
    /// As an installation holds a dump, to the trust the extraction is given.
    Install,
}

/// An extraction into a new folder, as its caller asks for it.
#[derive(Clone, Copy, Debug)]
pub struct ExtractRequest<'a> {
    /// The expansion whose tree is extracted.
    pub expansion: Expansion,
    /// The dump: for the N64 expansion the cartridge's ROM, in any byte order; for the Dreamcast
    /// expansion a cue sheet or a GDI, the folder that holds the one sheet, or the file of a
    /// track that one sheet beside it names. The path is read by its names, as the extraction
    /// scripts read it, so that a separator or a `.` after its last name leaves it that file's.
    pub dump: &'a Path,
    /// The new folder the tree goes into, in a folder that is there and lies outside every
    /// installation.
    pub output: &'a Path,
    /// How strictly the dump is held.
    pub strictness: StrictnessChoice,
}

/// What the manifests of an extracted tree count.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Counts {
    /// An N64 tree's.
    N64 {
        /// The ROMDISK's files.
        files: usize,
        /// The executable overlays.
        programs: usize,
        /// The maps among the files.
        maps: usize,
    },
    /// A Dreamcast tree's.
    Dreamcast {
        /// The disc's files.
        disc_files: usize,
        /// The native files restored from DISK.ROM.
        native_files: usize,
        /// The files SIZES.ROM lists outside DISK.ROM, which the tree lists and does not hold.
        external: usize,
    },
}

/// What an extraction into a new folder made.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ExtractSummary {
    /// The expansion extracted.
    pub expansion: Expansion,
    /// How strictly the dump was held, by its stable machine name: `script` or `install`.
    pub strictness: &'static str,
    /// The new folder, absolute, with its `.` and `..` folded by name.
    pub output: PathBuf,
    /// What the tree's manifests count.
    pub counts: Counts,
}

/// Why an extraction into a new folder was refused or failed, as technical detail for logs and a
/// console. What a player reads is chosen from its code, which
/// [`codes::extraction`](crate::codes::extraction) gives it.
#[derive(Debug, thiserror::Error)]
pub enum ExtractionError {
    /// The output's path ends in no name a folder of the launcher's may take.
    #[error("the output must end in a folder name: {error}")]
    OutputName {
        /// The name, and the rule it breaks.
        error: NameError,
    },
    /// Nothing, or something other than a folder, is where the output's folder should be.
    #[error("the output's folder is not a folder: {}", .path.display())]
    OutputParent {
        /// The folder the output was to be made in.
        path: PathBuf,
    },
    /// Something is at the output's path already, or came there before the tree could be put in
    /// place: an extraction never writes into, over or instead of anything.
    #[error("the output is there already: {}", .path.display())]
    OutputExists {
        /// The output.
        path: PathBuf,
    },
    /// The output's folder, or a folder above it, is a link or a junction.
    #[error("the output's path holds a link or a junction: {}", .path.display())]
    OutputLinks {
        /// The link.
        path: PathBuf,
    },
    /// A folder above the output holds the launcher's state folder: the output lies in an
    /// installation, or in a folder being made into one, where only the installation's own jobs
    /// write.
    #[error(
        "the output lies in an installation, whose folder holds {}: {}",
        STATE_DIR,
        .path.display()
    )]
    InsideInstallation {
        /// The installation's folder.
        path: PathBuf,
    },
    /// A folder above the output holds a name the alpha installer leaves, by which a folder it
    /// installed into is known.
    #[error(
        "the output lies in an alpha installation, whose folder holds {name}: {}",
        .path.display()
    )]
    AlphaInstallation {
        /// The installation's folder.
        path: PathBuf,
        /// The alpha installer's name it holds.
        name: &'static str,
    },
    /// Looking at the output's path, or at what a folder above it holds, failed.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path looked at.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// The dump is for another console than the one the expansion is extracted from.
    #[error(
        "the dump is for {}, not for {}",
        .found.map_or(NO_CONSOLE, Console::as_str),
        .wanted.as_str()
    )]
    WrongSlot {
        /// The console the dump's content tells, if any.
        found: Option<Console>,
        /// The expansion it was given for.
        wanted: Expansion,
    },
    /// The dump comes in a container the launcher does not read, or is nothing it knows.
    #[error("the dump is refused: {}", .refusal.as_str())]
    Refused {
        /// What identifying it found.
        refusal: DumpRefusal,
    },
    /// The dump could not be read or identified, or breaks a rule of its format. A stop the job
    /// asked for is never one, though a reader heeded it: it is the extraction's own.
    #[error(transparent)]
    Media(MediaError),
    /// The check or the extraction refused the dump or failed, or the job was asked to stop,
    /// wherever that was heeded.
    #[error(transparent)]
    Extract(ExtractError),
    /// Making the staging, or putting the tree in place from it, failed.
    #[error(transparent)]
    Files(FilesError),
}

impl ExtractionError {
    /// The failure's stable machine name, in camelCase, for reports and logs: the refusal's for a
    /// dump refused by its container, the reader's, the extraction's or the file operation's own
    /// for a failure met there, and the output's otherwise.
    pub fn rule(&self) -> &'static str {
        // Every variant is named, with no wildcard, so that a failure added later is given its
        // rule on purpose.
        match self {
            ExtractionError::OutputName { .. } => "outputName",
            ExtractionError::OutputParent { .. } => "outputParent",
            ExtractionError::OutputExists { .. } => "outputExists",
            ExtractionError::OutputLinks { .. } => "outputLinks",
            ExtractionError::InsideInstallation { .. } => "insideInstallation",
            ExtractionError::AlphaInstallation { .. } => "alphaInstallation",
            ExtractionError::Io { .. } => "io",
            ExtractionError::WrongSlot { .. } => "wrongSlot",
            ExtractionError::Refused { refusal } => refusal.as_str(),
            ExtractionError::Media(error) => error.rule(),
            ExtractionError::Extract(error) => error.rule(),
            ExtractionError::Files(error) => error.rule(),
        }
    }
}

/// The name the path `output` ends in, the name the output's folder is made with, held to
/// [`OUTPUT_NAMES`]. It is read from the path alone, which is never looked for. A path that ends
/// in no name, such as a root or a path that ends in `..`, is refused as empty, and a name that is
/// not Unicode, which no root of a tree can be named, as holding what is no ASCII character.
pub fn output_name(output: &Path) -> Result<RelPath, ExtractionError> {
    let refused = |error| ExtractionError::OutputName { error };
    let Some(name) = output.file_name() else {
        let name = output.to_string_lossy().into();
        return Err(refused(NameError {
            name,
            rule: NameRule::Empty,
        }));
    };
    let Some(text) = name.to_str() else {
        let name = name.to_string_lossy().into();
        return Err(refused(NameError {
            name,
            rule: NameRule::NonAscii,
        }));
    };
    RelPath::parse(text, &OUTPUT_NAMES).map_err(refused)
}

/// Extracts the expansion `request` names from its dump into its output, a new folder, held to
/// `trust` when the request asks for an installation's strictness, and gives back what it made.
///
/// Nothing is read from the dump until the output's place has passed, and nothing is made until
/// the dump has. The output's name must pass [`OUTPUT_NAMES`] as it was given and once the path is
/// made absolute, since Windows drops a trailing dot or space from a name it makes absolute. Its
/// folder must be a folder that is there, with no link on its path; nothing may be at the output;
/// and no folder above it, from its own up to the volume's root, on its path as given or on the
/// path the system gives for its folder, may hold the launcher's state folder or a name the
/// alpha's installer leaves, since only an installation's own jobs write in one. The dump's path
/// is then read by its names alone, as the extraction scripts read one: a separator, or a `.` and
/// a separator, after a file's name leaves it that file's path, which the system would not open
/// so, and a `..` stays as it was given. The dump is identified by its content: a container the
/// launcher does not read, or content it does not know, is refused, and so is a dump for another
/// console than the expansion's. Its identity is held to the trust by the check alone, which reads
/// the dump and decides on it, writing nothing. Only then is a staging made beside the output and
/// the tree written into it; unless the job has been asked to stop by then, the tree is put in
/// place as the output by one rename that never replaces anything.
///
/// A refusal before the staging leaves the output's folder as it was. After it, a failure leaves
/// nothing outside the staging, which goes when it is dropped, as it does once the tree has moved
/// out of it.
pub fn extract_to_new_folder(
    request: &ExtractRequest<'_>,
    trust: &Trust,
    job: &Job<'_>,
) -> Result<ExtractSummary, ExtractionError> {
    let place = OutputPlace::checked(request.output)?;
    // Rebuilt from its components, which drop a trailing separator and every `.` but a leading one.
    let dump: PathBuf = request.dump.components().collect();
    let identity = identify(&dump).map_err(read_failure)?;
    if let Verdict::Refused(refusal) = identity.verdict
        && refuses_container(refusal)
    {
        return Err(ExtractionError::Refused { refusal });
    }
    let mode = match request.strictness {
        StrictnessChoice::Script => Strictness::Script,
        StrictnessChoice::Install => Strictness::Install(trust),
    };
    let counts = match (request.expansion, &identity.source) {
        (Expansion::N64, Source::N64 { .. }) => extract_rom(&dump, &place, &mode, job)?,
        (Expansion::Dreamcast, Source::Dreamcast { sheet, .. }) => {
            extract_disc(sheet, &place, &mode, job)?
        }
        (wanted, _) => {
            return Err(ExtractionError::WrongSlot {
                found: identity.console,
                wanted,
            });
        }
    };
    Ok(ExtractSummary {
        expansion: request.expansion,
        strictness: mode.as_str(),
        output: place.output,
        counts,
    })
}

/// Whether `refusal`, which identifying a dump gave, is of the dump's container or content rather
/// than of its identity: a container the launcher does not read, a DiscJuggler image, an archive,
/// or content it does not know. Identity is the check's to hold to a trust, never identification's.
fn refuses_container(refusal: DumpRefusal) -> bool {
    match refusal {
        DumpRefusal::UnsupportedContainer
        | DumpRefusal::Cdi
        | DumpRefusal::Archive
        | DumpRefusal::Unrecognized => true,
        DumpRefusal::OtherGame | DumpRefusal::OtherRevision | DumpRefusal::HashMismatch => false,
    }
}

/// What a reader's failure to read the dump reports: the reader's own, but for a stop the job
/// asked for, which is the extraction's whichever part of it heeded the job's cancel, as it is to
/// the check and to the writer of the tree, so that no caller takes it for a failure.
fn read_failure(error: MediaError) -> ExtractionError {
    match error {
        MediaError::Cancelled => ExtractionError::Extract(ExtractError::Cancelled),
        error => ExtractionError::Media(error),
    }
}

/// Extracts the N64 tree of the ROM at `dump` into `place`, the ROM held as `mode` says.
fn extract_rom(
    dump: &Path,
    place: &OutputPlace,
    mode: &Strictness<'_>,
    job: &Job<'_>,
) -> Result<Counts, ExtractionError> {
    // The ROM is let go of once checked: the checked ROM holds what the tree is written from.
    let checked = {
        let rom = N64Rom::open(dump, job).map_err(read_failure)?;
        check_n64(&rom, mode, job).map_err(ExtractionError::Extract)?
    };
    let staging = place.staging(Expansion::N64)?;
    let extraction =
        extract_n64(&checked, &staging, &place.root, job).map_err(ExtractionError::Extract)?;
    place.publish(&staging, job)?;
    let manifest = &extraction.manifest;
    Ok(Counts::N64 {
        files: manifest.files.len(),
        programs: manifest.programs.len(),
        maps: manifest.maps.len(),
    })
}

/// Extracts the Dreamcast tree of the disc whose sheet is at `sheet` into `place`, the disc held
/// as `mode` says.
fn extract_disc(
    sheet: &Path,
    place: &OutputPlace,
    mode: &Strictness<'_>,
    job: &Job<'_>,
) -> Result<Counts, ExtractionError> {
    let mut checked = check_dreamcast(sheet, mode, job).map_err(ExtractionError::Extract)?;
    let staging = place.staging(Expansion::Dreamcast)?;
    let extraction = extract_dreamcast(&mut checked, &staging, &place.root, job)
        .map_err(ExtractionError::Extract)?;
    place.publish(&staging, job)?;
    Ok(Counts::Dreamcast {
        disc_files: extraction.disc.files.len(),
        native_files: extraction.native.files.len(),
        external: extraction.native.external.len(),
    })
}

/// Where an extraction's tree is to go, found fit before the dump is read.
struct OutputPlace {
    /// The output, absolute, with its `.` and `..` folded by name.
    output: PathBuf,
    /// The folder the output is made in, where the staging is made beside it.
    parent: PathBuf,
    /// The output's name: the root the tree lies under in the staging.
    root: RelPath,
}

impl OutputPlace {
    /// The place the path `given` names for a new folder, refused unless a tree may be put there.
    fn checked(given: &Path) -> Result<OutputPlace, ExtractionError> {
        output_name(given)?;
        let output = absolute_lexical(given).map_err(|source| ExtractionError::Io {
            path: given.to_path_buf(),
            source,
        })?;
        let root = output_name(&output)?;
        let parent = match output.parent() {
            Some(parent) => parent.to_path_buf(),
            None => return Err(ExtractionError::OutputParent { path: output }),
        };
        match fs::symlink_metadata(&parent) {
            Ok(found) if found.file_type().is_symlink() => {
                return Err(ExtractionError::OutputLinks { path: parent });
            }
            Ok(found) if found.is_dir() => {}
            Ok(_) => return Err(ExtractionError::OutputParent { path: parent }),
            Err(error) if is_absent(&error) => {
                return Err(ExtractionError::OutputParent { path: parent });
            }
            Err(source) => {
                return Err(ExtractionError::Io {
                    path: parent,
                    source,
                });
            }
        }
        match fs::symlink_metadata(&output) {
            Ok(_) => return Err(ExtractionError::OutputExists { path: output }),
            Err(error) if is_absent(&error) => {}
            Err(source) => {
                return Err(ExtractionError::Io {
                    path: output,
                    source,
                });
            }
        }
        no_links(&parent).map_err(on_the_way)?;
        outside_installations(&parent, |path| fs::canonicalize(path))?;
        Ok(OutputPlace {
            output,
            parent,
            root,
        })
    }

    /// A new staging beside the output, for a tree of `expansion`.
    fn staging(&self, expansion: Expansion) -> Result<Staging, ExtractionError> {
        Staging::create_in(&self.parent, staging_prefix(expansion)).map_err(on_the_way)
    }

    /// Puts the tree `staging` holds in place as the output, unless `job` has been asked to stop.
    fn publish(&self, staging: &Staging, job: &Job<'_>) -> Result<(), ExtractionError> {
        job.cancel
            .check()
            .map_err(|cancelled| ExtractionError::Extract(cancelled.into()))?;
        let roots = slice::from_ref(&self.root);
        staging
            .publish_new(roots, &self.parent, &mut Unrecorded)
            .map_err(|error| placed(error, staging.path()))
    }
}

/// Refuses an output whose folder `parent`, or a folder above it up to the volume's root, holds
/// the launcher's state folder, as an installation or a folder being made into one does, or a
/// name the alpha's installer leaves. The state folder is looked for by its name, and the alpha
/// installer's names among the names each folder lists, so no file of either is ever read.
///
/// The folders above are those `parent` names, then those above the path `real` gives for it,
/// the one the system finds by opening the folder: a drive letter mapped to a folder is no link,
/// and the folders above the one it maps lie beyond its root, where only that path reaches. A
/// folder whose path `real` cannot give is refused, since nothing then shows what lies above it.
/// A refusal names the folder without the verbatim prefix that such a path has on Windows.
fn outside_installations(
    parent: &Path,
    real: impl FnOnce(&Path) -> io::Result<PathBuf>,
) -> Result<(), ExtractionError> {
    nothing_installed_above(parent, Path::to_path_buf)?;
    let real = real(parent).map_err(|source| ExtractionError::Io {
        path: parent.to_path_buf(),
        source,
    })?;
    // The same folders again, when the system gives the folder's path as it was given.
    if strip_verbatim(&real) == parent {
        return Ok(());
    }
    nothing_installed_above(&real, strip_verbatim)
}

/// Refuses a folder `path` that, or a folder above which, holds the launcher's state folder or a
/// name the alpha's installer leaves, as [`outside_installations`] does, each folder a refusal
/// names written as `shown` gives it.
fn nothing_installed_above(
    path: &Path,
    shown: fn(&Path) -> PathBuf,
) -> Result<(), ExtractionError> {
    for folder in path.ancestors() {
        let state = folder.join(STATE_DIR);
        match fs::symlink_metadata(&state) {
            Ok(_) => {
                return Err(ExtractionError::InsideInstallation {
                    path: shown(folder),
                });
            }
            Err(error) if is_absent(&error) => {}
            Err(source) => {
                return Err(ExtractionError::Io {
                    path: shown(&state),
                    source,
                });
            }
        }
        let markers = alpha_markers(folder).map_err(|source| ExtractionError::Io {
            path: shown(folder),
            source,
        })?;
        if let Some(&name) = markers.first() {
            return Err(ExtractionError::AlphaInstallation {
                path: shown(folder),
                name,
            });
        }
    }
    Ok(())
}

/// What a refusal met on the way to the output's folder reports: a link there lies on the path
/// the developer chose, and any other refusal is the file operation's own.
fn on_the_way(error: FilesError) -> ExtractionError {
    match error {
        FilesError::Link { path } => ExtractionError::OutputLinks { path },
        error => ExtractionError::Files(error),
    }
}

/// What a refusal to put the tree in place from `staging` reports, by where it was met: a name
/// taken at the output is the output being there, and a link outside the staging lies on the
/// output's path, which the developer chose; a link inside the staging was put there by something
/// else, which makes it the staging's failure, as every other refusal is.
fn placed(error: FilesError, staging: &Path) -> ExtractionError {
    match error {
        FilesError::Exists { path } => ExtractionError::OutputExists { path },
        FilesError::Link { path } if !path.starts_with(staging) => {
            ExtractionError::OutputLinks { path }
        }
        error => ExtractionError::Files(error),
    }
}

/// The journal of a publish that records nothing: an extraction into a new folder moves its one
/// root in one rename, so nothing is left to finish or take back after it.
struct Unrecorded;

impl Journal for Unrecorded {
    fn published(&mut self, _: &RelPath) -> Result<(), FilesError> {
        Ok(())
    }
}

/// What the session's extraction and headless tests share: the dumps and trees of the shared
/// vectors, the folders an extraction is tried in, the one way a test compares what an extraction
/// left in them, and a name that is not Unicode.
#[cfg(test)]
pub(crate) mod vectors {
    use std::cell::Cell;
    use std::collections::BTreeMap;
    use std::ffi::OsString;
    use std::fs;
    use std::path::PathBuf;

    use gdl_extract::{DISC_FOLDER, Expansion, Fingerprints, IP_BIN, Trust};
    use gdl_files::{Cancel, Job, NoProgress, retry_locked};
    use gdl_fixtures::{DiscSet, EntryState, backdate, listed, snapshot, snapshot_outside, unhex};
    pub(crate) use gdl_fixtures::{Listed, tree_listing};
    use gdl_media::{N64Rom, ROMDISK_LIMITS, Sha1Digest, Sha256Digest, SourceName, US_DISC};
    use serde_json::Value;
    use tempfile::TempDir;

    use super::staging_prefix;

    /// The N64 cases.
    const N64_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/n64-vectors.json"
    ));

    /// The Dreamcast cases.
    const DREAMCAST_VECTORS: &str = include_str!(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../../../../tests/scripts/data/legends/dreamcast-vectors.json"
    ));

    /// The folder of the tests' parent that is there before any extraction and must stay as it
    /// was, with a file in it.
    const KEPT_FOLDER: &str = "kept";

    /// What no raw DEFLATE stream begins with: a last block of the type the format reserves.
    const NO_STREAM: u8 = 0xFF;

    /// The N64 cases, read.
    fn n64_vectors() -> Value {
        serde_json::from_str(N64_VECTORS).expect("the N64 vectors are JSON")
    }

    /// The Dreamcast cases, read.
    fn dreamcast_vectors() -> Value {
        serde_json::from_str(DREAMCAST_VECTORS).expect("the Dreamcast vectors are JSON")
    }

    /// A string of the vectors.
    fn text(value: &Value) -> &str {
        value.as_str().expect("a string")
    }

    /// The image the N64 vectors freeze as their default, and the name their extraction of it
    /// gives its dump, which the tree's manifest records.
    pub(crate) fn default_rom() -> (String, Vec<u8>) {
        let vectors = n64_vectors();
        let cases = vectors["extractions"].as_array().expect("the extractions");
        let case = cases
            .iter()
            .find(|case| case["image"] == "default")
            .expect("an extraction of the default image");
        let image = unhex(&vectors["images"]["default"]);
        (text(&case["name"]).to_owned(), image)
    }

    /// The tree the N64 extractor writes for the default image.
    pub(crate) fn default_tree() -> Vec<Listed> {
        listed(&n64_vectors()["trees"]["default"])
    }

    /// The tree the Dreamcast extractor writes for the vectors' full set.
    pub(crate) fn full_tree() -> Vec<Listed> {
        listed(&dreamcast_vectors()["sets"]["full"]["tree"])
    }

    /// `image`, an N64 ROM in the big-endian order, with its first file's first stored byte made
    /// one that no raw DEFLATE stream begins with: the N64 extractor's own corrupt stream, which
    /// the check refuses before anything is made.
    pub(crate) fn with_bad_stream(image: &[u8]) -> Vec<u8> {
        let rom = N64Rom::from_bytes(SourceName::from_units(Vec::new()), image.to_vec())
            .expect("an N64 ROM");
        let romdisk = rom.romdisk(&ROMDISK_LIMITS).expect("its ROMDISK");
        let first = usize::try_from(romdisk.entries[0].rom_offset).expect("an offset in the ROM");
        let mut corrupt = image.to_vec();
        corrupt[first] = NO_STREAM;
        corrupt
    }

    /// The fingerprints the fingerprint writer takes from the tree the vectors' full set extracts
    /// to: the path and digest of IP.BIN and of every disc file, never a native file's or a
    /// manifest's.
    pub(crate) fn full_fingerprints() -> Vec<(String, Sha256Digest)> {
        let disc_folder = format!("{DISC_FOLDER}/");
        full_tree()
            .into_iter()
            .filter(|(path, _, _)| path == IP_BIN || path.starts_with(&disc_folder))
            .map(|(path, _, digest)| {
                let digest = Sha256Digest::from_hex(&digest).expect("a listed digest");
                (path, digest)
            })
            .collect()
    }

    /// What an installation's trust holds for the tests' dumps: the cartridge whose SHA-1 digest
    /// is `n64`, the US disc's identity, which the vectors' full set's boot area gives, and
    /// `fingerprints`.
    pub(crate) fn trust_of(n64: Sha1Digest, fingerprints: Vec<(String, Sha256Digest)>) -> Trust {
        Trust {
            n64_canonical_sha1: n64,
            disc: US_DISC,
            fingerprints: Some(Fingerprints::from_files(fingerprints).expect("fingerprints")),
        }
    }

    /// A job nobody follows, asked to stop only through `cancel`.
    pub(crate) fn quiet(cancel: &Cancel) -> Job<'_> {
        Job {
            cancel,
            progress: &NoProgress,
        }
    }

    /// A name that is not Unicode, as a path or an argument may hold.
    #[cfg(unix)]
    pub(crate) fn not_unicode() -> OsString {
        use std::os::unix::ffi::OsStringExt;
        OsString::from_vec(b"caf\xe9".to_vec())
    }

    /// A name that is not Unicode, as a path or an argument may hold.
    #[cfg(windows)]
    pub(crate) fn not_unicode() -> OsString {
        use std::os::windows::ffi::OsStringExt;
        OsString::from_wide(&[u16::from(b'c'), u16::from(b'a'), u16::from(b'f'), 0xD800])
    }

    /// The folders a test extracts in, apart: one the outputs are made in, which holds a folder
    /// and a file of its own before any extraction, and one the dumps lie in.
    pub(crate) struct Folders {
        /// The folder they lie in, removed with them.
        _dir: TempDir,
        /// That folder's path, where a test keeps what must lie outside both.
        pub(crate) root: PathBuf,
        /// The folder the outputs are made in.
        pub(crate) parent: PathBuf,
        /// The folder the dumps lie in.
        dumps: PathBuf,
        /// How many discs have been written into the dump folder, each into a folder of its own.
        discs: Cell<usize>,
    }

    impl Folders {
        pub(crate) fn new() -> Folders {
            let dir = tempfile::tempdir().unwrap();
            let root = dir.path().to_path_buf();
            let parent = root.join("extracted");
            let kept = parent.join(KEPT_FOLDER);
            fs::create_dir_all(&kept).unwrap();
            fs::write(kept.join("kept.txt"), b"kept").unwrap();
            fs::write(parent.join("beside.txt"), b"beside").unwrap();
            let dumps = root.join("dumps");
            fs::create_dir(&dumps).unwrap();
            Folders {
                _dir: dir,
                root,
                parent,
                dumps,
                discs: Cell::new(0),
            }
        }

        /// The output named `name` in the parent.
        pub(crate) fn output(&self, name: &str) -> PathBuf {
            self.parent.join(name)
        }

        /// Writes `bytes` as the dump named `name`, and gives its path.
        pub(crate) fn dump(&self, name: &str, bytes: &[u8]) -> PathBuf {
            let path = self.dumps.join(name);
            fs::write(&path, bytes).unwrap();
            path
        }

        /// Writes the files of the disc `set` into a new folder of the dump folder, so that the
        /// discs a test writes never share a folder, and gives the path of its sheet.
        pub(crate) fn disc(&self, set: &DiscSet) -> PathBuf {
            let count = self.discs.get();
            self.discs.set(count + 1);
            let folder = self.dumps.join(format!("disc{count}"));
            fs::create_dir(&folder).unwrap();
            set.write(&folder).unwrap()
        }

        /// What a test compares after an extraction of `expansion`, taken once every time of
        /// change is long before it, so that an entry made and removed again shows as well as one
        /// changed.
        pub(crate) fn before(&self, expansion: Expansion) -> Before {
            backdate(&self.parent).unwrap();
            backdate(&self.dumps).unwrap();
            let prefix = staging_prefix(expansion);
            Before {
                prefix,
                outside: snapshot_outside(&self.parent, prefix).unwrap(),
                whole: snapshot(&self.parent).unwrap(),
                dumps: snapshot(&self.dumps).unwrap(),
            }
        }

        /// Checks, after a refusal that came before any staging, that the parent is as it was, its
        /// own entry included, since nothing was made in it, and so is the dump folder.
        #[track_caller]
        pub(crate) fn assert_untouched(&self, before: &Before) {
            assert!(
                snapshot(&self.parent).unwrap() == before.whole,
                "the parent changed"
            );
            self.assert_dumps_unchanged(before);
        }

        /// Checks, after a failure once a staging was made, that the staging is gone and that
        /// nothing else changed: neither what lies outside it in the parent nor the dump folder.
        #[track_caller]
        pub(crate) fn assert_staging_gone(&self, before: &Before) {
            self.clear_stagings(before.prefix);
            assert!(
                snapshot_outside(&self.parent, before.prefix).unwrap() == before.outside,
                "the parent changed outside its stagings"
            );
            self.assert_dumps_unchanged(before);
        }

        /// Checks, after a success, that the output named `name` holds exactly `tree`, that the
        /// staging is gone, and that nothing else changed: neither what lies outside the output
        /// and the staging in the parent nor the dump folder.
        #[track_caller]
        pub(crate) fn assert_published(&self, before: &Before, name: &str, tree: &[Listed]) {
            self.clear_stagings(before.prefix);
            let mut outside = snapshot_outside(&self.parent, before.prefix).unwrap();
            outside.retain(|path, _| !path.starts_with(name));
            assert!(
                outside == before.outside,
                "the parent changed outside the output"
            );
            assert_eq!(tree_listing(&self.output(name)), tree);
            self.assert_dumps_unchanged(before);
        }

        /// Checks that the dump folder is as it was: a dump is only ever read.
        #[track_caller]
        fn assert_dumps_unchanged(&self, before: &Before) {
            assert!(
                snapshot(&self.dumps).unwrap() == before.dumps,
                "the dump folder changed"
            );
        }

        /// Checks that no staging named after `prefix` is left in the parent. A staging that a
        /// scanner held a file of as it was dropped is removed once the hold ends, as its drop
        /// ignores a failed removal; only Windows' scanners hold files so, so a staging left on
        /// another system fails the check.
        #[track_caller]
        fn clear_stagings(&self, prefix: &str) {
            for entry in fs::read_dir(&self.parent).unwrap() {
                let entry = entry.unwrap();
                if !entry
                    .file_name()
                    .as_encoded_bytes()
                    .starts_with(prefix.as_bytes())
                {
                    continue;
                }
                let left = entry.path();
                if cfg!(windows) {
                    if let Err(error) = retry_locked(|| fs::remove_dir_all(&left)) {
                        panic!("a staging was left: {}: {error}", left.display());
                    }
                } else {
                    panic!("a staging was left: {}", left.display());
                }
            }
        }
    }

    /// What lay in a test's folders before an extraction.
    pub(crate) struct Before {
        /// The prefix of the extraction's staging.
        prefix: &'static str,
        /// The parent's entries outside its stagings.
        outside: BTreeMap<PathBuf, EntryState>,
        /// The parent whole, its own entry included.
        whole: BTreeMap<PathBuf, EntryState>,
        /// The dump folder whole.
        dumps: BTreeMap<PathBuf, EntryState>,
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io;
    use std::ops::Range;
    use std::path::{Path, PathBuf};
    use std::sync::Mutex;
    use std::sync::atomic::{AtomicUsize, Ordering};

    use gdl_extract::{
        DISC_FOLDER, DISK_ROM, Expansion, ExtractError, FingerprintRule, N64_MANIFEST, Trust,
        UNPACK,
    };
    use gdl_files::{
        Cancel, FilesError, Job, NameRule, PhaseId, Progress, RelPath, STAGING_RANDOM_CHARS,
        strip_verbatim,
    };
    use gdl_fixtures::{
        DiscSet, Edc, InstallFixture, RuntimeFiles, backdate, link_folder, make_alpha_markers,
        sha256_hex, snapshot,
    };
    use gdl_media::{
        Console, DumpRefusal, MediaError, RomRule, Sha1Digest, Sha256Digest, US_SHA1, Verdict,
        identify,
    };
    use gdl_release::Platform;
    use serde_json::Value;

    use super::vectors::{
        Folders, Listed, default_rom, default_tree, full_fingerprints, full_tree, not_unicode,
        quiet, trust_of, with_bad_stream,
    };
    use super::{
        Counts, ExtractRequest, ExtractSummary, ExtractionError, StrictnessChoice,
        extract_to_new_folder, output_name, outside_installations, placed, staging_prefix,
    };
    use crate::codes::{self, Report};

    /// The version the installation fixtures are of.
    const VERSION: &str = "1.0.0-beta";

    /// Where an N64 ROM's header gives its game code.
    const GAME_CODE: Range<usize> = 0x3B..0x3F;

    /// The game code the US cartridge's header gives.
    const US_GAME_CODE: &[u8; 4] = b"NGXE";

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// Extracts `expansion` from `dump` into `output`, held as `strictness` says to `trust`, in a
    /// job nobody stops or follows.
    fn extract(
        expansion: Expansion,
        dump: &Path,
        output: &Path,
        strictness: StrictnessChoice,
        trust: &Trust,
    ) -> Result<ExtractSummary, ExtractionError> {
        let cancel = Cancel::default();
        let request = ExtractRequest {
            expansion,
            dump,
            output,
            strictness,
        };
        extract_to_new_folder(&request, trust, &quiet(&cancel))
    }

    /// Extracts as [`extract`] does, under the extraction scripts' strictness.
    fn extract_as_script(
        expansion: Expansion,
        dump: &Path,
        output: &Path,
    ) -> Result<ExtractSummary, ExtractionError> {
        let trust = Trust::shipped();
        extract(expansion, dump, output, StrictnessChoice::Script, &trust)
    }

    /// The code `error` reports.
    fn code_of(error: &ExtractionError) -> Report {
        codes::extraction(error)
    }

    /// Checks that `error` refuses an output's name for breaking `rule`, and reports what an
    /// output that ends in no folder name does.
    #[track_caller]
    fn assert_name_refused(error: &ExtractionError, rule: NameRule) {
        match error {
            ExtractionError::OutputName { error } => assert_eq!(error.rule, rule, "{error}"),
            other => panic!("{other:?}"),
        }
        assert_eq!(code_of(error), Report::Error("folder.notDirectory"));
    }

    /// A GameCube disc's first bytes, which carry the mark every GameCube disc carries.
    fn gamecube_image() -> Vec<u8> {
        let mut image = vec![0; 0x440];
        image[..6].copy_from_slice(b"GUNE5D");
        image[0x1C..0x20].copy_from_slice(&[0xC2, 0x33, 0x9F, 0x3D]);
        image
    }

    /// The end of a DiscJuggler image of its third version and a half: its version, then where its
    /// header lies.
    fn cdi_image() -> Vec<u8> {
        let mut image = vec![0x55; 4096];
        image.extend_from_slice(&0x8000_0006_u32.to_le_bytes());
        image.extend_from_slice(&4000_u32.to_le_bytes());
        image
    }

    /// Progress that asks its job to stop once it has been told `at` advances, counted from one,
    /// or before the job begins when `at` is nought, and counts the advances it is told.
    struct CancelAt<'a> {
        /// The job's request to stop.
        cancel: &'a Cancel,
        /// The advance it asks at.
        at: usize,
        /// How many advances it has been told.
        advances: AtomicUsize,
    }

    impl<'a> CancelAt<'a> {
        fn new(cancel: &'a Cancel, at: usize) -> CancelAt<'a> {
            if at == 0 {
                cancel.cancel();
            }
            CancelAt {
                cancel,
                at,
                advances: AtomicUsize::new(0),
            }
        }
    }

    impl Progress for CancelAt<'_> {
        fn phase(&self, _: PhaseId) {}

        fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {
            if self.advances.fetch_add(1, Ordering::SeqCst) + 1 == self.at {
                self.cancel.cancel();
            }
        }
    }

    /// Progress that, as an extraction begins to write its tree, writes down the stagings in the
    /// folder the output is made in, and then asks the job to stop when `cancel` is given.
    struct StagingsAtUnpack<'a> {
        /// The folder the output is made in.
        parent: &'a Path,
        /// The request to stop, when the job is to be stopped there.
        cancel: Option<&'a Cancel>,
        /// The names of the stagings it found.
        found: Mutex<Vec<String>>,
    }

    impl Progress for StagingsAtUnpack<'_> {
        fn phase(&self, phase: PhaseId) {
            if phase != UNPACK {
                return;
            }
            let names = fs::read_dir(self.parent)
                .unwrap()
                .map(|entry| entry.unwrap().file_name().into_string().unwrap())
                .filter(|name| name.starts_with(".gdl-extract-"));
            self.found.lock().unwrap().extend(names);
            if let Some(cancel) = self.cancel {
                cancel.cancel();
            }
        }

        fn advance(&self, _: u64, _: u64, _: Option<&RelPath>) {}
    }

    #[test]
    fn output_names_follow_their_rules() {
        for name in [
            "n64",
            "dreamcast",
            "Legends (USA)",
            "caf\u{e9} \u{1d11e}",
            ".a",
        ] {
            let parsed = output_name(&Path::new("out").join(name)).unwrap();
            assert_eq!(parsed.as_str(), name);
        }
        // A trailing separator names the folder before it.
        let parsed = output_name(Path::new("out/n64/")).unwrap();
        assert_eq!(parsed.as_str(), "n64");
        let long = "a".repeat(201);
        for (name, rule) in [
            ("x.", NameRule::TrailingDotOrSpace),
            ("x ", NameRule::TrailingDotOrSpace),
            (".gdl-launcher", NameRule::Reserved),
            (".GDL-Launcher", NameRule::Reserved),
            ("installation.json", NameRule::AlphaName),
            ("CON", NameRule::DeviceName),
            ("a\tb", NameRule::Control),
            (long.as_str(), NameRule::PathTooLong),
        ] {
            let error = output_name(&Path::new("out").join(name)).unwrap_err();
            assert_name_refused(&error, rule);
        }
        // A path that ends in no name, and a name that is not Unicode.
        for path in ["", "..", "out/.."] {
            assert_name_refused(&output_name(Path::new(path)).unwrap_err(), NameRule::Empty);
        }
        let error = output_name(&Path::new("out").join(not_unicode())).unwrap_err();
        assert_name_refused(&error, NameRule::NonAscii);
    }

    #[test]
    fn an_output_is_named_as_it_was_given_on_every_system() {
        // Windows would drop the trailing dot or space as it makes the path absolute, and so make
        // another folder than the one named; the name is refused before that on every system,
        // before the dump, which is not even there, is looked for.
        let folders = Folders::new();
        let dump = folders.root.join("missing.z64");
        let before = folders.before(Expansion::N64);
        for name in ["n64.", "n64 ", "n64. "] {
            let error = extract_as_script(Expansion::N64, &dump, &folders.output(name));
            assert_name_refused(&error.unwrap_err(), NameRule::TrailingDotOrSpace);
        }
        folders.assert_untouched(&before);
    }

    #[test]
    fn an_existing_output_is_refused_before_the_dump_is_read() {
        let folders = Folders::new();
        // The dump is not even there, so a look at it would fail otherwise.
        let dump = folders.root.join("missing.z64");
        let output = folders.output("n64");
        fs::create_dir(&output).unwrap();
        fs::write(output.join("notes.txt"), b"the developer's own").unwrap();
        let file = folders.output("file");
        fs::write(&file, b"a file").unwrap();
        let before = folders.before(Expansion::N64);
        for (taken, expansion) in [(&output, Expansion::N64), (&file, Expansion::Dreamcast)] {
            let error = extract_as_script(expansion, &dump, taken).unwrap_err();
            match &error {
                ExtractionError::OutputExists { path } => assert_eq!(path, taken),
                other => panic!("{other:?}"),
            }
            assert_eq!(error.rule(), "outputExists");
            assert_eq!(code_of(&error), Report::Error("folder.collision"));
        }
        // What was there, the developer's file in its folder included, is as it was.
        folders.assert_untouched(&before);
    }

    #[test]
    fn a_missing_parent_is_refused() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        let before = folders.before(Expansion::N64);
        let missing = folders.output("missing");
        let file = folders.output("beside.txt");
        for parent in [&missing, &file] {
            let error = extract_as_script(Expansion::N64, &dump, &parent.join("n64")).unwrap_err();
            match &error {
                ExtractionError::OutputParent { path } => assert_eq!(path, parent),
                other => panic!("{other:?}"),
            }
            assert_eq!(error.rule(), "outputParent");
            assert_eq!(code_of(&error), Report::Error("folder.notDirectory"));
        }
        // Nothing was made, the missing folder least of all.
        folders.assert_untouched(&before);
        assert!(!missing.exists());
    }

    #[test]
    fn a_link_on_the_outputs_path_is_refused() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        // A folder of the developer's outside the parent, which a link in the parent leads to.
        let target = folders.root.join("target");
        fs::create_dir_all(target.join("sub")).unwrap();
        fs::write(target.join("sub").join("own.txt"), b"own").unwrap();
        let linked = folders.output("linked");
        link_folder(&linked, &target).unwrap();
        backdate(&target).unwrap();
        let target_before = snapshot(&target).unwrap();
        let before = folders.before(Expansion::N64);
        // The link as the output's folder, and as a folder above it.
        for output in [linked.join("n64"), linked.join("sub").join("n64")] {
            let error = extract_as_script(Expansion::N64, &dump, &output).unwrap_err();
            match &error {
                ExtractionError::OutputLinks { path } => assert_eq!(path, &linked),
                other => panic!("{}: {other:?}", output.display()),
            }
            assert_eq!(error.rule(), "outputLinks");
            assert_eq!(code_of(&error), Report::Error("folder.links"));
        }
        folders.assert_untouched(&before);
        assert!(
            snapshot(&target).unwrap() == target_before,
            "the folder the link leads to changed"
        );
    }

    #[test]
    fn outputs_inside_installations_are_refused() {
        let Some(platform) = Platform::of_this_build() else {
            eprintln!("skipped: no release is built for this system");
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        // The dump is never looked for: the output's place is refused first.
        let dump = dir.path().join("missing.z64");
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        let install = &fixture.root;
        // An expansion's folder, as the installation's own job makes it.
        fs::create_dir(install.join("n64")).unwrap();
        backdate(install).unwrap();
        let installed = snapshot(install).unwrap();
        // In the installation's own folder, where an expansion's folder goes, and in each folder
        // it holds.
        let outputs = [
            install.join("legends"),
            install.join("dreamcast"),
            install.join("n64").join("extracted"),
            install.join("Gauntlet").join("legends"),
            install.join("Gauntlet").join("PDATA").join("legends"),
            install.join("carddemo").join("legends"),
            install.join(".gdl-launcher").join("legends"),
        ];
        for output in &outputs {
            let error = extract_as_script(Expansion::N64, &dump, output).unwrap_err();
            match &error {
                ExtractionError::InsideInstallation { path } => assert_eq!(path, install),
                other => panic!("{}: {other:?}", output.display()),
            }
            assert_eq!(error.rule(), "insideInstallation");
            assert_eq!(code_of(&error), Report::Error("folder.insideInstallation"));
        }
        // The state folder's own name is no output's, in any letter case.
        for name in [".gdl-launcher", ".GDL-Launcher"] {
            let error = extract_as_script(Expansion::N64, &dump, &install.join(name)).unwrap_err();
            assert_name_refused(&error, NameRule::Reserved);
        }
        // The installation is as it was, its own entry included.
        assert!(
            snapshot(install).unwrap() == installed,
            "the installation changed"
        );

        // A folder the alpha's installer installed into, and a folder in it.
        let alpha = dir.path().join("alpha");
        fs::create_dir_all(alpha.join("sub")).unwrap();
        make_alpha_markers(&alpha, &["installation.json"]).unwrap();
        backdate(&alpha).unwrap();
        let alpha_before = snapshot(&alpha).unwrap();
        for output in [alpha.join("legends"), alpha.join("sub").join("legends")] {
            let error = extract_as_script(Expansion::Dreamcast, &dump, &output).unwrap_err();
            match &error {
                ExtractionError::AlphaInstallation { path, name } => {
                    assert_eq!((path, *name), (&alpha, "installation.json"));
                }
                other => panic!("{}: {other:?}", output.display()),
            }
            assert_eq!(error.rule(), "alphaInstallation");
            assert_eq!(code_of(&error), Report::Error("folder.alphaInstall"));
        }
        assert!(
            snapshot(&alpha).unwrap() == alpha_before,
            "the alpha installation changed"
        );
    }

    #[test]
    fn the_folders_above_an_outputs_real_folder_are_looked_at_too() {
        // A drive letter mapped to a folder is no link, and the folders above the one it maps lie
        // beyond its root; the path the system gives for the output's folder shows them. A
        // stand-in for that path tells here what the system would give for a folder reached so:
        // an installation's game folder, and a folder in an alpha installation, each in the form
        // the system gives, which on Windows is verbatim.
        let Some(platform) = Platform::of_this_build() else {
            eprintln!("skipped: no release is built for this system");
            return;
        };
        let dir = tempfile::tempdir().unwrap();
        let runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let fixture = InstallFixture::write(&dir.path().join("install"), runtime).unwrap();
        let alpha = dir.path().join("alpha");
        fs::create_dir_all(alpha.join("sub")).unwrap();
        make_alpha_markers(&alpha, &["installation.json"]).unwrap();
        // The folder as it was given, above which nothing is installed.
        let given = dir.path().join("mapped");
        fs::create_dir(&given).unwrap();
        outside_installations(&given, |path| fs::canonicalize(path)).unwrap();

        // The refusal names the folder that holds the installation's mark as it is shown, without
        // the verbatim prefix.
        let install = fs::canonicalize(&fixture.root).unwrap();
        let game = install.join("Gauntlet");
        let error = outside_installations(&given, |_| Ok(game.clone())).unwrap_err();
        match &error {
            ExtractionError::InsideInstallation { path } => {
                assert_eq!(path, &strip_verbatim(&install));
            }
            other => panic!("{other:?}"),
        }
        let alpha = fs::canonicalize(&alpha).unwrap();
        let sub = alpha.join("sub");
        let error = outside_installations(&given, |_| Ok(sub.clone())).unwrap_err();
        match &error {
            ExtractionError::AlphaInstallation { path, name } => {
                assert_eq!(
                    (path, *name),
                    (&strip_verbatim(&alpha), "installation.json")
                );
            }
            other => panic!("{other:?}"),
        }
        // A folder whose real path the system will not give is refused, since nothing then shows
        // what lies above it.
        let denied = |_: &Path| Err(io::Error::from(io::ErrorKind::PermissionDenied));
        let error = outside_installations(&given, denied).unwrap_err();
        match &error {
            ExtractionError::Io { path, .. } => assert_eq!(path, &given),
            other => panic!("{other:?}"),
        }
        assert_eq!(code_of(&error), Report::Error("io.failed"));
    }

    #[test]
    fn the_wrong_slot_is_refused() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let gamecube = folders.dump("game.iso", &gamecube_image());
        let cases = [
            (&sheet, Expansion::N64, Console::Dreamcast),
            (&rom, Expansion::Dreamcast, Console::Nintendo64),
            (&gamecube, Expansion::N64, Console::GameCube),
            (&gamecube, Expansion::Dreamcast, Console::GameCube),
        ];
        for (dump, wanted, console) in cases {
            let before = folders.before(wanted);
            let output = folders.output(wanted.as_str());
            let error = extract_as_script(wanted, dump, &output).unwrap_err();
            match &error {
                ExtractionError::WrongSlot {
                    found,
                    wanted: asked,
                } => {
                    assert_eq!((*found, *asked), (Some(console), wanted));
                }
                other => panic!("{}: {other:?}", dump.display()),
            }
            assert_eq!(error.rule(), "wrongSlot");
            assert_eq!(code_of(&error), Report::Error("source.wrongSlot"));
            folders.assert_untouched(&before);
        }
        let error = ExtractionError::WrongSlot {
            found: Some(Console::Dreamcast),
            wanted: Expansion::N64,
        };
        assert_eq!(error.to_string(), "the dump is for dreamcast, not for n64");
    }

    #[test]
    fn containers_are_refused_before_the_slot() {
        let folders = Folders::new();
        let cases = [
            (
                "dump.zip",
                b"PK\x03\x04\x14\x00\x00\x00".to_vec(),
                DumpRefusal::Archive,
            ),
            (
                "dump.chd",
                b"MComprHD\x00\x00\x00\x7c\x00\x00\x00\x05".to_vec(),
                DumpRefusal::UnsupportedContainer,
            ),
            // A DiscJuggler image is a Dreamcast dump, refused for its container whichever
            // expansion it is given for.
            ("dump.cdi", cdi_image(), DumpRefusal::Cdi),
            (
                "dump.bin",
                b"Gauntlet Legends (USA)\n".to_vec(),
                DumpRefusal::Unrecognized,
            ),
        ];
        for (name, bytes, refusal) in cases {
            let dump = folders.dump(name, &bytes);
            for expansion in Expansion::ALL {
                let before = folders.before(expansion);
                let output = folders.output(expansion.as_str());
                let error = extract_as_script(expansion, &dump, &output).unwrap_err();
                match &error {
                    ExtractionError::Refused { refusal: found } => assert_eq!(*found, refusal),
                    other => panic!("{name}: {other:?}"),
                }
                assert_eq!(error.rule(), refusal.as_str());
                let code = codes::dump_refusal(refusal);
                assert_eq!(code_of(&error), Report::Error(code), "{name}");
                folders.assert_untouched(&before);
            }
        }
    }

    #[test]
    fn install_publishes_what_the_trust_names() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        // The default image's game code is four NULs, so only its own digest can name it.
        let trust = trust_of(Sha1Digest::of(&image), full_fingerprints());
        let install = StrictnessChoice::Install;

        let before = folders.before(Expansion::N64);
        let output = folders.output("n64");
        let summary = extract(Expansion::N64, &dump, &output, install, &trust).unwrap();
        assert_eq!(summary.strictness, "install");
        folders.assert_published(&before, "n64", &default_tree());

        // A pressed disc's dump of the vectors' full set: every sector keeps its error detection
        // code, and its audio track's file is there.
        let sheet = folders.disc(&DiscSet::full(Edc::Computed).with_audio_files());
        let before = folders.before(Expansion::Dreamcast);
        let output = folders.output("dreamcast");
        let summary = extract(Expansion::Dreamcast, &sheet, &output, install, &trust).unwrap();
        assert_eq!(summary.strictness, "install");
        folders.assert_published(&before, "dreamcast", &full_tree());
    }

    #[test]
    fn install_refuses_what_the_trust_does_not_name() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let sheet = folders.disc(&DiscSet::full(Edc::Computed).with_audio_files());
        let install = StrictnessChoice::Install;

        // Another cartridge's digest: the image's code names no cartridge of the game.
        let mut other = Sha1Digest::of(&image);
        other.0[0] ^= 0x01;
        let shipped = Trust::shipped();
        let other_cartridge = trust_of(other, full_fingerprints());
        for trust in [&shipped, &other_cartridge] {
            let before = folders.before(Expansion::N64);
            let output = folders.output("n64");
            let error = extract(Expansion::N64, &rom, &output, install, trust).unwrap_err();
            match &error {
                ExtractionError::Extract(ExtractError::Refused { refusal }) => {
                    assert_eq!(*refusal, DumpRefusal::OtherGame);
                }
                other => panic!("{other:?}"),
            }
            assert_eq!(error.rule(), "otherGame");
            assert_eq!(code_of(&error), Report::Error("source.otherGame"));
            folders.assert_untouched(&before);
        }

        // The disc against the launcher's own fingerprints, which hold none of its files, and
        // against its own with one of them changed.
        let mut changed = full_fingerprints();
        let disk = format!("{DISC_FOLDER}/{DISK_ROM}");
        let row = changed.iter_mut().find(|(path, _)| *path == disk).unwrap();
        row.1 = Sha256Digest::of(b"other");
        let one_changed = trust_of(US_SHA1, changed);
        for (trust, rule) in [
            (&shipped, FingerprintRule::Missing),
            (&one_changed, FingerprintRule::Digest),
        ] {
            let before = folders.before(Expansion::Dreamcast);
            let output = folders.output("dreamcast");
            let error = extract(Expansion::Dreamcast, &sheet, &output, install, trust).unwrap_err();
            match &error {
                ExtractionError::Extract(ExtractError::Fingerprint { rule: found, .. }) => {
                    assert_eq!(*found, rule);
                }
                other => panic!("{other:?}"),
            }
            assert_eq!(error.rule(), rule.as_str());
            assert_eq!(code_of(&error), Report::Error("dump.hashMismatch"));
            folders.assert_untouched(&before);
        }
    }

    #[test]
    fn identification_leaves_a_cartridges_identity_to_the_check() {
        // The default image given the US cartridge's game code: identifying it finds the code
        // without the US cartridge's digest, an identity that only the check holds to a trust.
        // Under the extractor's strictness it extracts as any ROM does; under an installation's
        // it extracts when the trust names its digest, and the check refuses it when not.
        let folders = Folders::new();
        let (name, mut image) = default_rom();
        image[GAME_CODE].copy_from_slice(US_GAME_CODE);
        let dump = folders.dump(&name, &image);
        let identity = identify(&dump).unwrap();
        assert_eq!(
            identity.verdict,
            Verdict::Refused(DumpRefusal::HashMismatch)
        );
        // The default image's tree, but for its manifest, which records the dump it was
        // extracted from.
        let tree_of = |output: &Path| -> Vec<Listed> {
            let manifest = fs::read(output.join(N64_MANIFEST)).unwrap();
            let recorded: Value = serde_json::from_slice(&manifest).unwrap();
            assert_eq!(recorded["source"]["sha256"], sha256_hex(&image));
            let rows = default_tree().into_iter();
            rows.map(|(path, size, digest)| {
                if path == N64_MANIFEST {
                    (path, manifest.len() as u64, sha256_hex(&manifest))
                } else {
                    (path, size, digest)
                }
            })
            .collect()
        };

        let before = folders.before(Expansion::N64);
        let output = folders.output("script");
        extract_as_script(Expansion::N64, &dump, &output).unwrap();
        folders.assert_published(&before, "script", &tree_of(&output));

        let install = StrictnessChoice::Install;
        let named = trust_of(Sha1Digest::of(&image), full_fingerprints());
        let before = folders.before(Expansion::N64);
        let output = folders.output("install");
        extract(Expansion::N64, &dump, &output, install, &named).unwrap();
        folders.assert_published(&before, "install", &tree_of(&output));

        let shipped = Trust::shipped();
        let before = folders.before(Expansion::N64);
        let output = folders.output("refused");
        let error = extract(Expansion::N64, &dump, &output, install, &shipped).unwrap_err();
        match &error {
            ExtractionError::Extract(ExtractError::Refused { refusal }) => {
                assert_eq!(*refusal, DumpRefusal::HashMismatch);
            }
            other => panic!("{other:?}"),
        }
        assert_eq!(code_of(&error), Report::Error("dump.hashMismatch"));
        folders.assert_untouched(&before);
    }

    #[test]
    fn the_staging_sits_beside_the_output_and_goes() {
        assert_eq!(staging_prefix(Expansion::N64), ".gdl-extract-n64-");
        assert_eq!(
            staging_prefix(Expansion::Dreamcast),
            ".gdl-extract-dreamcast-"
        );
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let trust = Trust::shipped();
        for (expansion, dump, tree) in [
            (Expansion::N64, &rom, default_tree()),
            (Expansion::Dreamcast, &sheet, full_tree()),
        ] {
            let prefix = staging_prefix(expansion);
            let output = folders.output(expansion.as_str());
            // A run stopped as it begins to write, and one that runs to its end.
            for stopped in [true, false] {
                let cancel = Cancel::default();
                let progress = StagingsAtUnpack {
                    parent: &folders.parent,
                    cancel: stopped.then_some(&cancel),
                    found: Mutex::new(Vec::new()),
                };
                let job = Job {
                    cancel: &cancel,
                    progress: &progress,
                };
                let request = ExtractRequest {
                    expansion,
                    dump,
                    output: &output,
                    strictness: StrictnessChoice::Script,
                };
                let before = folders.before(expansion);
                let extracted = extract_to_new_folder(&request, &trust, &job);
                // One staging, beside the output, named after the expansion.
                let found = progress.found.into_inner().unwrap();
                assert_eq!(found.len(), 1, "{found:?}");
                let random = found[0]
                    .strip_prefix(prefix)
                    .expect("the expansion's prefix");
                assert_eq!(random.len(), STAGING_RANDOM_CHARS, "{found:?}");
                assert!(random.bytes().all(|byte| byte.is_ascii_alphanumeric()));
                if stopped {
                    let error = extracted.unwrap_err();
                    assert!(
                        matches!(error, ExtractionError::Extract(ExtractError::Cancelled)),
                        "{error:?}"
                    );
                    assert_eq!(code_of(&error), Report::Cancelled, "{error:?}");
                    folders.assert_staging_gone(&before);
                    assert!(!output.exists());
                } else {
                    let summary = extracted.unwrap();
                    assert_eq!(summary.output, output);
                    folders.assert_published(&before, expansion.as_str(), &tree);
                }
            }
        }
    }

    #[test]
    fn a_script_extraction_publishes_the_golden_tree() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let before = folders.before(Expansion::N64);
        let output = folders.output("n64");
        let summary = extract_as_script(Expansion::N64, &rom, &output).unwrap();
        // The default image's two files, one of them a map's world, and no overlays, since its
        // digest is no cartridge's whose boot table the launcher knows.
        let counts = Counts::N64 {
            files: 2,
            programs: 0,
            maps: 1,
        };
        let expected = ExtractSummary {
            expansion: Expansion::N64,
            strictness: "script",
            output: output.clone(),
            counts,
        };
        assert_eq!(summary, expected);
        folders.assert_published(&before, "n64", &default_tree());

        // The full set, whose sectors keep no error detection code, as the extractor's builder
        // writes them, and whose audio track's file is not there.
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let before = folders.before(Expansion::Dreamcast);
        let output = folders.output("dreamcast");
        let summary = extract_as_script(Expansion::Dreamcast, &sheet, &output).unwrap();
        // The full set's four disc files, and the two files SIZES.ROM places in DISK.ROM and the
        // one it lists outside it.
        let counts = Counts::Dreamcast {
            disc_files: 4,
            native_files: 2,
            external: 1,
        };
        assert_eq!((summary.counts, summary.strictness), (counts, "script"));
        folders.assert_published(&before, "dreamcast", &full_tree());
    }

    #[test]
    fn a_dump_is_the_file_its_path_names_however_the_path_ends() {
        // The extraction scripts take a dump's path by its names, so a separator typed after the
        // file's name, or a `.` and a separator, leaves it the path of that file, which the
        // system would not open so. The tree records the file's own name either way.
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &image);
        let sheet = folders.disc(&DiscSet::full(Edc::Zero));
        let mut endings = vec!["/", "//", "/.", "/./"];
        if cfg!(windows) {
            endings.extend([r"\", r"\\", r"\."]);
        }
        let mut made = 0;
        for (expansion, dump, tree) in [
            (Expansion::N64, &rom, default_tree()),
            (Expansion::Dreamcast, &sheet, full_tree()),
        ] {
            for ending in &endings {
                let mut typed = dump.clone().into_os_string();
                typed.push(ending);
                let typed = PathBuf::from(typed);
                let output = format!("{}{made}", expansion.as_str());
                made += 1;
                let before = folders.before(expansion);
                if let Err(error) = extract_as_script(expansion, &typed, &folders.output(&output)) {
                    panic!("{}: {error:?}", typed.display());
                }
                folders.assert_published(&before, &output, &tree);
            }
        }
    }

    #[test]
    fn a_cancel_at_any_step_leaves_the_folder_as_it_was() {
        let folders = Folders::new();
        let (name, image) = default_rom();
        let dump = folders.dump(&name, &image);
        let trust = Trust::shipped();
        let run = |output: &Path, progress: &CancelAt<'_>| {
            let request = ExtractRequest {
                expansion: Expansion::N64,
                dump: &dump,
                output,
                strictness: StrictnessChoice::Script,
            };
            let job = Job {
                cancel: progress.cancel,
                progress,
            };
            extract_to_new_folder(&request, &trust, &job)
        };
        // A run to the end, to count its advances.
        let cancel = Cancel::default();
        let counting = CancelAt::new(&cancel, usize::MAX);
        let before = folders.before(Expansion::N64);
        run(&folders.output("counted"), &counting).unwrap();
        folders.assert_published(&before, "counted", &default_tree());
        let advances = counting.advances.load(Ordering::SeqCst);
        assert!(advances >= 5, "{advances}");

        // A stop at every one of them, the last included, which comes after the last block of
        // the tree is written and before it is put in place, and one before the run, which the
        // ROM's read heeds first. Each is the extraction's own stop, wherever it was heeded, so
        // that no caller takes one for a reader's failure.
        let output = folders.output("n64");
        for at in 0..=advances {
            let cancel = Cancel::default();
            let progress = CancelAt::new(&cancel, at);
            let before = folders.before(Expansion::N64);
            let error = run(&output, &progress).unwrap_err();
            assert!(
                matches!(error, ExtractionError::Extract(ExtractError::Cancelled)),
                "at {at}: {error:?}"
            );
            assert_eq!(code_of(&error), Report::Cancelled, "at {at}: {error:?}");
            assert_eq!(error.rule(), "cancelled", "at {at}");
            folders.assert_staging_gone(&before);
            assert!(!output.exists(), "at {at}");
        }
    }

    #[test]
    fn a_refusal_in_the_check_comes_before_the_staging() {
        // The N64 extractor's corrupt stream, and a disc without its native table: both refused
        // by the check, so nothing is made, not even the staging.
        let folders = Folders::new();
        let (name, image) = default_rom();
        let rom = folders.dump(&name, &with_bad_stream(&image));
        let sheet = folders.disc(&DiscSet::small(Edc::Zero));
        for (expansion, dump, rule) in [
            (Expansion::N64, &rom, "deflate"),
            (Expansion::Dreamcast, &sheet, "nativeTableMissing"),
        ] {
            let before = folders.before(expansion);
            let output = folders.output(expansion.as_str());
            let error = extract_as_script(expansion, dump, &output).unwrap_err();
            assert!(matches!(error, ExtractionError::Extract(_)), "{error:?}");
            assert_eq!(error.rule(), rule);
            assert_eq!(code_of(&error), Report::Error("source.damaged"));
            folders.assert_untouched(&before);
        }
    }

    #[test]
    fn links_are_placed_where_they_were_found() {
        let staging = PathBuf::from("extracted").join(".gdl-extract-n64-AbC123xy");
        let elsewhere = PathBuf::from("extracted");
        // A link inside the staging was put there by something else.
        let inside = staging.join("n64").join("levels");
        let error = placed(FilesError::Link { path: inside }, &staging);
        assert!(
            matches!(error, ExtractionError::Files(FilesError::Link { .. })),
            "{error:?}"
        );
        assert_eq!(
            (error.rule(), code_of(&error)),
            ("link", Report::Error("io.failed"))
        );
        // One outside it lies on the output's path.
        let error = placed(
            FilesError::Link {
                path: elsewhere.clone(),
            },
            &staging,
        );
        match &error {
            ExtractionError::OutputLinks { path } => assert_eq!(path, &elsewhere),
            other => panic!("{other:?}"),
        }
        assert_eq!(code_of(&error), Report::Error("folder.links"));
        // A name taken at the output is the output being there.
        let output = elsewhere.join("n64");
        let error = placed(
            FilesError::Exists {
                path: output.clone(),
            },
            &staging,
        );
        match &error {
            ExtractionError::OutputExists { path } => assert_eq!(path, &output),
            other => panic!("{other:?}"),
        }
        assert_eq!(code_of(&error), Report::Error("folder.collision"));
    }

    #[test]
    fn every_failure_names_its_rule() {
        let path = PathBuf::from("extracted");
        let denied = std::io::Error::from(std::io::ErrorKind::PermissionDenied);
        let cases = [
            (output_name(Path::new("x.")).unwrap_err(), "outputName"),
            (
                ExtractionError::OutputParent { path: path.clone() },
                "outputParent",
            ),
            (
                ExtractionError::OutputExists { path: path.clone() },
                "outputExists",
            ),
            (
                ExtractionError::OutputLinks { path: path.clone() },
                "outputLinks",
            ),
            (
                ExtractionError::InsideInstallation { path: path.clone() },
                "insideInstallation",
            ),
            (
                ExtractionError::AlphaInstallation {
                    path: path.clone(),
                    name: "installation.json",
                },
                "alphaInstallation",
            ),
            (
                ExtractionError::Io {
                    path: path.clone(),
                    source: denied,
                },
                "io",
            ),
            (
                ExtractionError::WrongSlot {
                    found: None,
                    wanted: Expansion::Dreamcast,
                },
                "wrongSlot",
            ),
            (
                ExtractionError::Refused {
                    refusal: DumpRefusal::Archive,
                },
                "archive",
            ),
            (
                ExtractionError::Media(MediaError::Rom {
                    rule: RomRule::Header,
                    detail: "short".into(),
                }),
                "header",
            ),
            (
                ExtractionError::Extract(ExtractError::Cancelled),
                "cancelled",
            ),
            (
                ExtractionError::Files(FilesError::Exists { path }),
                "exists",
            ),
        ];
        for (error, rule) in cases {
            assert_eq!(error.rule(), rule, "{error:?}");
        }
        // A dump whose content tells no console is told as such.
        let error = ExtractionError::WrongSlot {
            found: None,
            wanted: Expansion::Dreamcast,
        };
        assert_eq!(
            error.to_string(),
            "the dump is for no console the launcher knows, not for dreamcast"
        );

        // Every failure is cheap to return, whatever it carries.
        assert!(size_of::<ExtractionError>() <= MOST_ERROR_BYTES);
    }
}
