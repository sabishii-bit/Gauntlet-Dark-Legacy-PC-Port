//! Judging a folder chosen to install into, before anything is written there: what refuses an
//! install there, what a player should be warned of, and what the judging found, for an install's
//! plan and for a report of the folder. Nothing is written but the entries a folder probe makes
//! and removes at once, and nothing at all in a folder the launcher must not write into: once such
//! a folder is found, the judging ends.

use std::ffi::OsStr;
use std::fmt;
use std::fs;
use std::io;
use std::path::{Component, Path, PathBuf};

#[cfg(any(windows, test))]
use gdl_files::strip_verbatim;
use gdl_files::{
    ALPHA_NAMES, FilesError, FolderProbe, MAX_INSTALL_ROOT_UTF16, ProbedFolder, RelPath, Volume,
    absolute_lexical, is_absent, nearest_existing, probe_folder, root_utf16_len, volume_of,
};

use crate::folder::{STATE_DIR, check_links, markers_among, names_in};
use crate::host::Unsupported;
#[cfg(windows)]
use crate::platform;

/// The attribute of an entry whose data a cloud provider fetches when it is read,
/// FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS.
#[cfg(any(windows, test))]
const RECALL_ON_DATA_ACCESS: u32 = 0x0040_0000;

/// The attribute of an entry that a cloud provider fetches when it is opened,
/// FILE_ATTRIBUTE_RECALL_ON_OPEN.
#[cfg(any(windows, test))]
const RECALL_ON_OPEN: u32 = 0x0004_0000;

/// The attribute of an entry that a cloud provider keeps on the device always,
/// FILE_ATTRIBUTE_PINNED.
#[cfg(any(windows, test))]
const PINNED: u32 = 0x0008_0000;

/// The attribute of an entry that a cloud provider may take off the device when room runs short,
/// FILE_ATTRIBUTE_UNPINNED.
#[cfg(any(windows, test))]
const UNPINNED: u32 = 0x0010_0000;

/// The attribute of an entry whose data is not on the device, FILE_ATTRIBUTE_OFFLINE.
#[cfg(any(windows, test))]
const OFFLINE: u32 = 0x0000_1000;

/// The attributes by which Windows shows that a cloud provider keeps an entry.
#[cfg(any(windows, test))]
const CLOUD_ATTRIBUTES: u32 = RECALL_ON_DATA_ACCESS | RECALL_ON_OPEN | PINNED | UNPINNED | OFFLINE;

/// The reparse tag of a cloud provider's placeholder, IO_REPARSE_TAG_CLOUD, in its first variant.
#[cfg(any(windows, test))]
const CLOUD_REPARSE_TAG: u32 = 0x9000_001A;

/// The bits of a cloud placeholder's reparse tag that name its variant, of which there are
/// sixteen, IO_REPARSE_TAG_CLOUD_MASK.
#[cfg(any(windows, test))]
const CLOUD_TAG_VARIANT: u32 = 0x0000_F000;

/// The first Windows build, version 1903's, that honours the UTF-8 code page the game's manifest
/// asks for: before it, the game's paths go through the system's own code page, which may not
/// hold every character of a path that is not ASCII.
#[cfg(any(windows, test))]
const FIRST_UTF8_BUILD: u32 = 18_362;

/// The last character of the Basic Multilingual Plane, beyond which NTFS folds no letter case.
const LAST_PLANE_ZERO: u32 = 0xFFFF;

/// The variables that name a OneDrive folder. They outlive a sign-out, so they warn of nothing,
/// and are only reported.
#[cfg(windows)]
const ONEDRIVE_VARIABLES: [&str; 3] = ["OneDrive", "OneDriveConsumer", "OneDriveCommercial"];

/// Every known folder that the judging looks for a destination in.
#[cfg(windows)]
const KNOWN_FOLDERS: [KnownFolder; 5] = [
    KnownFolder::Documents,
    KnownFolder::Desktop,
    KnownFolder::Pictures,
    KnownFolder::Videos,
    KnownFolder::Music,
];

/// A folder of the user's that Windows keeps for one kind of file, which Controlled Folder Access,
/// when it is on, keeps programs it does not know from writing into.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KnownFolder {
    /// The user's documents.
    Documents,
    /// The user's desktop.
    Desktop,
    /// The user's pictures.
    Pictures,
    /// The user's videos.
    Videos,
    /// The user's music.
    Music,
}

impl KnownFolder {
    /// The folder's stable machine name, for reports and logs.
    pub fn as_str(self) -> &'static str {
        match self {
            KnownFolder::Documents => "documents",
            KnownFolder::Desktop => "desktop",
            KnownFolder::Pictures => "pictures",
            KnownFolder::Videos => "videos",
            KnownFolder::Music => "music",
        }
    }
}

/// What refuses an install into a folder, as technical detail for logs. What a player reads is
/// chosen from it elsewhere.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Blocker {
    /// The system runs x64 programs, as the launcher and the game are, only by emulating them.
    Unsupported {
        /// The system's own machine, as a program's header names one: 0xAA64 for ARM64.
        native_machine: u16,
    },
    /// A folder on the destination's path has one of the alpha installer's names, which the
    /// launcher would write in making the folders down to the destination or the files below: in
    /// any ASCII letter case, as Windows would make the folder, and, for a folder that is there,
    /// by its full name where the path names it by a short alias.
    AlphaName {
        /// The name, as the alpha installer spells it.
        name: &'static str,
    },
    /// Nothing on the destination's path is there, its root included, as on a drive that does not
    /// exist or holds no medium.
    Unreachable {
        /// The destination.
        path: PathBuf,
    },
    /// The destination, or the nearest entry on its path that is there, is no folder.
    NotDirectory {
        /// The entry that is no folder.
        path: PathBuf,
    },
    /// The destination, or a folder above it, is a link: a symbolic link, or on Windows a junction
    /// or a mount point.
    Links {
        /// The link.
        path: PathBuf,
    },
    /// The destination lies inside a folder that holds an installation's state folder.
    InsideInstallation {
        /// The installation's folder, the nearest above the destination.
        path: PathBuf,
    },
    /// The destination holds the alpha installer's names, by which a folder it installed is known.
    AlphaInstall {
        /// The names it holds, as [`alpha_markers`](crate::alpha_markers) gives them.
        markers: Vec<&'static str>,
    },
    /// The destination holds entries that the install would make, in some letter case, or an
    /// installation's state folder, whether or not the install was to make one.
    Collision {
        /// The names the install would make that are taken, and the state folder's when one is
        /// there, spelled as the install would make them, in byte order.
        names: Vec<String>,
    },
    /// The destination's path is longer than the game can reach files below, on Windows.
    TooLong {
        /// Its length in UTF-16 units.
        units: usize,
    },
    /// The folder that the install would make its entries in refuses them.
    NotWritable {
        /// The folder probed: the destination, or the nearest folder above it that is there.
        path: PathBuf,
    },
    /// The file system that would hold the install ignores letter case, which on Linux keeps the
    /// executable `gauntlet` from standing beside the disc's `Gauntlet` folder.
    IgnoresCase {
        /// The folder probed.
        path: PathBuf,
    },
    /// The file system that would hold the install runs no program, as a Linux `noexec` mount.
    Noexec {
        /// The folder whose file system was asked.
        path: PathBuf,
    },
    /// The volume that would hold the install has too little room for it.
    Space {
        /// The bytes the install needs.
        needed: u64,
        /// The bytes this process may still write there.
        available: u64,
    },
}

impl Blocker {
    /// The finding's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            Blocker::Unsupported { .. } => "unsupported",
            Blocker::AlphaName { .. } => "alphaName",
            Blocker::Unreachable { .. } => "unreachable",
            Blocker::NotDirectory { .. } => "notDirectory",
            Blocker::Links { .. } => "links",
            Blocker::InsideInstallation { .. } => "insideInstallation",
            Blocker::AlphaInstall { .. } => "alphaInstall",
            Blocker::Collision { .. } => "collision",
            Blocker::TooLong { .. } => "tooLong",
            Blocker::NotWritable { .. } => "notWritable",
            Blocker::IgnoresCase { .. } => "ignoresCase",
            Blocker::Noexec { .. } => "noexec",
            Blocker::Space { .. } => "space",
        }
    }
}

/// The host check's finding, as a plan lists it among the folder's.
impl From<Unsupported> for Blocker {
    fn from(unsupported: Unsupported) -> Blocker {
        Blocker::Unsupported {
            native_machine: unsupported.native_machine,
        }
    }
}

impl fmt::Display for Blocker {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Blocker::Unsupported { native_machine } => write!(
                formatter,
                "This system runs x64 programs only through emulation: native machine \
                 0x{native_machine:04X}"
            ),
            Blocker::AlphaName { name } => write!(
                formatter,
                "The destination's path holds the alpha installer's name {name}, which the \
                 launcher never writes"
            ),
            Blocker::Unreachable { path } => write!(
                formatter,
                "No folder on the destination's path exists: {}",
                path.display()
            ),
            Blocker::NotDirectory { .. } => {
                formatter.write_str("Install destination is not a directory")
            }
            Blocker::Links { path } => write!(
                formatter,
                "Choose a directory without links or junctions: {}",
                path.display()
            ),
            Blocker::InsideInstallation { path } => write!(
                formatter,
                "The destination lies inside an installation, whose folder holds {STATE_DIR}: {}",
                path.display()
            ),
            Blocker::AlphaInstall { markers } => write!(
                formatter,
                "The folder holds an alpha installation: {}",
                markers.join(", ")
            ),
            Blocker::Collision { names } => write!(
                formatter,
                "Existing installation/files would be overwritten: {}. Choose a fresh directory.",
                names.join(", ")
            ),
            Blocker::TooLong { units } => write!(
                formatter,
                "The destination's path is {units} UTF-16 units long, over \
                 {MAX_INSTALL_ROOT_UTF16}"
            ),
            Blocker::NotWritable { path } => write!(
                formatter,
                "The launcher may not make files in {}",
                path.display()
            ),
            Blocker::IgnoresCase { path } => write!(
                formatter,
                "The file system at {} ignores letter case, which gauntlet beside Gauntlet needs",
                path.display()
            ),
            Blocker::Noexec { path } => write!(
                formatter,
                "The file system at {} does not run programs (noexec)",
                path.display()
            ),
            Blocker::Space { .. } => {
                formatter.write_str("Not enough free disk space for the installation")
            }
        }
    }
}

/// What a player should be told of a folder that an install may still go into, as technical
/// detail for logs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Warning {
    /// A cloud provider keeps the destination in sync, and may take the game's files off the
    /// device.
    CloudSynced {
        /// The entry on the destination's path that the provider marks, or the folder of the
        /// signed-in OneDrive account that it lies in.
        path: PathBuf,
    },
    /// The destination lies in a known folder, where Controlled Folder Access, when it is on,
    /// keeps the game from saving beside itself.
    Protected {
        /// The known folder.
        folder: KnownFolder,
        /// Where it is.
        path: PathBuf,
    },
    /// The destination's path is not ASCII, on a Windows build that ignores the game's UTF-8 code
    /// page.
    NonAsciiOldWindows {
        /// The system's build.
        build: u32,
    },
}

impl Warning {
    /// The finding's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            Warning::CloudSynced { .. } => "cloudSynced",
            Warning::Protected { .. } => "protected",
            Warning::NonAsciiOldWindows { .. } => "nonAsciiOldWindows",
        }
    }
}

impl fmt::Display for Warning {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Warning::CloudSynced { path } => write!(
                formatter,
                "The destination is kept in sync with the cloud: {}",
                path.display()
            ),
            Warning::Protected { folder, path } => write!(
                formatter,
                "The destination lies in the {} folder, which Controlled Folder Access may guard: \
                 {}",
                folder.as_str(),
                path.display()
            ),
            Warning::NonAsciiOldWindows { build } => write!(
                formatter,
                "This Windows build ({build}) predates 1903, and the destination's path is not \
                 ASCII"
            ),
        }
    }
}

/// What Windows shows of an entry on a destination's path that is there.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ComponentAttributes {
    /// The entry.
    pub path: PathBuf,
    /// Its attributes, as the system gives them.
    pub attributes: u32,
    /// Its reparse tag, when it is a reparse point.
    pub reparse_tag: Option<u32>,
}

/// What judging a folder found: what refuses an install there, what to warn of, and what was
/// asked of the folder and its volume. What the judging did not ask, because it ended before, is
/// `None`, or for the lists empty.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DestinationReport {
    /// The destination, made absolute and folded by name.
    pub path: PathBuf,
    /// Whether the destination is there, a file or a link included.
    pub exists: Option<bool>,
    /// The destination itself when it is there, or else the nearest folder above it that is.
    pub nearest_existing: Option<PathBuf>,
    /// The bytes this process may still write to the volume that holds the nearest folder.
    pub available_bytes: Option<u64>,
    /// The destination's length in UTF-16 units, as Windows' path limits count it.
    pub root_utf16: Option<usize>,
    /// What a probe of the nearest folder found.
    pub probe: Option<FolderProbe>,
    /// Whether the nearest folder's file system runs no program.
    pub noexec: Option<bool>,
    /// What refuses an install there, in the order the judging found them.
    pub blockers: Vec<Blocker>,
    /// What to warn of, on Windows.
    pub warnings: Vec<Warning>,
    /// What Windows shows of each entry on the path that is there, the nearest first.
    pub components: Vec<ComponentAttributes>,
    /// The variables that name a OneDrive folder, by name, as they are set, on Windows.
    pub onedrive_variables: Vec<(String, PathBuf)>,
    /// The folders that the OneDrive accounts signed in keep in sync, on Windows.
    pub cloud_roots: Vec<PathBuf>,
}

impl DestinationReport {
    /// The report of a judging of `path` that has found nothing yet.
    fn new(path: PathBuf) -> DestinationReport {
        DestinationReport {
            path,
            exists: None,
            nearest_existing: None,
            available_bytes: None,
            root_utf16: None,
            probe: None,
            noexec: None,
            blockers: Vec::new(),
            warnings: Vec::new(),
            components: Vec::new(),
            onedrive_variables: Vec::new(),
            cloud_roots: Vec::new(),
        }
    }
}

/// Why a folder could not be judged, as technical detail for logs.
#[derive(Debug, thiserror::Error)]
pub enum DestinationError {
    /// The system refused to look at `path`.
    #[error("{}: {source}", .path.display())]
    Io {
        /// The path looked at: the destination, an entry on its path or beside it, or the folder
        /// whose volume was asked.
        path: PathBuf,
        /// What the system answered.
        #[source]
        source: io::Error,
    },
    /// A look at the path, or the folder probe, failed.
    #[error(transparent)]
    Files(FilesError),
}

impl DestinationError {
    /// The refusal of a look at `path`.
    fn io(path: &Path, source: io::Error) -> DestinationError {
        DestinationError::Io {
            path: path.to_path_buf(),
            source,
        }
    }

    /// The failure's stable machine name, in camelCase, for reports and logs.
    pub fn rule(&self) -> &'static str {
        match self {
            DestinationError::Io { .. } => "io",
            DestinationError::Files(error) => error.rule(),
        }
    }
}

/// What judging a folder asks of the system, so that each system's rules can be tested on both.
pub(crate) struct FolderChecks<'a> {
    /// Probes a folder as [`probe_folder`] does.
    pub(crate) probe: &'a dyn Fn(&Path, ProbedFolder) -> Result<FolderProbe, FilesError>,
    /// Measures a folder's volume as [`volume_of`] does.
    pub(crate) volume: &'a dyn Fn(&Path) -> io::Result<Volume>,
    /// Whether a file system that ignores letter case refuses the folder, as it does on Linux.
    pub(crate) refuse_folded_case: bool,
    /// Whether a file system that runs no program refuses the folder, as it does on Linux.
    pub(crate) refuse_noexec: bool,
}

impl FolderChecks<'static> {
    /// What the launcher asks: the folder probe and the volume's measure themselves, and Linux's
    /// two rules on Linux alone.
    pub(crate) const SHIPPED: FolderChecks<'static> = FolderChecks {
        probe: &probe_folder,
        volume: &volume_of,
        refuse_folded_case: cfg!(target_os = "linux"),
        refuse_noexec: cfg!(target_os = "linux"),
    };
}

/// Judges `destination` as a folder to install into, for an install that makes `names` at its top,
/// each one name, and needs `needed_bytes` of room.
///
/// In order, each finding listed as it is found: a folder on the path with one of the alpha
/// installer's names, as Windows would make it, without the dots and spaces it drops from a name's
/// end or the stream a colon names; nothing on the path there; the nearest entry there no folder;
/// a link on the path; on Windows, a folder there whose full name is one of the alpha installer's,
/// where the path gives it by a short alias; an installation in a folder above; the alpha
/// installer's names in the destination, then names it holds that the install would make,
/// compared in upper case as NTFS compares them; on Windows a path too long; the nearest folder,
/// probed as a destination when it is one, refusing new entries or, on Linux, ignoring letter
/// case; its file system, on Linux, running no program; and its volume too small. Each of the
/// first seven ends the judging, and so does a destination that holds a state folder, another
/// installation's or one being made, so that nothing in such a folder is made, opened or asked of
/// its volume. A judging that goes on to the end warns, on Windows, of a destination that a cloud
/// provider keeps, that lies in a known folder, or whose path is not ASCII on a build before 1903.
///
/// Before anything on the path is looked at, the system is set to fail a call that meets a
/// critical error, as on a drive that holds no medium, rather than show a dialog.
pub fn judge_destination(
    destination: &Path,
    names: &[RelPath],
    needed_bytes: u64,
) -> Result<DestinationReport, DestinationError> {
    judge_with(destination, names, needed_bytes, &FolderChecks::SHIPPED)
}

/// Judges `destination` as [`judge_destination`] does, asking the system through `checks`.
pub(crate) fn judge_with(
    destination: &Path,
    names: &[RelPath],
    needed_bytes: u64,
    checks: &FolderChecks<'_>,
) -> Result<DestinationReport, DestinationError> {
    #[cfg(windows)]
    platform::fail_critical_errors();
    let path = absolute_lexical(destination)
        .map_err(|source| DestinationError::io(destination, source))?;
    let mut report = DestinationReport::new(path);
    judge_into(&mut report, names, needed_bytes, checks)?;
    Ok(report)
}

/// Runs the judging into `report`, which names the destination, returning at the first finding
/// that ends it.
fn judge_into(
    report: &mut DestinationReport,
    names: &[RelPath],
    needed_bytes: u64,
    checks: &FolderChecks<'_>,
) -> Result<(), DestinationError> {
    let path = report.path.clone();
    if let Some(name) = alpha_name_in(&path) {
        report.blockers.push(Blocker::AlphaName { name });
        return Ok(());
    }
    let nearest = match nearest_existing(&path) {
        Ok(nearest) => nearest,
        Err(error) if error.kind() == io::ErrorKind::NotFound => {
            report.exists = Some(false);
            report.blockers.push(Blocker::Unreachable { path });
            return Ok(());
        }
        Err(source) => return Err(DestinationError::io(&path, source)),
    };
    let exists = nearest == path;
    report.exists = Some(exists);
    report.nearest_existing = Some(nearest.clone());
    let found =
        fs::symlink_metadata(&nearest).map_err(|source| DestinationError::io(&nearest, source))?;
    // A link is left to the link check, which names it as one.
    if !found.is_dir() && !found.file_type().is_symlink() {
        report
            .blockers
            .push(Blocker::NotDirectory { path: nearest });
        return Ok(());
    }
    match check_links(&path) {
        Ok(()) => {}
        Err(FilesError::Link { path: link }) => {
            report.blockers.push(Blocker::Links { path: link });
            return Ok(());
        }
        Err(error) => return Err(DestinationError::Files(error)),
    }
    // A short alias names the folder of the name it stands for, which only its full name shows.
    #[cfg(windows)]
    {
        let full = platform::long_path(&nearest)
            .map_err(|source| DestinationError::io(&nearest, source))?;
        if let Some(name) = alpha_name_in(&full) {
            report.blockers.push(Blocker::AlphaName { name });
            return Ok(());
        }
    }
    if let Some(installation) = installation_above(&path)? {
        report
            .blockers
            .push(Blocker::InsideInstallation { path: installation });
        return Ok(());
    }
    if exists {
        let held = names_in(&path).map_err(|source| DestinationError::io(&path, source))?;
        let markers = markers_among(&held);
        if !markers.is_empty() {
            report.blockers.push(Blocker::AlphaInstall { markers });
            return Ok(());
        }
        let held: Vec<String> = held.iter().map(|name| upper_case(name)).collect();
        let mut taken = collisions(names, &held);
        // A state folder is another installation's, or one being made, whatever names the
        // caller asked of: it refuses the folder even when they leave it out.
        let state = upper_case(OsStr::new(STATE_DIR));
        let holds_state = held.contains(&state);
        if holds_state
            && !taken
                .iter()
                .any(|name| upper_case(OsStr::new(name)) == state)
        {
            taken.push(STATE_DIR.to_owned());
            taken.sort();
        }
        if !taken.is_empty() {
            report.blockers.push(Blocker::Collision { names: taken });
        }
        if holds_state {
            return Ok(());
        }
    }
    judge_length(report)?;
    let probed = if exists {
        ProbedFolder::Destination
    } else {
        ProbedFolder::AboveDestination
    };
    let probe = (checks.probe)(&nearest, probed).map_err(DestinationError::Files)?;
    report.probe = Some(probe);
    if !probe.writable {
        report.blockers.push(Blocker::NotWritable {
            path: nearest.clone(),
        });
    }
    if probe.ignores_case && checks.refuse_folded_case {
        report.blockers.push(Blocker::IgnoresCase {
            path: nearest.clone(),
        });
    }
    let volume =
        (checks.volume)(&nearest).map_err(|source| DestinationError::io(&nearest, source))?;
    report.available_bytes = Some(volume.available);
    report.noexec = Some(volume.noexec);
    if volume.noexec && checks.refuse_noexec {
        report.blockers.push(Blocker::Noexec {
            path: nearest.clone(),
        });
    }
    if volume.available < needed_bytes {
        report.blockers.push(Blocker::Space {
            needed: needed_bytes,
            available: volume.available,
        });
    }
    #[cfg(windows)]
    warn(report, &nearest)?;
    Ok(())
}

/// The first of the alpha installer's names that a folder on `path` has, spelled as the alpha
/// installer spells it: each name of the path compared in any ASCII letter case, and as Windows
/// would make a folder of it, without what follows a colon, which names a stream of the entry
/// before it, and without the dots and spaces at its end, which Windows drops from a name it
/// makes. Every system compares so, since elsewhere it only refuses more.
fn alpha_name_in(path: &Path) -> Option<&'static str> {
    path.components().find_map(|component| match component {
        Component::Normal(name) => {
            let bytes = name.as_encoded_bytes();
            let entry = bytes.split(|&byte| byte == b':').next().unwrap_or_default();
            let kept = entry
                .iter()
                .rposition(|&byte| byte != b'.' && byte != b' ')
                .map_or(0, |last| last + 1);
            let made = &entry[..kept];
            ALPHA_NAMES
                .into_iter()
                .find(|alpha| made.eq_ignore_ascii_case(alpha.as_bytes()))
        }
        _ => None,
    })
}

/// The nearest folder above `path` that holds an installation's state folder, a link or any entry
/// of that name included.
fn installation_above(path: &Path) -> Result<Option<PathBuf>, DestinationError> {
    for folder in path.ancestors().skip(1) {
        let state = folder.join(STATE_DIR);
        match fs::symlink_metadata(&state) {
            Ok(_) => return Ok(Some(folder.to_path_buf())),
            Err(error) if is_absent(&error) => {}
            Err(source) => return Err(DestinationError::io(&state, source)),
        }
    }
    Ok(None)
}

/// A name in upper case as NTFS compares names, as near as Unicode's tables give it: NTFS maps each
/// character of the Basic Multilingual Plane to one character, so here each such character takes
/// its simple upper case, and one whose upper case is longer, such as ß or the ligature ﬁ, keeps
/// its form, as does a character beyond that plane.
fn upper_case(name: &OsStr) -> String {
    name.to_string_lossy()
        .chars()
        .map(simple_upper_case)
        .collect()
}

/// `character` in upper case when that is one character of the Basic Multilingual Plane, as
/// `character` is, and otherwise `character` itself.
fn simple_upper_case(character: char) -> char {
    if u32::from(character) > LAST_PLANE_ZERO {
        return character;
    }
    let mut upper = character.to_uppercase();
    match (upper.next(), upper.next()) {
        (Some(one), None) if u32::from(one) <= LAST_PLANE_ZERO => one,
        _ => character,
    }
}

/// The names of `names` that a folder holding names of the upper-case forms `held` holds already,
/// spelled as `names` spells them, in byte order and each once.
fn collisions(names: &[RelPath], held: &[String]) -> Vec<String> {
    let mut taken: Vec<String> = names
        .iter()
        .filter(|name| held.contains(&upper_case(OsStr::new(name.as_str()))))
        .map(|name| name.as_str().to_owned())
        .collect();
    taken.sort();
    taken.dedup();
    taken
}

/// Records the length of the destination that `report` names and, on Windows, refuses one longer
/// than the game can reach files below. Elsewhere a path that has no such length, as one that is
/// not UTF-8, is only not measured.
fn judge_length(report: &mut DestinationReport) -> Result<(), DestinationError> {
    #[cfg(windows)]
    {
        let units = root_utf16_len(&report.path)
            .map_err(|source| DestinationError::io(&report.path, source))?;
        report.root_utf16 = Some(units);
        if units > MAX_INSTALL_ROOT_UTF16 {
            report.blockers.push(Blocker::TooLong { units });
        }
    }
    #[cfg(not(windows))]
    {
        report.root_utf16 = root_utf16_len(&report.path).ok();
    }
    Ok(())
}

/// Adds to `report` what Windows shows of each entry on its destination's path, from `nearest`
/// up, the OneDrive folders it knows of, and the warnings these call for.
#[cfg(windows)]
fn warn(report: &mut DestinationReport, nearest: &Path) -> Result<(), DestinationError> {
    use std::os::windows::fs::MetadataExt;

    /// The attribute of an entry that is a reparse point, FILE_ATTRIBUTE_REPARSE_POINT.
    const REPARSE_POINT: u32 = 0x0400;

    for entry in nearest.ancestors() {
        let attributes = fs::symlink_metadata(entry)
            .map_err(|source| DestinationError::io(entry, source))?
            .file_attributes();
        let reparse_tag = if attributes & REPARSE_POINT == 0 {
            None
        } else {
            platform::reparse_tag(entry).map_err(|source| DestinationError::io(entry, source))?
        };
        report.components.push(ComponentAttributes {
            path: entry.to_path_buf(),
            attributes,
            reparse_tag,
        });
    }
    report.onedrive_variables = ONEDRIVE_VARIABLES
        .into_iter()
        .filter_map(|name| std::env::var_os(name).map(|value| (name.to_owned(), value.into())))
        .collect();
    report.cloud_roots = platform::onedrive_account_folders();

    let marked = report
        .components
        .iter()
        .find(|entry| is_cloud_marked(entry.attributes, entry.reparse_tag))
        .map(|entry| entry.path.clone());
    if let Some(path) = marked.or_else(|| under_cloud_roots(&report.path, &report.cloud_roots)) {
        report.warnings.push(Warning::CloudSynced { path });
    }
    let known: Vec<(KnownFolder, PathBuf)> = KNOWN_FOLDERS
        .into_iter()
        .filter_map(|folder| platform::known_folder(folder).map(|path| (folder, path)))
        .collect();
    if let Some((folder, path)) = protection_of(&report.path, &known) {
        report.warnings.push(Warning::Protected { folder, path });
    }
    if let Some(warning) = non_ascii_warning(platform::windows_build(), &report.path) {
        report.warnings.push(warning);
    }
    Ok(())
}

/// The known folder of `known`, each paired with where it is, that `path` lies in or is: the first
/// that holds it, compared name by name in Unicode upper case, as NTFS compares names.
#[cfg(any(windows, test))]
pub(crate) fn protection_of(
    path: &Path,
    known: &[(KnownFolder, PathBuf)],
) -> Option<(KnownFolder, PathBuf)> {
    known
        .iter()
        .find(|(_, folder)| lies_within(path, folder))
        .cloned()
}

/// Whether an entry of `attributes`, and of the reparse tag `tag` when it is a reparse point, is
/// one a cloud provider keeps: one of the attributes that say so, or a cloud placeholder's tag in
/// any of its variants.
#[cfg(any(windows, test))]
pub(crate) fn is_cloud_marked(attributes: u32, tag: Option<u32>) -> bool {
    attributes & CLOUD_ATTRIBUTES != 0
        || tag.is_some_and(|tag| tag & !CLOUD_TAG_VARIANT == CLOUD_REPARSE_TAG)
}

/// The first of `roots`, folders a cloud provider keeps in sync, that `path` lies in or is,
/// compared as [`protection_of`] compares.
#[cfg(any(windows, test))]
pub(crate) fn under_cloud_roots(path: &Path, roots: &[PathBuf]) -> Option<PathBuf> {
    roots.iter().find(|root| lies_within(path, root)).cloned()
}

/// The warning a Windows build of `build` calls for when `path` is not ASCII: one before 1903
/// ignores the game's UTF-8 code page.
#[cfg(any(windows, test))]
pub(crate) fn non_ascii_warning(build: u32, path: &Path) -> Option<Warning> {
    let ascii = path.as_os_str().as_encoded_bytes().is_ascii();
    (build < FIRST_UTF8_BUILD && !ascii).then_some(Warning::NonAsciiOldWindows { build })
}

/// Whether `path` is `folder` or lies below it, both without a verbatim prefix, comparing each
/// name in Unicode upper case: a folder whose name only begins as one of `path`'s does holds
/// nothing of it.
#[cfg(any(windows, test))]
fn lies_within(path: &Path, folder: &Path) -> bool {
    let path = strip_verbatim(path);
    let folder = strip_verbatim(folder);
    let mut path_names = path.components();
    folder.components().all(|name| {
        path_names
            .next()
            .is_some_and(|own| upper_case(own.as_os_str()) == upper_case(name.as_os_str()))
    })
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;
    use std::ffi::OsStr;
    use std::fs;
    use std::io;
    use std::path::{Path, PathBuf};

    #[cfg(windows)]
    use gdl_files::MAX_INSTALL_ROOT_UTF16;
    use gdl_files::{
        ALPHA_NAMES, FilesError, FolderProbe, PAYLOAD_NAMES, ProbedFolder, RelPath, Volume,
    };
    use gdl_fixtures::{
        InstallFixture, RuntimeFiles, backdate, deny_writes, folder_names, hold_closed, holds_bind,
        link_folder, make_alpha_markers, snapshot, symlink_folder_where_allowed,
    };
    use gdl_release::Platform;

    use super::{
        Blocker, CLOUD_ATTRIBUTES, DestinationError, DestinationReport, FIRST_UTF8_BUILD,
        FolderChecks, KnownFolder, OFFLINE, PINNED, RECALL_ON_DATA_ACCESS, RECALL_ON_OPEN,
        UNPINNED, Warning, is_cloud_marked, judge_destination, judge_with, non_ascii_warning,
        protection_of, under_cloud_roots,
    };
    use crate::MIN_LAUNCHER;
    use crate::folder::STATE_DIR;
    use crate::host::Unsupported;

    /// The most bytes an error may take, so that returning one by value stays cheap.
    const MOST_ERROR_BYTES: usize = 128;

    /// The names at a folder's top that the tests' installs make: some of the disc's, the
    /// runtime's and the launcher's own.
    const NAMES: [&str; 7] = [
        "Gauntlet", "carddemo", "saves", "config", "data", "shaders", STATE_DIR,
    ];

    /// Room no volume has.
    const NEEDED: u64 = u64::MAX / 2;

    /// What a probe finds in a folder of the tests' own, which takes anything: Windows' file
    /// systems ignore letter case, and the Linux tests' do not.
    const WRITABLE: FolderProbe = FolderProbe {
        writable: true,
        ignores_case: cfg!(windows),
    };

    /// What a probe finds in a folder that refuses what an install would make there.
    const NOT_WRITABLE: FolderProbe = FolderProbe {
        writable: false,
        ignores_case: false,
    };

    /// `names`, each read as a name at an install's top.
    fn names_of(names: &[&str]) -> Vec<RelPath> {
        names
            .iter()
            .map(|name| RelPath::parse(name, &PAYLOAD_NAMES).unwrap())
            .collect()
    }

    /// Judges `destination` for an install of [`NAMES`] that needs no room.
    #[track_caller]
    fn judge(destination: &Path) -> DestinationReport {
        judge_destination(destination, &names_of(&NAMES), 0).unwrap()
    }

    /// Checks that the judging that gave `report` ended before it probed the folder or asked its
    /// volume, and so warned of nothing.
    #[track_caller]
    fn assert_unasked(report: &DestinationReport) {
        assert_eq!(report.probe, None, "{report:?}");
        assert_eq!(report.available_bytes, None, "{report:?}");
        assert_eq!(report.noexec, None, "{report:?}");
        assert_eq!(report.root_utf16, None, "{report:?}");
        assert!(report.warnings.is_empty(), "{report:?}");
        assert!(report.components.is_empty(), "{report:?}");
    }

    /// The platform this test program was built for, whose installations it judges.
    fn this_platform() -> Platform {
        Platform::of_this_build().expect("a release is built for the system the tests run on")
    }

    /// `name` with its letters in alternating case, upper first, so that it is spelled in a case
    /// of its own.
    fn odd_case(name: &str) -> String {
        name.chars()
            .enumerate()
            .map(|(index, letter)| {
                if index % 2 == 0 {
                    letter.to_ascii_uppercase()
                } else {
                    letter.to_ascii_lowercase()
                }
            })
            .collect()
    }

    #[test]
    fn a_fresh_folder_is_judged_whole_and_left_as_it_was() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();

        // A destination not there yet, below a folder that is: that folder is probed as one the
        // destination is made in.
        let destination = top.join("game").join("deeper");
        let report = judge(&destination);
        assert_eq!(report.path, destination);
        assert!(report.blockers.is_empty(), "{report:?}");
        assert_eq!(report.exists, Some(false));
        assert_eq!(report.nearest_existing.as_deref(), Some(top));
        assert_eq!(report.probe, Some(WRITABLE));
        assert!(report.available_bytes.is_some_and(|bytes| bytes > 0));
        assert_eq!(report.noexec, Some(false));
        assert_eq!(
            report.root_utf16,
            Some(gdl_files::root_utf16_len(&destination).unwrap())
        );
        assert!(folder_names(top).unwrap().is_empty());

        // A destination that is there, an empty folder: it is probed as the destination.
        let report = judge(top);
        assert!(report.blockers.is_empty(), "{report:?}");
        assert_eq!(report.exists, Some(true));
        assert_eq!(report.nearest_existing.as_deref(), Some(top));
        assert_eq!(report.probe, Some(WRITABLE));
        assert!(folder_names(top).unwrap().is_empty());

        // On Windows each entry on the path is shown, the nearest first, down from the folder
        // itself, which the test made, to the drive's root.
        #[cfg(windows)]
        {
            /// FILE_ATTRIBUTE_DIRECTORY.
            const DIRECTORY: u32 = 0x10;

            let shown: Vec<&Path> = report
                .components
                .iter()
                .map(|entry| entry.path.as_path())
                .collect();
            let ancestors: Vec<&Path> = top.ancestors().collect();
            assert_eq!(shown, ancestors);
            for entry in &report.components {
                assert_ne!(entry.attributes & DIRECTORY, 0, "{entry:?}");
            }
            assert_eq!(report.components[0].reparse_tag, None);
        }
        #[cfg(not(windows))]
        {
            assert!(report.components.is_empty());
            assert!(report.warnings.is_empty());
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_missing_drive_is_unreachable() {
        let missing_drive = (b'A'..=b'Z')
            .rev()
            .map(|letter| PathBuf::from(format!("{}:\\", char::from(letter))))
            .find(|root| {
                fs::symlink_metadata(root)
                    .is_err_and(|error| error.kind() == io::ErrorKind::NotFound)
            });
        let Some(root) = missing_drive else {
            eprintln!("skipped: every drive letter names a drive");
            return;
        };
        let destination = root.join("Games").join("Gauntlet Dark Legacy");
        let report = judge(&destination);
        assert_eq!(
            report.blockers,
            [Blocker::Unreachable {
                path: destination.clone()
            }]
        );
        assert_eq!(report.exists, Some(false));
        assert_eq!(report.nearest_existing, None);
        assert_unasked(&report);
    }

    #[test]
    fn a_file_is_not_a_directory() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("file.txt");
        fs::write(&file, b"kept").unwrap();
        for (destination, exists) in [
            (file.clone(), true),
            (file.join("game"), false),
            (file.join("game").join("deeper"), false),
        ] {
            let report = judge(&destination);
            assert_eq!(
                report.blockers,
                [Blocker::NotDirectory { path: file.clone() }],
                "{}",
                destination.display()
            );
            assert_eq!(report.exists, Some(exists));
            assert_eq!(report.nearest_existing.as_ref(), Some(&file));
            assert_unasked(&report);
        }
        assert_eq!(fs::read(&file).unwrap(), b"kept");
        assert_eq!(folder_names(dir.path()).unwrap(), ["file.txt"]);
    }

    #[test]
    fn a_link_anywhere_on_the_path_refuses() {
        let dir = tempfile::tempdir().unwrap();
        let real = dir.path().join("real");
        fs::create_dir_all(real.join("sub")).unwrap();

        // A link to a folder, which any account can make on either system: on Windows a
        // junction. At the destination, and above it whether what lies below is there or not.
        let link = dir.path().join("link");
        link_folder(&link, &real).unwrap();
        let mut links = vec![link];
        // A symbolic link to a folder, on Windows too, where this account may make one.
        let symlink = dir.path().join("symlink");
        if symlink_folder_where_allowed(&symlink, &real) {
            links.push(symlink);
        } else {
            eprintln!("skipped the symbolic link: this account may not make one");
        }
        for link in links {
            for destination in [
                link.clone(),
                link.join("game"),
                link.join("sub"),
                link.join("sub").join("game"),
            ] {
                let report = judge(&destination);
                assert_eq!(
                    report.blockers,
                    [Blocker::Links { path: link.clone() }],
                    "{}",
                    destination.display()
                );
                assert_unasked(&report);
            }
        }
        // Nothing was made where the links lead.
        assert_eq!(folder_names(&real).unwrap(), ["sub"]);
        assert!(folder_names(&real.join("sub")).unwrap().is_empty());
    }

    #[test]
    fn a_folder_inside_an_installation_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let installation = dir.path().join("installation");
        let runtime = RuntimeFiles::standard(this_platform().as_str(), MIN_LAUNCHER);
        InstallFixture::write(&installation, runtime).unwrap();
        backdate(dir.path()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        for destination in [
            installation.join("saves"),
            installation.join("saves").join("game"),
            installation.join("Gauntlet").join("PDATA"),
            installation.join("new").join("deeper"),
        ] {
            let report = judge(&destination);
            assert_eq!(
                report.blockers,
                [Blocker::InsideInstallation {
                    path: installation.clone()
                }],
                "{}",
                destination.display()
            );
            assert_unasked(&report);
        }
        assert_eq!(snapshot(dir.path()).unwrap(), before);

        // A folder beside an installation is no part of it.
        let report = judge(&dir.path().join("beside"));
        assert!(report.blockers.is_empty(), "{report:?}");
    }

    #[test]
    fn every_alpha_marker_refuses_unopened() {
        // Asked apart from the holds, so that a hold which kept nothing out fails the test.
        let probe = tempfile::tempdir().unwrap();
        let binds = holds_bind(probe.path()).unwrap();
        for name in ALPHA_NAMES {
            let odd = odd_case(name);
            assert_ne!(odd, name);
            let dir = tempfile::tempdir().unwrap();
            let destination = dir.path().join("game");
            fs::create_dir(&destination).unwrap();
            make_alpha_markers(&destination, &[odd.as_str()]).unwrap();
            // Backdated, so that a probe's entry made and removed again would show in the
            // folder's time of change.
            backdate(dir.path()).unwrap();
            let before = snapshot(dir.path()).unwrap();
            // Any open of the marker fails now, but for root on Unix.
            let marker = destination.join(&odd);
            let held = hold_closed(&[marker.as_path()]).unwrap();
            assert_eq!(held.proven(), binds, "{odd}");
            let report = judge(&destination);
            drop(held);
            assert_eq!(
                report.blockers,
                [Blocker::AlphaInstall {
                    markers: vec![name]
                }],
                "{odd}"
            );
            assert_unasked(&report);
            assert_eq!(snapshot(dir.path()).unwrap(), before, "{odd}");
        }
        if !binds {
            eprintln!(
                "only the listing was checked: the proof that no marker is opened needs a user \
                 other than root, who opens entries of mode 0o000"
            );
        }
    }

    #[test]
    fn a_folder_judged_refused_is_never_written() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let alpha = top.join("alpha");
        fs::create_dir(&alpha).unwrap();
        make_alpha_markers(&alpha, &["installation.json"]).unwrap();
        let installation = top.join("installation");
        let runtime = RuntimeFiles::standard(this_platform().as_str(), MIN_LAUNCHER);
        InstallFixture::write(&installation, runtime).unwrap();
        let real = top.join("real");
        fs::create_dir(&real).unwrap();
        let link = top.join("link");
        link_folder(&link, &real).unwrap();
        backdate(top).unwrap();
        let before = snapshot(top).unwrap();

        let mut taken: Vec<String> = NAMES.iter().map(|name| (*name).to_owned()).collect();
        taken.sort();
        let cases = [
            (
                alpha.clone(),
                Blocker::AlphaInstall {
                    markers: vec!["installation.json"],
                },
            ),
            // An installation's top, whose state folder ends the judging.
            (installation.clone(), Blocker::Collision { names: taken }),
            (
                installation.join("saves"),
                Blocker::InsideInstallation {
                    path: installation.clone(),
                },
            ),
            (
                installation.join("saves").join("new"),
                Blocker::InsideInstallation {
                    path: installation.clone(),
                },
            ),
            (link.clone(), Blocker::Links { path: link.clone() }),
        ];
        for (destination, blocker) in cases {
            let report = judge(&destination);
            assert_eq!(report.blockers, [blocker], "{}", destination.display());
            assert_unasked(&report);
            assert_eq!(snapshot(top).unwrap(), before, "{}", destination.display());
        }
        assert!(folder_names(&real).unwrap().is_empty());
    }

    #[test]
    fn alpha_names_in_the_destinations_path_refuse_it() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        fs::create_dir(top.join("GauntletDarkLegacy-Update.EXE")).unwrap();
        backdate(top).unwrap();
        let before = snapshot(top).unwrap();
        for (destination, name) in [
            (top.join(".gdl-update"), ".gdl-update"),
            // An ancestor not there, which the install would make.
            (
                top.join("installation.json").join("game"),
                "installation.json",
            ),
            // An ancestor there, which the install would write below.
            (
                top.join("GauntletDarkLegacy-Update.EXE").join("game"),
                "GauntletDarkLegacy-Update.exe",
            ),
            // Names that Windows makes into an alpha name: it drops the dots and spaces at a
            // name's end as it makes the folder, and takes a colon's stream of a folder's index
            // for the folder itself, whatever making the path absolute kept of them.
            (top.join(".gdl-update ").join("game"), ".gdl-update"),
            (
                top.join("installation.json..").join("game"),
                "installation.json",
            ),
            (
                top.join(".gdl-install.lock. .").join("game"),
                ".gdl-install.lock",
            ),
            (
                top.join("installation.json::$INDEX_ALLOCATION")
                    .join("game"),
                "installation.json",
            ),
            (
                top.join(".GDL-INSTALLER-UPDATE :$I30:$INDEX_ALLOCATION"),
                ".gdl-installer-update",
            ),
            (
                top.join("GauntletDarkLegacy-Update. "),
                "GauntletDarkLegacy-Update",
            ),
        ] {
            let report = judge(&destination);
            assert_eq!(
                report.blockers,
                [Blocker::AlphaName { name }],
                "{}",
                destination.display()
            );
            // Nothing on the path was looked at.
            assert_eq!(report.exists, None);
            assert_eq!(report.nearest_existing, None);
            assert_unasked(&report);
            assert_eq!(snapshot(top).unwrap(), before);
        }

        // Names that only look like the alpha's are none: a name begun with a space or a dot
        // keeps it, and a stream is of the entry before its colon.
        for name in [
            "installation.json.bak",
            " .gdl-update",
            "..gdl-update",
            "x.gdl-update",
            ".gdl-updat",
            "installation",
            "data:installation.json",
            "...",
        ] {
            let path = top.join(name).join("game");
            assert_eq!(super::alpha_name_in(&path), None, "{name:?}");
        }
    }

    /// The short alias of the folder at `folder`: `alias`, where this process may give it one, or
    /// else the one its file system gave it, or `None` where it has none.
    #[cfg(windows)]
    fn short_alias_of(folder: &Path, alias: &str) -> Option<String> {
        use crate::platform::{set_short_name, short_path};

        if set_short_name(folder, alias).is_ok() {
            return Some(alias.to_owned());
        }
        let short = short_path(folder).ok()?;
        let alias = short.file_name()?.to_str()?.to_owned();
        (Some(alias.as_str()) != folder.file_name().and_then(|name| name.to_str())).then_some(alias)
    }

    #[cfg(windows)]
    #[test]
    fn a_short_alias_of_an_alpha_name_refuses_it() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let alpha = top.join(".gdl-update");
        fs::create_dir(&alpha).unwrap();
        let Some(alias) = short_alias_of(&alpha, "GDLUPD~1") else {
            eprintln!(
                "skipped: this process may give no folder a short alias, and the file system gave \
                 none"
            );
            return;
        };
        let aliased = top.join(&alias);
        assert!(fs::symlink_metadata(&aliased).unwrap().is_dir(), "{alias}");
        backdate(top).unwrap();
        let before = snapshot(top).unwrap();
        // The alias is the destination, a folder above it, and one above a folder not there yet:
        // the folder of the alpha's name is never probed nor written below.
        for destination in [
            aliased.clone(),
            aliased.join("game"),
            aliased.join("game").join("deeper"),
        ] {
            let report = judge(&destination);
            assert_eq!(
                report.blockers,
                [Blocker::AlphaName {
                    name: ".gdl-update"
                }],
                "{}",
                destination.display()
            );
            assert_unasked(&report);
            assert_eq!(snapshot(top).unwrap(), before, "{}", destination.display());
        }
        assert!(folder_names(&alpha).unwrap().is_empty());

        // A short alias of any other name is judged as that folder.
        let other = top.join("a folder of a long name");
        fs::create_dir(&other).unwrap();
        if let Some(alias) = short_alias_of(&other, "AFOLDE~1") {
            let report = judge(&top.join(alias).join("game"));
            assert!(report.blockers.is_empty(), "{report:?}");
            assert!(folder_names(&other).unwrap().is_empty());
        }
    }

    #[test]
    fn collisions_compare_names_as_ntfs_does() {
        let dir = tempfile::tempdir().unwrap();
        let destination = dir.path().join("game");
        fs::create_dir(&destination).unwrap();
        fs::write(destination.join("DATA"), b"mine").unwrap();
        // A long s, whose upper case is a plain S.
        fs::create_dir(destination.join("\u{17f}aves")).unwrap();
        // A Kelvin sign, which is upper case already and so never meets a plain k.
        fs::write(destination.join("\u{212a}ept"), b"mine").unwrap();
        // The ligature fi, whose upper case is two letters, which NTFS's table of one character
        // for each never gives it, so that it never meets a plain fi.
        fs::write(destination.join("con\u{fb01}g"), b"mine").unwrap();
        let names = names_of(&["data", "saves", "kept", "config", "Gauntlet"]);
        let report = judge_destination(&destination, &names, 0).unwrap();
        assert_eq!(
            report.blockers,
            [Blocker::Collision {
                names: vec!["data".to_owned(), "saves".to_owned()]
            }]
        );
        // Taken names leave the judging going on.
        assert_eq!(report.probe, Some(WRITABLE));
        assert_eq!(fs::read(destination.join("DATA")).unwrap(), b"mine");
        assert_eq!(folder_names(&destination).unwrap().len(), 4);

        // Each character of the first plane in its one-character upper case, and the rest as
        // they are.
        for (name, upper) in [
            ("saves", "SAVES"),
            ("\u{17f}aves", "SAVES"),
            ("\u{212a}ept", "\u{212a}EPT"),
            ("con\u{fb01}g", "CON\u{fb01}G"),
            ("stra\u{df}e", "STRA\u{df}E"),
            ("\u{e9}t\u{e9}", "\u{c9}T\u{c9}"),
            ("\u{10428}x", "\u{10428}X"),
        ] {
            assert_eq!(super::upper_case(OsStr::new(name)), upper, "{name}");
        }
    }

    #[test]
    fn a_state_folder_refuses_whatever_names_are_asked() {
        // A folder whose state folder is another installation's, or one being made, and which
        // holds a name that some installs make.
        let dir = tempfile::tempdir().unwrap();
        let destination = dir.path().join("game");
        fs::create_dir_all(destination.join(STATE_DIR)).unwrap();
        fs::write(destination.join("DATA"), b"mine").unwrap();
        backdate(dir.path()).unwrap();
        let before = snapshot(dir.path()).unwrap();
        let cases: [(&[&str], &[&str]); 4] = [
            (&[], &[STATE_DIR]),
            (&["saves"], &[STATE_DIR]),
            (&["data"], &[STATE_DIR, "data"]),
            (&NAMES, &[STATE_DIR, "data"]),
        ];
        for (names, taken) in cases {
            let report = judge_destination(&destination, &names_of(names), 0).unwrap();
            let taken: Vec<String> = taken.iter().map(|name| (*name).to_owned()).collect();
            assert_eq!(
                report.blockers,
                [Blocker::Collision { names: taken }],
                "{names:?}"
            );
            // The judging ended there.
            assert_unasked(&report);
            assert_eq!(snapshot(dir.path()).unwrap(), before, "{names:?}");
        }
    }

    #[cfg(windows)]
    #[test]
    fn a_root_of_181_units_is_too_long() {
        let base = tempfile::tempdir().unwrap();
        let fits =
            gdl_fixtures::long_root(&base.path().join("fits"), MAX_INSTALL_ROOT_UTF16).unwrap();
        let report = judge(&fits);
        assert!(report.blockers.is_empty(), "{report:?}");
        assert_eq!(report.root_utf16, Some(MAX_INSTALL_ROOT_UTF16));

        let long =
            gdl_fixtures::long_root(&base.path().join("long"), MAX_INSTALL_ROOT_UTF16 + 1).unwrap();
        let report = judge(&long);
        assert_eq!(
            report.blockers,
            [Blocker::TooLong {
                units: MAX_INSTALL_ROOT_UTF16 + 1
            }]
        );
        // The judging goes on.
        assert_eq!(report.probe, Some(WRITABLE));

        // A destination not there yet is measured as it would be made.
        let report = judge(&fits.join("x"));
        assert_eq!(
            report.blockers,
            [Blocker::TooLong {
                units: MAX_INSTALL_ROOT_UTF16 + 2
            }]
        );
        assert!(folder_names(&fits).unwrap().is_empty());
    }

    #[test]
    fn a_folder_without_writes_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let destination = dir.path().join("game");
        fs::create_dir(&destination).unwrap();
        let denied = deny_writes(&destination).unwrap();
        if !denied.proven() {
            eprintln!("skipped: this user makes files in a folder denied writes, as root does");
            return;
        }
        let report = judge(&destination);
        assert_eq!(
            report.blockers,
            [Blocker::NotWritable {
                path: destination.clone()
            }]
        );
        assert_eq!(report.probe, Some(NOT_WRITABLE));
        assert!(folder_names(&destination).unwrap().is_empty());

        // A destination below it would be made there as a new folder, which on Windows a process
        // whose restore privilege is enabled still makes, as the guard's check found.
        let report = judge(&destination.join("deeper"));
        if denied.refuses_folders() {
            assert_eq!(
                report.blockers,
                [Blocker::NotWritable {
                    path: destination.clone()
                }]
            );
        } else {
            assert!(report.blockers.is_empty(), "{report:?}");
        }
        assert!(folder_names(&destination).unwrap().is_empty());
        drop(denied);
    }

    /// A folder that takes new folders but no new file, as the top of a Windows system drive is
    /// for any user, takes a destination below it but is no destination itself.
    #[cfg(windows)]
    #[test]
    fn a_folder_taking_only_folders_takes_a_destination_below_it() {
        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("drive top");
        fs::create_dir(&folder).unwrap();
        let denied = gdl_fixtures::deny_new_files(&folder).unwrap();

        let report = judge(&folder.join("Games").join("Gauntlet Dark Legacy"));
        assert!(report.blockers.is_empty(), "{report:?}");
        assert_eq!(report.probe, Some(WRITABLE));
        let report = judge(&folder);
        assert_eq!(
            report.blockers,
            [Blocker::NotWritable {
                path: folder.clone()
            }]
        );
        assert!(folder_names(&folder).unwrap().is_empty());
        drop(denied);
    }

    /// The variable that names a folder on a file system mounted noexec, where the system the
    /// tests run on has one.
    #[cfg(target_os = "linux")]
    const NOEXEC_MOUNT: &str = "GDL_NOEXEC_MOUNT";

    #[cfg(target_os = "linux")]
    #[test]
    fn a_noexec_mount_is_refused() {
        let Some(mount) = std::env::var_os(NOEXEC_MOUNT) else {
            eprintln!("skipped: {NOEXEC_MOUNT} names no folder on a noexec mount");
            return;
        };
        // A folder of the test's own on the mount, so that the probe writes nowhere else.
        let dir = tempfile::tempdir_in(mount).unwrap();
        let report = judge(&dir.path().join("game"));
        assert_eq!(
            report.blockers,
            [Blocker::Noexec {
                path: dir.path().to_path_buf()
            }]
        );
        assert_eq!(report.noexec, Some(true));
        assert!(folder_names(dir.path()).unwrap().is_empty());
    }

    #[test]
    fn the_blockers_come_in_order_on_both_systems() {
        let dir = tempfile::tempdir().unwrap();
        let top = dir.path();
        let destination = top.join("game");
        let asked = RefCell::new(Vec::new());
        // A folder that refuses new entries and ignores letter case, on a volume of no room
        // that runs no program: more than any one file system shows.
        let probe = |folder: &Path, probed: ProbedFolder| -> Result<FolderProbe, FilesError> {
            asked.borrow_mut().push((folder.to_path_buf(), probed));
            Ok(FolderProbe {
                writable: false,
                ignores_case: true,
            })
        };
        let volume = |_: &Path| -> io::Result<Volume> {
            Ok(Volume {
                available: 0,
                noexec: true,
            })
        };
        let not_writable = Blocker::NotWritable {
            path: top.to_path_buf(),
        };
        let space = Blocker::Space {
            needed: 1,
            available: 0,
        };
        for (linux_rules, expected) in [
            (
                true,
                vec![
                    not_writable.clone(),
                    Blocker::IgnoresCase {
                        path: top.to_path_buf(),
                    },
                    Blocker::Noexec {
                        path: top.to_path_buf(),
                    },
                    space.clone(),
                ],
            ),
            (false, vec![not_writable.clone(), space.clone()]),
        ] {
            let checks = FolderChecks {
                probe: &probe,
                volume: &volume,
                refuse_folded_case: linux_rules,
                refuse_noexec: linux_rules,
            };
            let report = judge_with(&destination, &names_of(&NAMES), 1, &checks).unwrap();
            assert_eq!(report.blockers, expected, "{linux_rules}");
            assert_eq!(report.noexec, Some(true));
        }
        // The folder the destination would be made in was probed as such, and a destination
        // that is there as itself.
        fs::create_dir(&destination).unwrap();
        let checks = FolderChecks {
            probe: &probe,
            volume: &volume,
            refuse_folded_case: false,
            refuse_noexec: false,
        };
        judge_with(&destination, &names_of(&NAMES), 0, &checks).unwrap();
        assert_eq!(
            *asked.borrow(),
            [
                (top.to_path_buf(), ProbedFolder::AboveDestination),
                (top.to_path_buf(), ProbedFolder::AboveDestination),
                (destination.clone(), ProbedFolder::Destination),
            ]
        );

        // A probe that fails, and a volume that cannot be measured, fail the judging.
        let failing_probe = |folder: &Path, _: ProbedFolder| -> Result<FolderProbe, FilesError> {
            Err(FilesError::Io {
                path: folder.to_path_buf(),
                source: io::Error::other("probe"),
            })
        };
        let failing_volume = |_: &Path| -> io::Result<Volume> { Err(io::Error::other("volume")) };
        let checks = FolderChecks {
            probe: &failing_probe,
            volume: &volume,
            refuse_folded_case: false,
            refuse_noexec: false,
        };
        match judge_with(&destination, &[], 0, &checks) {
            Err(DestinationError::Files(FilesError::Io { path, .. })) => {
                assert_eq!(path, destination);
            }
            other => panic!("{other:?}"),
        }
        let checks = FolderChecks {
            probe: &probe,
            volume: &failing_volume,
            refuse_folded_case: false,
            refuse_noexec: false,
        };
        match judge_with(&destination, &[], 0, &checks) {
            Err(DestinationError::Io { path, source }) => {
                assert_eq!(path, destination);
                assert_eq!(source.to_string(), "volume");
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn too_little_space_is_refused() {
        let dir = tempfile::tempdir().unwrap();
        let report =
            judge_destination(&dir.path().join("game"), &names_of(&NAMES), NEEDED).unwrap();
        let available = report.available_bytes.unwrap();
        assert!(available < NEEDED);
        assert_eq!(
            report.blockers,
            [Blocker::Space {
                needed: NEEDED,
                available
            }]
        );
    }

    #[test]
    fn the_report_lists_what_it_found() {
        let base = tempfile::tempdir().unwrap();
        // On Windows a path too long as well.
        #[cfg(windows)]
        let destination = gdl_fixtures::long_root(base.path(), MAX_INSTALL_ROOT_UTF16 + 1).unwrap();
        #[cfg(not(windows))]
        let destination = {
            let destination = base.path().join("game");
            fs::create_dir(&destination).unwrap();
            destination
        };
        fs::create_dir(destination.join("saves")).unwrap();
        let denied = deny_writes(&destination).unwrap();
        if !denied.proven() {
            eprintln!("skipped: this user makes files in a folder denied writes, as root does");
            return;
        }
        let report = judge_destination(&destination, &names_of(&NAMES), NEEDED).unwrap();
        drop(denied);
        let available = report.available_bytes.unwrap();
        let mut expected = vec![Blocker::Collision {
            names: vec!["saves".to_owned()],
        }];
        #[cfg(windows)]
        expected.push(Blocker::TooLong {
            units: MAX_INSTALL_ROOT_UTF16 + 1,
        });
        expected.push(Blocker::NotWritable {
            path: destination.clone(),
        });
        expected.push(Blocker::Space {
            needed: NEEDED,
            available,
        });
        assert_eq!(report.blockers, expected);
        assert_eq!(report.path, destination);
        assert_eq!(report.exists, Some(true));
        assert_eq!(report.nearest_existing.as_ref(), Some(&destination));
        assert_eq!(report.probe, Some(NOT_WRITABLE));
        assert_eq!(report.noexec, Some(false));
        assert_eq!(
            report.root_utf16,
            Some(gdl_files::root_utf16_len(&destination).unwrap())
        );
        #[cfg(windows)]
        assert_eq!(report.components[0].path, destination);
        assert_eq!(folder_names(&destination).unwrap(), ["saves"]);
    }

    #[test]
    fn cloud_marks_follow_the_attributes_and_the_tag() {
        /// FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_ARCHIVE and
        /// FILE_ATTRIBUTE_REPARSE_POINT, which no cloud provider needs.
        const PLAIN: [u32; 4] = [0x1, 0x10, 0x20, 0x400];
        /// IO_REPARSE_TAG_MOUNT_POINT and IO_REPARSE_TAG_SYMLINK, the tags of a junction and of a
        /// symbolic link.
        const LINK_TAGS: [u32; 2] = [0xA000_0003, 0xA000_000C];

        for attribute in [
            RECALL_ON_DATA_ACCESS,
            RECALL_ON_OPEN,
            PINNED,
            UNPINNED,
            OFFLINE,
        ] {
            assert!(is_cloud_marked(attribute, None), "{attribute:#x}");
            assert!(is_cloud_marked(attribute | 0x10, None), "{attribute:#x}");
        }
        assert_eq!(CLOUD_ATTRIBUTES.count_ones(), 5);
        for attributes in PLAIN {
            assert!(!is_cloud_marked(attributes, None), "{attributes:#x}");
        }
        // A cloud placeholder's tag in its first variant, another and its last.
        for tag in [0x9000_001A, 0x9000_101A, 0x9000_F01A] {
            assert!(is_cloud_marked(0x400, Some(tag)), "{tag:#x}");
        }
        // A link's tag, and tags that differ from the cloud's outside its variant.
        for tag in LINK_TAGS
            .into_iter()
            .chain([0x8000_001A, 0x9000_001B, 0x9001_001A])
        {
            assert!(!is_cloud_marked(0x400, Some(tag)), "{tag:#x}");
        }
    }

    /// A folder of the user's, on each system as it would be spelt there.
    fn home() -> PathBuf {
        if cfg!(windows) {
            PathBuf::from(r"C:\Users\Player")
        } else {
            PathBuf::from("/home/player")
        }
    }

    #[test]
    fn onedrive_account_folders_cover_their_folders() {
        let home = home();
        let personal = home.join("OneDrive");
        let work = home.join("OneDrive - Work");
        let roots = [personal.clone(), work.clone()];
        for (path, root) in [
            (personal.join("Games").join("Gauntlet"), Some(&personal)),
            (personal.clone(), Some(&personal)),
            // Each name compared in upper case.
            (home.join("ONEDRIVE").join("games"), Some(&personal)),
            (work.join("game"), Some(&work)),
            // A sibling whose name begins as the folder's is not inside it.
            (home.join("OneDriveBackup").join("game"), None),
            (home.join("game"), None),
            (home.clone(), None),
        ] {
            assert_eq!(
                under_cloud_roots(&path, &roots).as_ref(),
                root,
                "{}",
                path.display()
            );
        }
        assert_eq!(under_cloud_roots(&personal.join("game"), &[]), None);
    }

    #[test]
    fn protection_follows_the_known_folders() {
        let home = home();
        let known = [
            (KnownFolder::Documents, home.join("Documents")),
            (KnownFolder::Desktop, home.join("Desktop")),
            (KnownFolder::Pictures, home.join("Pictures")),
            (KnownFolder::Videos, home.join("Videos")),
            (KnownFolder::Music, home.join("Music")),
        ];
        for (folder, path) in &known {
            let game = path.join("Games").join("Gauntlet");
            assert_eq!(
                protection_of(&game, &known),
                Some((*folder, path.clone())),
                "{}",
                game.display()
            );
            assert_eq!(
                protection_of(path, &known),
                Some((*folder, path.clone())),
                "{}",
                path.display()
            );
        }
        // Each name compared in upper case, as NTFS compares names.
        let shouted = home.join("DOCUMENTS").join("game");
        assert_eq!(
            protection_of(&shouted, &known),
            Some((KnownFolder::Documents, home.join("Documents")))
        );
        for outside in [
            home.join("Games"),
            home.join("DocumentsOld").join("game"),
            home.clone(),
        ] {
            assert_eq!(
                protection_of(&outside, &known),
                None,
                "{}",
                outside.display()
            );
        }
        assert_eq!(protection_of(&home.join("Documents"), &[]), None);
        // The known folders' names for reports.
        let names: Vec<&str> = known.iter().map(|(folder, _)| folder.as_str()).collect();
        assert_eq!(
            names,
            ["documents", "desktop", "pictures", "videos", "music"]
        );
    }

    #[cfg(windows)]
    #[test]
    fn the_known_folders_are_absolute() {
        for folder in super::KNOWN_FOLDERS {
            if let Some(path) = crate::platform::known_folder(folder) {
                assert!(path.is_absolute(), "{folder:?}: {}", path.display());
            }
        }
    }

    #[test]
    fn old_windows_warns_of_a_non_ascii_path() {
        let path = home().join("Spiele").join("Gauntlet \u{fc}ber alles");
        let ascii = home().join("Games").join("Gauntlet");
        assert_eq!(
            non_ascii_warning(17_763, &path),
            Some(Warning::NonAsciiOldWindows { build: 17_763 })
        );
        assert_eq!(
            non_ascii_warning(FIRST_UTF8_BUILD - 1, &path),
            Some(Warning::NonAsciiOldWindows {
                build: FIRST_UTF8_BUILD - 1
            })
        );
        for build in [FIRST_UTF8_BUILD, 19_045, 26_100] {
            assert_eq!(non_ascii_warning(build, &path), None, "{build}");
        }
        assert_eq!(non_ascii_warning(17_763, &ascii), None);
    }

    /// The flags of the system's own that the judging's constants stand for.
    #[cfg(windows)]
    #[test]
    fn the_cloud_attributes_are_the_systems() {
        use windows_sys::Win32::Storage::FileSystem as files;

        assert_eq!(
            RECALL_ON_DATA_ACCESS,
            files::FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS
        );
        assert_eq!(RECALL_ON_OPEN, files::FILE_ATTRIBUTE_RECALL_ON_OPEN);
        assert_eq!(PINNED, files::FILE_ATTRIBUTE_PINNED);
        assert_eq!(UNPINNED, files::FILE_ATTRIBUTE_UNPINNED);
        assert_eq!(OFFLINE, files::FILE_ATTRIBUTE_OFFLINE);
    }

    #[test]
    fn findings_name_their_rules_and_say_what_they_found() {
        let path = Path::new("games").join("gauntlet");
        let shown = path.display();
        let blockers = [
            (
                Blocker::from(Unsupported {
                    native_machine: 0xAA64,
                }),
                "unsupported",
                "This system runs x64 programs only through emulation: native machine 0xAA64"
                    .to_owned(),
            ),
            (
                Blocker::AlphaName {
                    name: ".gdl-update",
                },
                "alphaName",
                "The destination's path holds the alpha installer's name .gdl-update, which the \
                 launcher never writes"
                    .to_owned(),
            ),
            (
                Blocker::Unreachable { path: path.clone() },
                "unreachable",
                format!("No folder on the destination's path exists: {shown}"),
            ),
            (
                Blocker::NotDirectory { path: path.clone() },
                "notDirectory",
                "Install destination is not a directory".to_owned(),
            ),
            (
                Blocker::Links { path: path.clone() },
                "links",
                format!("Choose a directory without links or junctions: {shown}"),
            ),
            (
                Blocker::InsideInstallation { path: path.clone() },
                "insideInstallation",
                format!(
                    "The destination lies inside an installation, whose folder holds \
                     .gdl-launcher: {shown}"
                ),
            ),
            (
                Blocker::AlphaInstall {
                    markers: vec!["installation.json", ".gdl-update"],
                },
                "alphaInstall",
                "The folder holds an alpha installation: installation.json, .gdl-update".to_owned(),
            ),
            (
                Blocker::Collision {
                    names: vec!["config".to_owned(), "data".to_owned()],
                },
                "collision",
                "Existing installation/files would be overwritten: config, data. Choose a fresh \
                 directory."
                    .to_owned(),
            ),
            (
                Blocker::TooLong { units: 181 },
                "tooLong",
                "The destination's path is 181 UTF-16 units long, over 180".to_owned(),
            ),
            (
                Blocker::NotWritable { path: path.clone() },
                "notWritable",
                format!("The launcher may not make files in {shown}"),
            ),
            (
                Blocker::IgnoresCase { path: path.clone() },
                "ignoresCase",
                format!(
                    "The file system at {shown} ignores letter case, which gauntlet beside \
                     Gauntlet needs"
                ),
            ),
            (
                Blocker::Noexec { path: path.clone() },
                "noexec",
                format!("The file system at {shown} does not run programs (noexec)"),
            ),
            (
                Blocker::Space {
                    needed: 2,
                    available: 1,
                },
                "space",
                "Not enough free disk space for the installation".to_owned(),
            ),
        ];
        for (blocker, rule, text) in blockers {
            assert_eq!(blocker.rule(), rule);
            assert_eq!(blocker.to_string(), text);
        }

        let warnings = [
            (
                Warning::CloudSynced { path: path.clone() },
                "cloudSynced",
                format!("The destination is kept in sync with the cloud: {shown}"),
            ),
            (
                Warning::Protected {
                    folder: KnownFolder::Documents,
                    path: path.clone(),
                },
                "protected",
                format!(
                    "The destination lies in the documents folder, which Controlled Folder \
                     Access may guard: {shown}"
                ),
            ),
            (
                Warning::NonAsciiOldWindows { build: 17_763 },
                "nonAsciiOldWindows",
                "This Windows build (17763) predates 1903, and the destination's path is not \
                 ASCII"
                    .to_owned(),
            ),
        ];
        for (warning, rule, text) in warnings {
            assert_eq!(warning.rule(), rule);
            assert_eq!(warning.to_string(), text);
        }

        // A judging that failed names what it looked at, or keeps the file check's failure.
        let refused = DestinationError::Io {
            path: path.clone(),
            source: io::Error::new(io::ErrorKind::PermissionDenied, "denied"),
        };
        assert_eq!(refused.rule(), "io");
        assert_eq!(refused.to_string(), format!("{shown}: denied"));
        let link = DestinationError::Files(FilesError::Link { path: path.clone() });
        assert_eq!(link.rule(), "link");
        assert_eq!(
            link.to_string(),
            format!("Choose a directory without links or junctions: {shown}")
        );
        assert!(size_of::<DestinationError>() <= MOST_ERROR_BYTES);
    }

    /// The variable that makes a run of this test program judge a folder from the system's default
    /// error mode, in `a_judging_fails_critical_errors_rather_than_showing_them`. Unset, as in a
    /// run of the tests, that part does nothing.
    #[cfg(windows)]
    const ERROR_MODE_CHILD: &str = "GDL_DESTINATION_ERROR_MODE_CHILD";

    /// The test that a child run of this program, started in the default error mode, runs alone.
    #[cfg(windows)]
    const JUDGING_TEST: &str = "destination::tests::judging_from_the_default_error_mode";

    /// The exit code of a child run whose every check of the error mode held.
    #[cfg(windows)]
    const ERROR_MODE_CHECKED: i32 = 75;

    #[cfg(windows)]
    #[test]
    fn a_judging_fails_critical_errors_rather_than_showing_them() {
        use std::os::windows::process::CommandExt;
        use std::process::{Command, Stdio};
        use windows_sys::Win32::System::Threading::{CREATE_DEFAULT_ERROR_MODE, CREATE_NO_WINDOW};

        // A program inherits its error mode from what starts it, and the shells and runners the
        // tests run under fail critical errors already, so the judging is made in a child of this
        // program started in the system's default mode, which shows them, as a launcher started
        // from Explorer is.
        let output = Command::new(std::env::current_exe().unwrap())
            .args(["--exact", JUDGING_TEST, "--test-threads", "1"])
            .env(ERROR_MODE_CHILD, "1")
            .creation_flags(CREATE_DEFAULT_ERROR_MODE | CREATE_NO_WINDOW)
            .stdin(Stdio::null())
            .output()
            .unwrap();
        assert_eq!(
            output.status.code(),
            Some(ERROR_MODE_CHECKED),
            "{}{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
    }

    /// The child's part of `a_judging_fails_critical_errors_rather_than_showing_them`.
    #[cfg(windows)]
    #[test]
    fn judging_from_the_default_error_mode() {
        use crate::platform::{error_mode, refuse_fault_dialogs};
        use windows_sys::Win32::System::Diagnostics::Debug::SEM_FAILCRITICALERRORS;

        if std::env::var_os(ERROR_MODE_CHILD).is_none() {
            return;
        }
        refuse_fault_dialogs();
        let started = error_mode();
        assert_eq!(started & SEM_FAILCRITICALERRORS, 0, "{started:#x}");
        let dir = tempfile::tempdir().unwrap();
        let report = judge(&dir.path().join("game"));
        assert!(report.blockers.is_empty(), "{report:?}");
        // Critical errors fail calls from the judging on, and the rest of the mode is as it was.
        assert_eq!(error_mode(), started | SEM_FAILCRITICALERRORS);
        // Removed before the exit, which runs no destructor.
        dir.close().unwrap();
        std::process::exit(ERROR_MODE_CHECKED);
    }
}
