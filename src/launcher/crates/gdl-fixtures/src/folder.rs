//! Install folders in the states the launcher's tests need: roots of an exact length, links to
//! folders and files, the names the alpha installer leaves, entries only root may open, folders
//! that take no new entries and installations as the launcher leaves them; and the names a folder
//! holds, what a tree holds, all of it or all but a job's staging, and a tree's files as the
//! shared vectors list them, to compare.

use std::collections::BTreeMap;
#[cfg(windows)]
use std::ffi::OsStr;
use std::fs;
use std::io::{self, Write};
use std::iter;
use std::path::{Component, Path, PathBuf};
#[cfg(windows)]
use std::thread;
#[cfg(windows)]
use std::time::Instant;
use std::time::{Duration, SystemTime};

use serde_json::{Map, Value, json};
use sha2::{Digest, Sha256};

use crate::runtime::{BUILD_INFO, RuntimeFiles, WINDOWS_PLATFORM, pretty_json, sha256_hex};

/// The most characters a padding folder of a long root takes, well under the 255 that NTFS and
/// ext4 allow a name.
const MAX_PADDING_CHARS: usize = 64;

/// What the padding folders between a long root's base and the root itself are named with.
const PADDING_CHAR: char = 'p';

/// What a long root's own folder is named with. It differs from the padding, so a root made from
/// a base never lies inside another root made from it.
const ROOT_CHAR: char = 'r';

/// The names the alpha installer gives folders: the transaction folders of its game updates and of
/// its own. Every other name it leaves is a file.
const ALPHA_FOLDERS: [&str; 2] = [".gdl-update", ".gdl-installer-update"];

/// Characters that cmd changes in a command line even between quotes: `%` and `!` begin variables,
/// and `"` ends the quoting.
#[cfg(windows)]
const CHANGED_BY_CMD: [u8; 3] = *b"%!\"";

/// The mode of a denied entry: no reading, writing or searching for anyone but root.
#[cfg(unix)]
const NO_ACCESS: u32 = 0o000;

/// The mode of a folder whose names alone may be read: no searching, so that nothing it names can
/// be looked at or opened through it, and no writing, for anyone but root.
#[cfg(unix)]
const LISTING_ONLY: u32 = 0o400;

/// The name of the file that [`holds_bind`] makes, denies and removes again.
#[cfg(unix)]
const HOLD_PROBE: &str = "hold-probe";

/// The bits of a mode that hold its permissions, the set-id and sticky bits included.
#[cfg(unix)]
const PERMISSION_BITS: u32 = 0o7777;

/// The mode of a folder that takes no new entries: its names can be listed and what it holds
/// looked at and read, but nothing can be made, removed or renamed in it, by anyone but root.
#[cfg(unix)]
const NO_NEW_ENTRIES: u32 = 0o555;

/// The name under which [`deny_writes`] tries a new file in the folder it has denied, to show
/// whether the denial holds for this process.
const WRITE_PROBE: &str = "write-probe";

/// The variable that names the folder Windows is installed in, whose programs lie below it.
#[cfg(windows)]
const SYSTEM_ROOT: &str = "SystemRoot";

/// Where the program that changes a folder's permissions lies, below [`SYSTEM_ROOT`].
#[cfg(windows)]
const ICACLS: [&str; 2] = ["System32", "icacls.exe"];

/// Everyone, by the security identifier that every account's token holds, an administrator's
/// full token included, so that an entry denying it binds them all.
#[cfg(windows)]
const EVERYONE: &str = "*S-1-1-0";

/// What a deny entry takes from a folder: adding a file to it (WD) and adding a folder (AD), and
/// nothing else, so that listing it, which also asks to synchronise with it, and reading what it
/// holds still work.
#[cfg(windows)]
const ADD_ENTRIES: &str = "(WD,AD)";

/// The creation flag that runs a console program without a window of its own.
#[cfg(windows)]
const CREATE_NO_WINDOW: u32 = 0x0800_0000;

/// How long after the Unix epoch the time lies that [`backdate`] gives an entry: September 2001,
/// long before any test runs, and a time every file system the launcher meets can hold.
const LONG_AGO: Duration = Duration::from_secs(1_000_000_000);

/// The right to change an entry's times, all that [`backdate`] opens one with on Windows.
#[cfg(windows)]
const FILE_WRITE_ATTRIBUTES: u32 = 0x0100;

/// The flag without which Windows opens no folder as a handle.
#[cfg(windows)]
const FILE_FLAG_BACKUP_SEMANTICS: u32 = 0x0200_0000;

/// The share mode of a handle that lets nothing else open what it holds open.
#[cfg(windows)]
const NO_SHARING: u32 = 0;

/// What Windows refuses an open with while another handle on the entry shares nothing.
#[cfg(windows)]
const ERROR_SHARING_VIOLATION: i32 = 32;

/// What Windows refuses a symbolic link with when the account lacks the privilege to make one, as
/// one that is neither elevated nor in developer mode does.
const ERROR_PRIVILEGE_NOT_HELD: i32 = 1314;

/// What Windows may refuse an open with, besides a sharing violation, while a scanner looks at an
/// entry just made.
#[cfg(windows)]
const ERROR_ACCESS_DENIED: i32 = 5;

/// How long [`hold_closed`] waits out a scanner that looks at an entry just made: as long as the
/// launcher waits one out.
#[cfg(windows)]
const SCANNER_WINDOW: Duration = Duration::from_secs(8);

/// How long [`hold_closed`] waits between two tries while a scanner looks at an entry.
#[cfg(windows)]
const SCANNER_PAUSE: Duration = Duration::from_millis(200);

/// The folder at an installation's top that holds the launcher's own state.
const STATE_DIR: &str = ".gdl-launcher";

/// The installation's receipt, in the state folder.
const RECEIPT_FILE: &str = "receipt.json";

/// The memory card's icon on the disc, which the launcher sets into the Windows executable.
const DISC_ICON: &str = "carddemo/icon.tpl";

/// What an installation fixture holds as the icon: no real one.
const ICON_BYTES: &[u8] = b"not a real icon";

/// What an installation fixture holds of the disc, by name under its top: one of the game's data
/// files, which is no real one either, and the icon.
const DISC_FILES: [(&str, &[u8]); 2] = [
    ("Gauntlet/PDATA/WAR.WAD", b"not a real class file"),
    (DISC_ICON, ICON_BYTES),
];

/// The folders an installation keeps for the player's own files, which a new one has empty.
const PLAYER_FOLDERS: [&str; 2] = ["saves", "config"];

/// The launcher copy a Windows installation keeps at its top.
const WINDOWS_LAUNCHER: &str = "GauntletDarkLegacy-Launcher.exe";

/// The launcher copy an installation keeps at its top on every other platform.
const LAUNCHER: &str = "GauntletDarkLegacy-Launcher";

/// What an installation fixture's launcher copy holds: no real program.
const LAUNCHER_BYTES: &[u8] = b"not a real launcher";

/// What an installation fixture adds to the Windows executable in place of the disc's icon, so
/// that the executable it installs differs from the one the runtime ships, as the launcher's does.
const ICON_MARK: &[u8] = b" with the disc's icon";

/// The schema of the receipt the first launcher writes.
const RECEIPT_SCHEMA: u64 = 1;

/// The lowest launcher version a receipt of the first schema lets change it.
const MIN_LAUNCHER: &str = "1.0.0-beta";

/// The disc an installation's game data comes from.
const DISC: &str = "GUNE5D";

/// The revision the disc's header gives.
const DISC_REVISION: u8 = 1;

/// Makes a new, empty folder under `base` whose absolute path is exactly `utf16_units` UTF-16
/// units long, the measure the launcher holds an install root to, and returns that path.
///
/// Padding folders, each name far shorter than file systems allow, make up the length. `base` and
/// the padding are made when missing; the root itself must not exist yet. A base whose absolute
/// path keeps a verbatim prefix or a `..` is refused, because its length would then depend on how
/// it is resolved.
pub fn long_root(base: &Path, utf16_units: usize) -> io::Result<PathBuf> {
    let base = plain_absolute(base)?;
    let separator = usize::from(!ends_in_separator(&base));
    let lengths = utf16_units
        .checked_sub(utf16_len(&base)?)
        .and_then(|left| padding_lengths(left, separator))
        .unwrap_or_default();
    let Some((&own, padding)) = lengths.split_last() else {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "no folder of {utf16_units} UTF-16 units fits under {}",
                base.display()
            ),
        ));
    };
    let mut parent = base;
    for &chars in padding {
        parent.push(iter::repeat_n(PADDING_CHAR, chars).collect::<String>());
    }
    let root = parent.join(iter::repeat_n(ROOT_CHAR, own).collect::<String>());
    let measured = utf16_len(&std::path::absolute(&root)?)?;
    if measured != utf16_units {
        return Err(io::Error::other(format!(
            "{} measures {measured} UTF-16 units, not {utf16_units}",
            root.display()
        )));
    }
    fs::create_dir_all(&parent)?;
    fs::create_dir(&root)?;
    Ok(root)
}

/// The lengths of folder names that use up exactly `left` units of a path, each name costing a
/// separator before it, the first only when `separator` is 1. `None` when no names can.
fn padding_lengths(mut left: usize, mut separator: usize) -> Option<Vec<usize>> {
    let mut lengths = Vec::new();
    while left > 0 {
        let room = left.checked_sub(separator).filter(|room| *room > 0)?;
        let chars = match room.checked_sub(MAX_PADDING_CHARS) {
            None | Some(0) => room,
            // A single unit left over could not hold another separator and character.
            Some(1) => MAX_PADDING_CHARS - 1,
            Some(_) => MAX_PADDING_CHARS,
        };
        lengths.push(chars);
        left -= separator + chars;
        separator = 1;
    }
    Some(lengths)
}

/// `path` made absolute by the rules std applies to it, refused when that keeps a verbatim prefix
/// or a `..`.
fn plain_absolute(path: &Path) -> io::Result<PathBuf> {
    let absolute = std::path::absolute(path)?;
    let plain = absolute.components().all(|component| match component {
        Component::Prefix(prefix) => !prefix.kind().is_verbatim(),
        other => other != Component::ParentDir,
    });
    if plain {
        Ok(absolute)
    } else {
        Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!(
                "{} measures differently once it is resolved",
                absolute.display()
            ),
        ))
    }
}

/// Whether `path` ends in a separator, as a drive's root does, so that a name joins it without
/// another.
fn ends_in_separator(path: &Path) -> bool {
    path.as_os_str()
        .as_encoded_bytes()
        .last()
        .is_some_and(|&byte| std::path::is_separator(char::from(byte)))
}

/// A path's length in UTF-16 units, the units of Windows' own path limits.
#[cfg(windows)]
fn utf16_len(path: &Path) -> io::Result<usize> {
    use std::os::windows::ffi::OsStrExt;
    Ok(path.as_os_str().encode_wide().count())
}

/// A path's length in UTF-16 units, the units of Windows' own path limits. A path that is not
/// UTF-8 has no such length here and is refused.
#[cfg(not(windows))]
fn utf16_len(path: &Path) -> io::Result<usize> {
    path.to_str()
        .map(|text| text.encode_utf16().count())
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("{} is not UTF-8", path.display()),
            )
        })
}

/// Makes `link` a junction to the folder `target` through `mklink /J`, which, unlike a symbolic
/// link, needs no privilege. Both paths reach cmd absolute and quoted; a path holding a character
/// that cmd would change even so is refused before anything runs.
#[cfg(windows)]
pub fn make_junction(link: &Path, target: &Path) -> io::Result<()> {
    use std::ffi::OsString;
    use std::os::windows::process::CommandExt;
    use std::process::Command;

    let link = std::path::absolute(link)?;
    let target = std::path::absolute(target)?;
    for path in [&link, &target] {
        let bytes = path.as_os_str().as_encoded_bytes();
        if bytes.iter().any(|byte| CHANGED_BY_CMD.contains(byte)) {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                format!("cmd would change {}", path.display()),
            ));
        }
    }
    let quoted = |path: &Path| {
        let mut text = OsString::from("\"");
        text.push(path.as_os_str());
        text.push("\"");
        text
    };
    let shell = std::env::var_os("ComSpec").unwrap_or_else(|| OsString::from("cmd.exe"));
    let output = Command::new(shell)
        .args(["/d", "/c", "mklink", "/J"])
        .raw_arg(quoted(&link))
        .raw_arg(quoted(&target))
        .output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "mklink /J {} {} failed ({}): {}",
            link.display(),
            target.display(),
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        )));
    }
    if !fs::symlink_metadata(&link)?.file_type().is_symlink() {
        return Err(io::Error::other(format!(
            "mklink /J made no junction at {}",
            link.display()
        )));
    }
    Ok(())
}

/// Makes `link` a link to the folder `target` the way each system lets any account make one: a
/// junction on Windows, as [`make_junction`] makes it, and a symbolic link elsewhere.
pub fn link_folder(link: &Path, target: &Path) -> io::Result<()> {
    #[cfg(windows)]
    return make_junction(link, target);
    #[cfg(unix)]
    return std::os::unix::fs::symlink(target, link);
}

/// Makes `link` a symbolic link to the file `target`, which need not exist. Windows makes one only
/// for an account allowed to, as in developer mode, and refuses it otherwise, so a test that needs
/// one makes its first through [`symlink_file_where_allowed`] and returns early there.
pub fn symlink_file(link: &Path, target: &Path) -> io::Result<()> {
    #[cfg(windows)]
    return std::os::windows::fs::symlink_file(target, link);
    #[cfg(unix)]
    return std::os::unix::fs::symlink(target, link);
}

/// Makes `link` a symbolic link to the file `target`, as [`symlink_file`] makes one, and says
/// whether it was made: `false` only where Windows withholds from this account the privilege such
/// a link needs, the one reason a test may return early for. Any other failure, and any failure
/// on Unix, where every account may make one, is the test's own and panics, so that no case is
/// passed over unseen.
pub fn symlink_file_where_allowed(link: &Path, target: &Path) -> bool {
    made_where_allowed(symlink_file(link, target), link)
}

/// Makes `link` a symbolic link to the folder `target`, on Windows a symbolic link and not the
/// junction [`link_folder`] makes, and says whether it was made, as
/// [`symlink_file_where_allowed`] does.
pub fn symlink_folder_where_allowed(link: &Path, target: &Path) -> bool {
    #[cfg(windows)]
    let made = std::os::windows::fs::symlink_dir(target, link);
    #[cfg(unix)]
    let made = std::os::unix::fs::symlink(target, link);
    made_where_allowed(made, link)
}

/// Whether `made`, the making of the symbolic link `link`, made it: `false` when the privilege
/// was withheld, and a panic for any other failure.
fn made_where_allowed(made: io::Result<()>, link: &Path) -> bool {
    match made {
        Ok(()) => true,
        Err(error) if withholds_links(&error) => false,
        Err(error) => panic!("making the symbolic link {}: {error}", link.display()),
    }
}

/// Whether `error` is Windows' refusal of a symbolic link to an account that lacks the privilege
/// to make one. No other refusal is, and none on Unix.
fn withholds_links(error: &io::Error) -> bool {
    cfg!(windows) && error.raw_os_error() == Some(ERROR_PRIVILEGE_NOT_HELD)
}

/// The names in `folder`, sorted, as a test compares what a folder holds. A name that is not
/// Unicode is refused.
pub fn folder_names(folder: &Path) -> io::Result<Vec<String>> {
    let mut names = Vec::new();
    for entry in fs::read_dir(folder)? {
        let name = entry?.file_name().into_string().map_err(|name| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("{name:?} is not Unicode"),
            )
        })?;
        names.push(name);
    }
    names.sort();
    Ok(names)
}

/// A file of a tree as the shared vectors list one: its path, with `/` between its components, its
/// size and the SHA-256 digest of its bytes in lowercase hexadecimal.
pub type Listed = (String, u64, String);

/// A tree as the shared vectors list it: each file's path, size and digest. Panics when `rows` is
/// no such list.
pub fn listed(rows: &Value) -> Vec<Listed> {
    let rows = rows.as_array().expect("a tree lists its files");
    let text = |value: &Value| value.as_str().expect("a string").to_owned();
    rows.iter()
        .map(|row| {
            let size = row[1].as_u64().expect("a size");
            (text(&row[0]), size, text(&row[2]))
        })
        .collect()
}

/// Every file of the tree at `root`, as the shared vectors list a tree: sorted by the path's text,
/// which orders as its UTF-8 bytes do. Panics on what is neither a folder nor a regular file, such
/// as a link, which is never followed, and on a folder that holds nothing, since an extraction
/// makes no folder but those above its files; and on a tree that cannot be read, or a name in it
/// that is not Unicode.
#[track_caller]
pub fn tree_listing(root: &Path) -> Vec<Listed> {
    let mut files = Vec::new();
    let mut folders = vec![(root.to_path_buf(), String::new())];
    while let Some((folder, above)) = folders.pop() {
        let mut empty = true;
        for entry in fs::read_dir(&folder).unwrap() {
            empty = false;
            let entry = entry.unwrap();
            let name = entry.file_name().into_string().unwrap();
            let path = format!("{above}{name}");
            let kind = entry.file_type().unwrap();
            if kind.is_dir() {
                folders.push((entry.path(), format!("{path}/")));
            } else {
                assert!(kind.is_file(), "{path} is no file");
                let bytes = fs::read(entry.path()).unwrap();
                files.push((path, bytes.len() as u64, sha256_hex(&bytes)));
            }
        }
        assert!(!empty, "the folder {} holds nothing", folder.display());
    }
    files.sort();
    files
}

/// What [`snapshot`] keeps of an entry: enough to tell that it was changed, replaced or made anew.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EntryState {
    /// What kind of entry it is, a link counting as a link and not as what it leads to.
    pub file_type: fs::FileType,
    /// Its length in bytes, as the system gives it.
    pub len: u64,
    /// When it was last changed; for a folder, when an entry was last made in it or removed.
    pub modified: SystemTime,
    /// The SHA-256 digest of what it holds, when it is a regular file: other bytes show even where
    /// they keep the length and fall within the clock's tick.
    pub digest: Option<[u8; 32]>,
}

/// Every entry of the tree at `root`, by its path under `root`, `root` itself as the empty path:
/// what a test compares before and after an operation that must change nothing. A link is
/// recorded and never followed, and every regular file is read for its digest.
///
/// Bytes written again as they were show only by the time of change, which a write soon after
/// the tree was made may share with it; a test that must see those too takes its first snapshot
/// after a [`backdate`].
pub fn snapshot(root: &Path) -> io::Result<BTreeMap<PathBuf, EntryState>> {
    snapshot_trees(root, vec![PathBuf::new()])
}

/// What [`snapshot`] gives of the tree at `root`, without `root`'s own entry and without every
/// entry under a top-level name that begins with `skipped_prefix`: what stays the same of a folder
/// that a job makes its staging in, whose own time of change moves as the staging comes and goes.
///
/// A name is left out only at the top, and only when it begins with the prefix exactly, letter
/// case included. What is left out is never walked or read, so a staging that is still there
/// changes nothing here, whatever it holds. A `root` that is not itself a folder, such as a link
/// to one, and an empty prefix are refused as [`io::ErrorKind::InvalidInput`]: either would leave
/// nothing to compare, and so a comparison that could never fail.
pub fn snapshot_outside(
    root: &Path,
    skipped_prefix: &str,
) -> io::Result<BTreeMap<PathBuf, EntryState>> {
    if skipped_prefix.is_empty() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "an empty prefix leaves out every name",
        ));
    }
    if !fs::symlink_metadata(root)?.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{} is not a folder", root.display()),
        ));
    }
    // On every system a name's encoded bytes begin with a text prefix's exactly when the name
    // begins with that prefix.
    let skipped = skipped_prefix.as_bytes();
    let mut kept = Vec::new();
    for entry in fs::read_dir(root)? {
        let name = entry?.file_name();
        if !name.as_encoded_bytes().starts_with(skipped) {
            kept.push(PathBuf::from(name));
        }
    }
    snapshot_trees(root, kept)
}

/// Every entry of the trees whose tops lie at `tops`, paths under `root`, keyed by its own path
/// under `root` and recorded as [`snapshot`] records one: a link never followed, and every regular
/// file read for its digest.
fn snapshot_trees(root: &Path, tops: Vec<PathBuf>) -> io::Result<BTreeMap<PathBuf, EntryState>> {
    let mut entries = BTreeMap::new();
    let mut unvisited = tops;
    while let Some(relative) = unvisited.pop() {
        // Not joined for the root itself, which a join would give a trailing separator.
        let path = if relative.as_os_str().is_empty() {
            root.to_path_buf()
        } else {
            root.join(&relative)
        };
        let metadata = fs::symlink_metadata(&path)?;
        let file_type = metadata.file_type();
        if file_type.is_dir() {
            for entry in fs::read_dir(&path)? {
                unvisited.push(relative.join(entry?.file_name()));
            }
        }
        let digest = if file_type.is_file() {
            Some(Sha256::digest(fs::read(&path)?).into())
        } else {
            None
        };
        let state = EntryState {
            file_type,
            len: metadata.len(),
            modified: metadata.modified()?,
            digest,
        };
        entries.insert(relative, state);
    }
    Ok(entries)
}

/// Gives every folder and regular file of the tree at `root`, `root` itself included, a time of
/// last change long before the test, so that a [`snapshot`] taken after it shows anything written
/// later: bytes written again as they were, and an entry made and removed again in a folder. A
/// link is passed over, and what it leads to is left as it was.
pub fn backdate(root: &Path) -> io::Result<()> {
    let long_ago = SystemTime::UNIX_EPOCH + LONG_AGO;
    let mut unvisited = vec![root.to_path_buf()];
    while let Some(path) = unvisited.pop() {
        let file_type = fs::symlink_metadata(&path)?.file_type();
        if file_type.is_dir() {
            for entry in fs::read_dir(&path)? {
                unvisited.push(entry?.path());
            }
        } else if !file_type.is_file() {
            // A link, or what no test makes: a FIFO, a socket or a device.
            continue;
        }
        open_to_set_times(&path, file_type.is_dir())?.set_modified(long_ago)?;
    }
    Ok(())
}

/// Opens the folder or file at `path`, which `folder` says it is, to change its times and nothing
/// else.
#[cfg(windows)]
fn open_to_set_times(path: &Path, folder: bool) -> io::Result<fs::File> {
    use std::os::windows::fs::OpenOptionsExt;

    let mut options = fs::OpenOptions::new();
    options.access_mode(FILE_WRITE_ATTRIBUTES);
    if folder {
        options.custom_flags(FILE_FLAG_BACKUP_SEMANTICS);
    }
    options.open(path)
}

/// Opens the folder or file at `path` to change its times, which its owner may do through any
/// open of it.
#[cfg(unix)]
fn open_to_set_times(path: &Path, _: bool) -> io::Result<fs::File> {
    fs::File::open(path)
}

/// Leaves `names` in `root` the way the alpha installer leaves them: its transaction folders, in
/// any letter case, as empty folders, and every other name as an empty file. Each must be a single
/// name, or nothing is made, and one that exists already is refused.
pub fn make_alpha_markers(root: &Path, names: &[&str]) -> io::Result<()> {
    if let Some(name) = names
        .iter()
        .find(|name| Path::new(name).file_name() != Some(name.as_ref()))
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{name:?} is not a single name"),
        ));
    }
    for name in names {
        let path = root.join(name);
        if ALPHA_FOLDERS
            .iter()
            .any(|folder| folder.eq_ignore_ascii_case(name))
        {
            fs::create_dir(&path)?;
        } else {
            fs::File::create_new(&path)?;
        }
    }
    Ok(())
}

/// Entries whose permissions a test has taken away, every one of them or all but the listing of
/// a folder's names, so that opening them, or looking at what a folder holds, fails for anyone but
/// root. Dropping it gives each its mode back, which a `TempDir` needs before it can remove them;
/// made after the `TempDir`, it is dropped before it.
#[cfg(unix)]
#[derive(Debug)]
#[must_use = "dropping it gives the entries their modes back at once"]
pub struct DeniedAccess {
    /// Each denied entry and its mode before, in the order they were denied.
    modes: Vec<(PathBuf, u32)>,
}

#[cfg(unix)]
impl DeniedAccess {
    /// Sets each of `paths` to mode 0o000, in order. An entry inside a folder must come before
    /// that folder, which cannot be searched once denied. When one cannot be changed, those
    /// already denied get their modes back before the error returns.
    pub fn deny(paths: &[&Path]) -> io::Result<DeniedAccess> {
        DeniedAccess::set(paths, NO_ACCESS)
    }

    /// Sets each of `folders` to mode 0o400, in order, as [`deny`](DeniedAccess::deny) sets
    /// its entries: the names in each can still be listed, but nothing they name can be looked
    /// at, opened or changed through it, so that code which still finds what it looks for there
    /// found it by the names alone.
    pub fn listing_only(folders: &[&Path]) -> io::Result<DeniedAccess> {
        DeniedAccess::set(folders, LISTING_ONLY)
    }

    /// Sets each of `paths` to `mode`, in order, keeping the mode it had for the drop.
    fn set(paths: &[&Path], mode: u32) -> io::Result<DeniedAccess> {
        use std::os::unix::fs::PermissionsExt;

        let mut denied = DeniedAccess {
            modes: Vec::with_capacity(paths.len()),
        };
        for path in paths {
            let before = fs::metadata(path)?.permissions().mode() & PERMISSION_BITS;
            fs::set_permissions(path, fs::Permissions::from_mode(mode))?;
            denied.modes.push((path.to_path_buf(), before));
        }
        Ok(denied)
    }
}

/// Whether this process is kept from what [`hold_closed`] holds, which is asked apart from any
/// hold, so that a hold which kept nothing out cannot pass for one that root passes. On Windows
/// it always is, since a hold there is checked by an open as it is made.
#[cfg(windows)]
pub fn holds_bind(_: &Path) -> io::Result<bool> {
    Ok(true)
}

/// Whether this process is kept from what [`hold_closed`] holds, which is asked apart from any
/// hold, so that a hold which kept nothing out cannot pass for one that root passes. On Unix it
/// is unless the process opens a file of mode 0o000 regardless, as root does, which a file made
/// for the question in `folder`, and removed again, shows.
#[cfg(unix)]
pub fn holds_bind(folder: &Path) -> io::Result<bool> {
    use std::os::unix::fs::PermissionsExt;

    let probe = folder.join(HOLD_PROBE);
    fs::File::create_new(&probe)?;
    fs::set_permissions(&probe, fs::Permissions::from_mode(NO_ACCESS))?;
    let opened = fs::File::open(&probe);
    fs::remove_file(&probe)?;
    match opened {
        Ok(_) => Ok(false),
        Err(error) if error.kind() == io::ErrorKind::PermissionDenied => Ok(true),
        Err(error) => Err(error),
    }
}

#[cfg(unix)]
impl Drop for DeniedAccess {
    fn drop(&mut self) {
        use std::os::unix::fs::PermissionsExt;

        // Last denied first, so a folder can be searched again before what lies inside it.
        for (path, mode) in self.modes.iter().rev() {
            // A drop cannot report a failure; removing the test's folder reports it instead.
            let _ = fs::set_permissions(path, fs::Permissions::from_mode(*mode));
        }
    }
}

/// Entries that [`hold_closed`] keeps every open from, until this is dropped.
#[derive(Debug)]
#[must_use = "dropping it lets the entries be opened again at once"]
pub struct HeldClosed {
    /// A handle on each entry that shares nothing.
    #[cfg(windows)]
    _handles: Vec<fs::File>,
    /// The entries' modes, taken away.
    #[cfg(unix)]
    _denied: DeniedAccess,
    /// Whether every open of an entry fails for this process too.
    proven: bool,
}

impl HeldClosed {
    /// Whether every open of a held entry fails for this process too, so that code which still
    /// found what it looked for while they were held opened none of them: always on Windows, and
    /// on Unix for every user but root, who opens entries of mode 0o000. A test holds it to
    /// [`holds_bind`], which asks the same of a file of its own, so that a hold which kept nothing
    /// out fails the test rather than passing for root's.
    pub fn proven(&self) -> bool {
        self.proven
    }
}

/// Keeps every open of each of `paths`, files and folders, from succeeding until the result is
/// dropped, so that a test can show that code which looks at them never opens one.
///
/// On Windows each is held open sharing nothing, a folder with the flag that a folder's handle
/// needs and a file without it: with it, a process whose backup privilege is enabled, as one
/// started from Git Bash may be, opens a file whatever another handle shares. A scanner that
/// still looks at a new entry is waited out. Each is then opened as such a process would open it,
/// and one that opens is an error. On Unix each loses every permission, which root passes.
#[cfg(windows)]
pub fn hold_closed(paths: &[&Path]) -> io::Result<HeldClosed> {
    use std::os::windows::fs::OpenOptionsExt;

    let mut handles = Vec::with_capacity(paths.len());
    for &path in paths {
        let mut holding = read_options(path)?;
        holding.share_mode(NO_SHARING);
        let handle = while_scanned(|| holding.open(path)).map_err(|error| {
            io::Error::new(error.kind(), format!("{}: {error}", path.display()))
        })?;
        handles.push(handle);
    }
    for &path in paths {
        match read_options(path)?.open(path) {
            Err(error) if error.raw_os_error() == Some(ERROR_SHARING_VIOLATION) => {}
            Err(error) => return Err(error),
            Ok(_) => {
                return Err(io::Error::other(format!(
                    "{} opened while it was held",
                    path.display()
                )));
            }
        }
    }
    Ok(HeldClosed {
        _handles: handles,
        proven: true,
    })
}

/// Keeps every open of each of `paths`, files and folders, from succeeding until the result is
/// dropped, so that a test can show that code which looks at them never opens one.
///
/// On Unix each loses every permission, as [`DeniedAccess`] takes them, which root passes; an
/// entry inside a folder must come before that folder. On Windows each is held open sharing
/// nothing.
#[cfg(unix)]
pub fn hold_closed(paths: &[&Path]) -> io::Result<HeldClosed> {
    let denied = DeniedAccess::deny(paths)?;
    let mut proven = true;
    for &path in paths {
        match fs::File::open(path) {
            Ok(_) => proven = false,
            Err(error) if error.kind() == io::ErrorKind::PermissionDenied => {}
            Err(error) => return Err(error),
        }
    }
    Ok(HeldClosed {
        _denied: denied,
        proven,
    })
}

/// How a test opens `path` to read: a folder with the flag without which Windows opens no folder
/// as a handle, and a file without it.
#[cfg(windows)]
fn read_options(path: &Path) -> io::Result<fs::OpenOptions> {
    use std::os::windows::fs::OpenOptionsExt;

    let mut options = fs::OpenOptions::new();
    options.read(true);
    if fs::symlink_metadata(path)?.is_dir() {
        options.custom_flags(FILE_FLAG_BACKUP_SEMANTICS);
    }
    Ok(options)
}

/// Runs `open` again while Windows refuses it as it refuses an open of an entry that a scanner
/// still looks at, for up to [`SCANNER_WINDOW`].
#[cfg(windows)]
fn while_scanned(mut open: impl FnMut() -> io::Result<fs::File>) -> io::Result<fs::File> {
    let deadline = Instant::now() + SCANNER_WINDOW;
    loop {
        match open() {
            Err(error)
                if matches!(
                    error.raw_os_error(),
                    Some(ERROR_ACCESS_DENIED | ERROR_SHARING_VIOLATION)
                ) && Instant::now() < deadline =>
            {
                thread::sleep(SCANNER_PAUSE);
            }
            result => return result,
        }
    }
}

/// A folder that [`deny_writes`] keeps from taking new entries, until this is dropped.
#[derive(Debug)]
#[must_use = "dropping it lets the folder take new entries again at once"]
pub struct WritesDenied {
    /// The folder, by its absolute path.
    folder: PathBuf,
    /// The program that gives the folder its permissions back.
    #[cfg(windows)]
    icacls: PathBuf,
    /// The folder's mode before, which the drop gives back.
    #[cfg(unix)]
    mode: u32,
    /// Whether the folder refuses this process a new file too.
    proven: bool,
}

impl WritesDenied {
    /// Whether the folder refuses this process a new file too: always on Windows, where a denial
    /// that did not is an error, and on Unix for every user but root, who makes entries in a
    /// folder of any mode. A test that needs the refusal returns early, with its reason, when it
    /// is not proven.
    pub fn proven(&self) -> bool {
        self.proven
    }

    /// Denies everyone the rights to add a file and a folder to `folder`, on the folder alone, so
    /// that what it holds keeps its own. The guard is made first, so that a run of icacls that
    /// fails part way is undone too.
    #[cfg(windows)]
    fn deny(folder: PathBuf) -> io::Result<WritesDenied> {
        let denied = WritesDenied {
            folder,
            icacls: icacls_program()?,
            proven: false,
        };
        let rule = format!("{EVERYONE}:{ADD_ENTRIES}");
        let args = [
            denied.folder.as_os_str(),
            OsStr::new("/deny"),
            OsStr::new(&rule),
        ];
        run_icacls(&denied.icacls, &args)?;
        Ok(denied)
    }

    /// Sets `folder` to mode 0o555, keeping the mode it had for the drop.
    #[cfg(unix)]
    fn deny(folder: PathBuf) -> io::Result<WritesDenied> {
        use std::os::unix::fs::PermissionsExt;

        let mode = fs::symlink_metadata(&folder)?.permissions().mode() & PERMISSION_BITS;
        fs::set_permissions(&folder, fs::Permissions::from_mode(NO_NEW_ENTRIES))?;
        Ok(WritesDenied {
            folder,
            mode,
            proven: false,
        })
    }
}

impl Drop for WritesDenied {
    fn drop(&mut self) {
        // A drop cannot report a failure; a test that writes into the folder again finds it.
        #[cfg(windows)]
        {
            let args = [
                self.folder.as_os_str(),
                OsStr::new("/remove:d"),
                OsStr::new(EVERYONE),
            ];
            let _ = run_icacls(&self.icacls, &args);
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;

            let _ = fs::set_permissions(&self.folder, fs::Permissions::from_mode(self.mode));
        }
    }
}

/// Keeps `folder` from taking new entries until the result is dropped: a file made in it is
/// refused, and so is a folder but for the case below, while its names can still be listed and it
/// and what it holds looked at and read, so that a test can show what code does with a folder it
/// may not write into.
///
/// On Windows an entry that denies everyone the rights to add a file and a folder is set on the
/// folder alone, through the icacls of the system's own folder, and the drop removes every entry
/// that denies everyone anything, of which a folder a test made holds none of its own. A process
/// whose restore privilege is enabled, as one started from Git Bash may be, still makes a folder
/// in it: the system makes a folder with backup intent, which that privilege lets past the entry,
/// while a file made as std and tempfile make one asks for none. On Unix the folder's mode
/// becomes 0o555, which root passes, and the drop gives its mode back; there, removing or
/// renaming what the folder holds is refused too, which on Windows it is not.
///
/// A new file is then tried in it under a name that it must not hold, and removed again when
/// made: one made is an error on Windows, and on Unix [`proven`](WritesDenied::proven) says
/// whether it was refused. Anything but a folder, a link to one included, is refused before
/// anything changes. Made after the `TempDir` that holds the folder, it is dropped before it.
pub fn deny_writes(folder: &Path) -> io::Result<WritesDenied> {
    let folder = std::path::absolute(folder)?;
    if !fs::symlink_metadata(&folder)?.is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("{} is not a folder", folder.display()),
        ));
    }
    let probe = folder.join(WRITE_PROBE);
    match fs::symlink_metadata(&probe) {
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
        Ok(_) => {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                format!("{} is there already", probe.display()),
            ));
        }
    }
    let mut denied = WritesDenied::deny(folder)?;
    denied.proven = refuses_new_file(&probe)?;
    if cfg!(windows) && !denied.proven {
        return Err(io::Error::other(format!(
            "{} took a new file while it was denied",
            denied.folder.display()
        )));
    }
    Ok(denied)
}

/// Whether this process is refused a new file at `probe` for want of permission. A file that was
/// made is removed again, and any other failure is returned.
fn refuses_new_file(probe: &Path) -> io::Result<bool> {
    match fs::File::create_new(probe) {
        Ok(file) => {
            drop(file);
            fs::remove_file(probe).map(|()| false)
        }
        Err(error) if error.kind() == io::ErrorKind::PermissionDenied => Ok(true),
        Err(error) => Err(error),
    }
}

/// The icacls of the system's own folder, by its absolute path, so that no program of that name
/// elsewhere on the search path runs in its place.
#[cfg(windows)]
fn icacls_program() -> io::Result<PathBuf> {
    let root = std::env::var_os(SYSTEM_ROOT)
        .map(PathBuf::from)
        .filter(|root| root.is_absolute())
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("{SYSTEM_ROOT} names no absolute folder"),
            )
        })?;
    Ok(ICACLS.iter().fold(root, |path, part| path.join(part)))
}

/// Runs `icacls` with `args`, without a window, and gives what it wrote out. A run that fails is
/// an error that names what it said.
#[cfg(windows)]
fn run_icacls(icacls: &Path, args: &[&OsStr]) -> io::Result<Vec<u8>> {
    use std::os::windows::process::CommandExt;
    use std::process::Command;

    let output = Command::new(icacls)
        .args(args)
        .creation_flags(CREATE_NO_WINDOW)
        .output()?;
    if output.status.success() {
        return Ok(output.stdout);
    }
    let shown: Vec<String> = args
        .iter()
        .map(|arg| arg.to_string_lossy().into_owned())
        .collect();
    let said = [output.stderr.as_slice(), output.stdout.as_slice()].concat();
    Err(io::Error::other(format!(
        "{} {} failed ({}): {}",
        icacls.display(),
        shown.join(" "),
        output.status,
        String::from_utf8_lossy(&said).trim()
    )))
}

/// An installation as the launcher leaves one, for the tests that read or change one: a runtime's
/// files, the disc's, the player's folders, the launcher copy and a receipt that describes them.
#[derive(Clone, Debug)]
pub struct InstallFixture {
    /// The installation's folder.
    pub root: PathBuf,
    /// The runtime installed in it, as the release shipped it.
    pub runtime: RuntimeFiles,
}

impl InstallFixture {
    /// Lays out an installation of `runtime` in `root`, which is made when missing: the runtime's
    /// files and its `build-info.json`, the Windows executable carrying the disc's icon as the
    /// launcher sets it in; `Gauntlet/PDATA/WAR.WAD` and `carddemo/icon.tpl` from the disc; empty
    /// `saves` and `config` folders; the launcher copy, `GauntletDarkLegacy-Launcher.exe` on
    /// windows-x64 and `GauntletDarkLegacy-Launcher` elsewhere; and `.gdl-launcher/receipt.json`
    /// holding [`receipt`](Self::receipt). Every file and folder is made new, so a name that
    /// exists already is refused. A Linux installation holds both the executable `gauntlet` and
    /// the disc's `Gauntlet`, which a file system that ignores letter case, as Windows' does, takes
    /// for one name, so there it is refused the same way.
    pub fn write(root: &Path, runtime: RuntimeFiles) -> io::Result<Self> {
        let fixture = InstallFixture {
            root: root.to_path_buf(),
            runtime,
        };
        fs::create_dir_all(root)?;
        for (name, data) in fixture.installed_files() {
            write_new(&path_under(root, name), &data)?;
        }
        write_new(&root.join(BUILD_INFO), &fixture.runtime.build_info_bytes())?;
        for (name, data) in DISC_FILES {
            write_new(&path_under(root, name), data)?;
        }
        for folder in PLAYER_FOLDERS {
            fs::create_dir(root.join(folder))?;
        }
        write_new(&root.join(fixture.launcher_name()), LAUNCHER_BYTES)?;
        fs::create_dir(root.join(STATE_DIR))?;
        write_new(&fixture.receipt_path(), &pretty_json(&fixture.receipt()))?;
        Ok(fixture)
    }

    /// The receipt the launcher writes for this installation, from [`runtime`](Self::runtime) and
    /// what the fixture lays out beside it: the stamp of the first schema; the release's version,
    /// commit and platform; the disc and its revision; the SHA-256 digest of `build-info.json`;
    /// the installed digest of each file installed with other bytes than the runtime's, which is
    /// the Windows executable alone; on windows-x64 where its icon came from and the digest the
    /// runtime gives the executable; and the launcher copy's name and digest.
    pub fn receipt(&self) -> Value {
        let installed_digests: Map<String, Value> = self
            .installed_files()
            .into_iter()
            .filter(|(name, _)| self.personalises(name))
            .map(|(name, data)| (name.to_owned(), Value::String(sha256_hex(&data))))
            .collect();
        let mut receipt = json!({
            "schema": RECEIPT_SCHEMA,
            "minLauncher": MIN_LAUNCHER,
            "version": self.runtime.version,
            "commit": self.runtime.commit,
            "platform": self.runtime.platform,
            "disc": DISC,
            "discRevision": DISC_REVISION,
            "buildInfoSha256": sha256_hex(&self.runtime.build_info_bytes()),
            "installedDigests": installed_digests,
            "launcher": {"name": self.launcher_name(), "sha256": sha256_hex(LAUNCHER_BYTES)},
        });
        if self.is_windows() {
            // The digest the release record lists, whatever the runtime holds: nothing when it
            // lists none.
            let release = self.runtime.build_info()["files"][&self.runtime.executable].clone();
            receipt["discIcon"] = json!({
                "path": DISC_ICON,
                "sha256": sha256_hex(ICON_BYTES),
                "releaseExecutableSha256": release,
            });
        }
        receipt
    }

    /// Writes `receipt` in place of the installation's receipt, as the launcher writes its JSON:
    /// indented by two spaces, with LF line ends and a line end after it.
    pub fn write_receipt(&self, receipt: &Value) -> io::Result<()> {
        fs::write(self.receipt_path(), pretty_json(receipt))
    }

    /// The runtime's files as the installation holds them, by name: the Windows executable with
    /// [`ICON_MARK`] after its bytes, every other file as the runtime ships it.
    fn installed_files(&self) -> Vec<(&str, Vec<u8>)> {
        self.runtime
            .files
            .iter()
            .map(|(name, data)| {
                let data = if self.personalises(name) {
                    [data.as_slice(), ICON_MARK].concat()
                } else {
                    data.clone()
                };
                (name.as_str(), data)
            })
            .collect()
    }

    /// Whether the runtime's file `name` is installed carrying the disc's icon: the executable,
    /// on Windows.
    fn personalises(&self, name: &str) -> bool {
        self.is_windows() && name == self.runtime.executable
    }

    /// Whether the runtime was built for Windows.
    fn is_windows(&self) -> bool {
        self.runtime.platform == WINDOWS_PLATFORM
    }

    /// The name of the launcher copy at the installation's top.
    fn launcher_name(&self) -> &'static str {
        if self.is_windows() {
            WINDOWS_LAUNCHER
        } else {
            LAUNCHER
        }
    }

    /// Where the installation keeps its receipt.
    fn receipt_path(&self) -> PathBuf {
        self.root.join(STATE_DIR).join(RECEIPT_FILE)
    }
}

/// The path of `name`, a name of `/`-separated parts as a runtime and a disc give them, under
/// `root`.
fn path_under(root: &Path, name: &str) -> PathBuf {
    name.split('/')
        .fold(root.to_path_buf(), |path, part| path.join(part))
}

/// Makes the file `path` new, with the folders above it, and writes `data` in it.
fn write_new(path: &Path, data: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::File::create_new(path)?.write_all(data)
}

#[cfg(test)]
mod tests {
    use std::collections::BTreeSet;
    use std::fs;
    use std::io;
    use std::path::Path;

    use super::{MAX_PADDING_CHARS, long_root, make_alpha_markers};

    /// The names the alpha installer leaves in a folder it installed or updated.
    const ALPHA_NAMES: [&str; 7] = [
        "installation.json",
        ".gdl-update.lock",
        ".gdl-update",
        ".gdl-installer-update",
        ".gdl-install.lock",
        "GauntletDarkLegacy-Update",
        "GauntletDarkLegacy-Update.exe",
    ];

    /// Those of them that are folders: the transaction folders of its game updates and of its own.
    const ALPHA_FOLDER_NAMES: [&str; 2] = [".gdl-update", ".gdl-installer-update"];

    /// The prefix of a staging that an N64 extraction makes in the folder it writes into.
    const STAGING_PREFIX: &str = ".gdl-extract-n64-";

    /// Whether the file system that holds `dir` takes names that differ only in letter case for
    /// one name, as Windows' does.
    fn names_fold_case(dir: &Path) -> bool {
        let probe = dir.join("case-probe");
        fs::write(&probe, b"").unwrap();
        let folds = fs::symlink_metadata(dir.join("CASE-PROBE")).is_ok();
        fs::remove_file(&probe).unwrap();
        folds
    }

    /// A path's absolute length in UTF-16 units, as the launcher measures an install root.
    fn absolute_units(path: &Path) -> usize {
        let absolute = std::path::absolute(path).unwrap();
        #[cfg(windows)]
        let units = {
            use std::os::windows::ffi::OsStrExt;
            absolute.as_os_str().encode_wide().count()
        };
        #[cfg(not(windows))]
        let units = absolute.to_str().unwrap().encode_utf16().count();
        units
    }

    #[test]
    fn long_roots_measure_exactly() {
        let dir = tempfile::tempdir().unwrap();
        let base = std::path::absolute(dir.path()).unwrap();
        // A separator and one character.
        let shortest = absolute_units(&base) + 2;
        // Every length through three padding folders, then the launcher's 180-unit install root,
        // the lengths about MAX_PATH and one well past it.
        let mut targets: BTreeSet<usize> =
            (shortest..shortest + 3 * (MAX_PADDING_CHARS + 1) + 2).collect();
        targets.extend(
            [180, 259, 260, 261, 300]
                .into_iter()
                .filter(|target| *target >= shortest),
        );
        let mut roots = Vec::new();
        for &target in &targets {
            let root = long_root(&base, target).unwrap();
            assert_eq!(root, std::path::absolute(&root).unwrap());
            assert_eq!(absolute_units(&root), target, "{}", root.display());
            assert!(fs::symlink_metadata(&root).unwrap().is_dir(), "{target}");
            for component in root.strip_prefix(&base).unwrap() {
                let chars = component.len();
                assert!((1..=MAX_PADDING_CHARS).contains(&chars), "{target}");
            }
            roots.push(root);
        }
        // Roots of different lengths share no more than padding, so each is still empty.
        for root in &roots {
            let entries = fs::read_dir(root).unwrap().count();
            assert_eq!(entries, 0, "{}", root.display());
        }
        // A length made before names a root that exists, which is refused rather than shared.
        let error = long_root(&base, shortest).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        // No room for a separator and a character.
        for target in [0, shortest - 2, shortest - 1] {
            let error = long_root(&base, target).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{target}");
        }

        // A base that ends in a separator needs none before its first padding folder.
        let parent = base.join("separated");
        fs::create_dir(&parent).unwrap();
        let separated = parent.join("");
        let root = long_root(&separated, absolute_units(&separated) + 1).unwrap();
        assert_eq!(absolute_units(&root), absolute_units(&parent) + 2);
        assert_eq!(root.parent(), Some(parent.as_path()));
        let error = long_root(&separated, absolute_units(&separated)).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);

        // A missing base is made with the root.
        let missing = base.join("made").join("with spaces");
        let root = long_root(&missing, absolute_units(&missing) + 2).unwrap();
        assert!(missing.is_dir() && root.starts_with(&missing));

        // Lengths count UTF-16 units, whatever a path's bytes or characters: U+00E9 takes one
        // unit and U+1D11E two, so a separator and one character remain for the root.
        let unicode = base.join("\u{e9}\u{1d11e}");
        let root = long_root(&unicode, absolute_units(&base) + 6).unwrap();
        assert_eq!(absolute_units(&root), absolute_units(&base) + 6);
        assert_eq!(root.parent(), Some(unicode.as_path()));

        // A base whose length depends on how it is resolved is refused before anything is made.
        #[cfg(windows)]
        {
            let unmade = base.join("verbatim");
            let verbatim = format!(r"\\?\{}", unmade.display());
            let error = long_root(Path::new(&verbatim), 300).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            assert!(!unmade.exists());
        }
        #[cfg(unix)]
        {
            let dotted = base.join("up").join("..").join("down");
            let error = long_root(&dotted, absolute_units(&dotted) + 2).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
            assert!(!base.join("up").exists());
        }
    }

    #[cfg(windows)]
    #[test]
    fn junctions_read_as_links() {
        use std::os::windows::fs::FileTypeExt;

        use super::make_junction;

        let dir = tempfile::tempdir().unwrap();
        // Characters that cmd reads specially outside quotes must reach mklink as they are, in a
        // path with a space and in one without, which argument quoting alone would leave bare.
        for folder in ["a b & (c) ^d", "x&y^z(1)"] {
            let place = dir.path().join(folder);
            let target = place.join("target");
            fs::create_dir_all(&target).unwrap();
            fs::write(target.join("file.txt"), b"through the junction").unwrap();
            let link = place.join("link");
            make_junction(&link, &target).unwrap();

            let kind = fs::symlink_metadata(&link).unwrap().file_type();
            assert!(
                kind.is_symlink() && kind.is_symlink_dir() && !kind.is_dir(),
                "{folder}"
            );
            assert!(fs::metadata(&link).unwrap().is_dir(), "{folder}");
            let pointed = fs::read_link(&link).unwrap();
            assert_eq!(pointed, std::path::absolute(&target).unwrap());
            let read = fs::read(link.join("file.txt")).unwrap();
            assert_eq!(read, b"through the junction");

            // A link that exists already is refused, as mklink refuses it.
            assert!(make_junction(&link, &target).is_err(), "{folder}");
            // Removing the junction leaves what it pointed to.
            fs::remove_dir(&link).unwrap();
            assert!(target.join("file.txt").is_file(), "{folder}");
        }

        // Paths that cmd would change are refused before anything runs.
        let target = dir.path().join("x&y^z(1)").join("target");
        let other = dir.path().join("other");
        for name in ["100%", "%TEMP%", "loud!", "quote\"d"] {
            let changed = dir.path().join(name);
            let error = make_junction(&changed, &target).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name}");
            assert!(fs::symlink_metadata(&changed).is_err(), "{name}");
            let error = make_junction(&other, &changed).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name}");
        }
        assert!(fs::symlink_metadata(&other).is_err());
    }

    #[test]
    fn folder_links_read_as_links() {
        use super::link_folder;

        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("target");
        fs::create_dir(&target).unwrap();
        fs::write(target.join("file.txt"), b"through the link").unwrap();
        let link = dir.path().join("link");
        link_folder(&link, &target).unwrap();
        assert!(
            fs::symlink_metadata(&link)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert!(fs::metadata(&link).unwrap().is_dir());
        let read = fs::read(link.join("file.txt")).unwrap();
        assert_eq!(read, b"through the link");
        // A name that is taken is refused.
        assert!(link_folder(&link, &target).is_err());
    }

    #[test]
    fn file_links_read_as_links() {
        use super::{symlink_file, symlink_file_where_allowed};

        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("file.txt");
        fs::write(&target, b"through the link").unwrap();
        let link = dir.path().join("link.txt");
        if !symlink_file_where_allowed(&link, &target) {
            eprintln!("skipped: this account may not make symbolic links");
            return;
        }
        assert!(
            fs::symlink_metadata(&link)
                .unwrap()
                .file_type()
                .is_symlink()
        );
        assert_eq!(fs::read(&link).unwrap(), b"through the link");
        // One to nothing is made too, and is a link all the same.
        let dangling = dir.path().join("dangling.txt");
        symlink_file(&dangling, &dir.path().join("nowhere.txt")).unwrap();
        let kind = fs::symlink_metadata(&dangling).unwrap().file_type();
        assert!(kind.is_symlink());
        assert!(fs::metadata(&dangling).is_err());
    }

    #[test]
    fn symbolic_links_are_excused_only_where_the_privilege_is_withheld() {
        use std::panic;

        use super::{
            ERROR_PRIVILEGE_NOT_HELD, symlink_file_where_allowed, symlink_folder_where_allowed,
            withholds_links,
        };

        // Windows' refusal of an account without the privilege excuses a test there; nothing
        // else does, and on Unix, where any account makes links, nothing at all.
        let withheld = io::Error::from_raw_os_error(ERROR_PRIVILEGE_NOT_HELD);
        assert_eq!(withholds_links(&withheld), cfg!(windows));
        for error in [
            io::Error::from(io::ErrorKind::AlreadyExists),
            io::Error::from(io::ErrorKind::PermissionDenied),
            io::Error::from(io::ErrorKind::NotFound),
            io::Error::other("anything else"),
        ] {
            assert!(!withholds_links(&error), "{error}");
        }

        // Where this account may make links, as every account may on Unix, both kinds are made.
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("file.txt");
        fs::write(&target, b"target").unwrap();
        let folder = dir.path().join("folder");
        fs::create_dir(&folder).unwrap();
        let file_link = dir.path().join("file-link.txt");
        if !symlink_file_where_allowed(&file_link, &target) {
            eprintln!("only the refusals were checked: this account may not make symbolic links");
            return;
        }
        assert_eq!(fs::read(&file_link).unwrap(), b"target");
        let folder_link = dir.path().join("folder-link");
        assert!(symlink_folder_where_allowed(&folder_link, &folder));
        let kind = fs::symlink_metadata(&folder_link).unwrap().file_type();
        assert!(kind.is_symlink());
        assert!(fs::metadata(&folder_link).unwrap().is_dir());

        // A link that cannot be made there for another reason, such as a name already taken, is
        // the test's own fault, and fails it loudly instead of passing the case over.
        let taken = dir.path().join("taken.txt");
        fs::write(&taken, b"taken").unwrap();
        let over_file = panic::catch_unwind(|| symlink_file_where_allowed(&taken, &target));
        assert!(over_file.is_err(), "{over_file:?}");
        let over_folder = panic::catch_unwind(|| symlink_folder_where_allowed(&folder, &target));
        assert!(over_folder.is_err(), "{over_folder:?}");
        assert_eq!(fs::read(&taken).unwrap(), b"taken");
        assert!(fs::read_dir(&folder).unwrap().next().is_none());
    }

    #[test]
    fn folder_names_come_sorted() {
        use super::folder_names;

        let dir = tempfile::tempdir().unwrap();
        for name in ["b.json", "a", "C"] {
            fs::write(dir.path().join(name), b"").unwrap();
        }
        let inner = dir.path().join("d");
        fs::create_dir(&inner).unwrap();
        // In byte order, which puts capitals first.
        assert_eq!(folder_names(dir.path()).unwrap(), ["C", "a", "b.json", "d"]);
        assert!(folder_names(&inner).unwrap().is_empty());
        let error = folder_names(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);

        // A name that is no text has nothing to compare by.
        #[cfg(target_os = "linux")]
        {
            use std::ffi::OsStr;
            use std::os::unix::ffi::OsStrExt;

            fs::write(inner.join(OsStr::from_bytes(b"r\xff")), b"").unwrap();
            let error = folder_names(&inner).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        }
    }

    #[test]
    fn trees_list_their_files_as_the_vectors_do() {
        use std::panic;

        use serde_json::json;

        use super::{link_folder, listed, tree_listing};
        use crate::sha256_hex;

        let dir = tempfile::tempdir().unwrap();
        // A tree's files, in folders and not, are listed by their paths' text, which puts
        // capitals first, each with its size and digest, as the vectors list a tree.
        let tree = dir.path().join("tree");
        fs::create_dir_all(tree.join("b").join("c")).unwrap();
        fs::write(tree.join("b").join("c").join("x.bin"), b"deep").unwrap();
        fs::write(tree.join("a.json"), b"{}").unwrap();
        fs::write(tree.join("B.txt"), b"").unwrap();
        let rows = json!([
            ["B.txt", 0, sha256_hex(b"")],
            ["a.json", 2, sha256_hex(b"{}")],
            ["b/c/x.bin", 4, sha256_hex(b"deep")],
        ]);
        assert_eq!(tree_listing(&tree), listed(&rows));

        // A folder that holds nothing is no part of a tree an extraction writes, and neither is a
        // link, which is never followed.
        let empty = dir.path().join("empty");
        fs::create_dir_all(empty.join("b")).unwrap();
        fs::write(empty.join("a.bin"), b"a").unwrap();
        assert!(panic::catch_unwind(|| tree_listing(&empty)).is_err());
        let linked = dir.path().join("linked");
        fs::create_dir(&linked).unwrap();
        fs::write(linked.join("a.bin"), b"a").unwrap();
        link_folder(&linked.join("b"), &tree).unwrap();
        assert!(panic::catch_unwind(|| tree_listing(&linked)).is_err());
    }

    #[test]
    fn alpha_markers_take_their_kinds() {
        for upper in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let names: Vec<String> = ALPHA_NAMES
                .iter()
                .map(|name| {
                    if upper {
                        name.to_ascii_uppercase()
                    } else {
                        (*name).to_owned()
                    }
                })
                .collect();
            let names: Vec<&str> = names.iter().map(String::as_str).collect();
            make_alpha_markers(dir.path(), &names).unwrap();

            let listed: BTreeSet<String> = fs::read_dir(dir.path())
                .unwrap()
                .map(|entry| entry.unwrap().file_name().into_string().unwrap())
                .collect();
            let expected: BTreeSet<String> = names.iter().map(|name| name.to_string()).collect();
            assert_eq!(listed, expected);
            for name in &names {
                let path = dir.path().join(name);
                let metadata = fs::symlink_metadata(&path).unwrap();
                if ALPHA_FOLDER_NAMES
                    .iter()
                    .any(|folder| folder.eq_ignore_ascii_case(name))
                {
                    assert!(metadata.is_dir(), "{name}");
                    assert_eq!(fs::read_dir(&path).unwrap().count(), 0, "{name}");
                } else {
                    assert!(metadata.is_file(), "{name}");
                    assert_eq!(metadata.len(), 0, "{name}");
                }
            }
            // A name that is there already is refused, a folder's and a file's alike.
            for name in [names[0], names[2]] {
                let error = make_alpha_markers(dir.path(), &[name]).unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{name}");
            }
        }

        // Only single names, so nothing lands outside the folder, and nothing is made when one is
        // refused.
        let dir = tempfile::tempdir().unwrap();
        let outside = dir.path().join("outside");
        let outside = outside.to_str().unwrap();
        for name in ["", ".", "..", "a/b", "a/", "../escaped", outside] {
            let error = make_alpha_markers(dir.path(), &["installation.json", name]).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::InvalidInput, "{name:?}");
        }
        assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0);
    }

    #[cfg(unix)]
    #[test]
    fn denied_access_is_restored_on_drop() {
        use std::os::unix::fs::PermissionsExt;

        use super::DeniedAccess;

        let mode = |path: &Path| fs::metadata(path).unwrap().permissions().mode() & 0o7777;
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("file");
        let folder = dir.path().join("folder");
        let inner = folder.join("inner");
        fs::write(&file, b"kept").unwrap();
        fs::create_dir(&folder).unwrap();
        fs::write(&inner, b"inside").unwrap();
        // The folder's sticky bit shows that a mode comes back whole, not only its permissions.
        for (path, bits) in [(&file, 0o640), (&folder, 0o1750), (&inner, 0o604)] {
            fs::set_permissions(path, fs::Permissions::from_mode(bits)).unwrap();
        }

        let denied = DeniedAccess::deny(&[&file, &folder]).unwrap();
        assert_eq!(mode(&file), 0);
        assert_eq!(mode(&folder), 0);
        if fs::read_dir(&folder).is_ok() {
            eprintln!(
                "skipped the denial checks: this user lists folders of mode 0o000, as root does"
            );
        } else {
            let error = fs::read(&file).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
            let error = fs::read_dir(&folder).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
        }
        drop(denied);
        assert_eq!(mode(&file), 0o640);
        assert_eq!(mode(&folder), 0o1750);
        assert_eq!(fs::read(&file).unwrap(), b"kept");
        assert_eq!(fs::read_dir(&folder).unwrap().count(), 1);

        // What lies inside a folder is denied before it and given back after it.
        let nested = DeniedAccess::deny(&[&inner, &folder]).unwrap();
        assert_eq!(mode(&folder), 0);
        drop(nested);
        assert_eq!(mode(&folder), 0o1750);
        assert_eq!(mode(&inner), 0o604);
        assert_eq!(fs::read(&inner).unwrap(), b"inside");

        // An entry that cannot be denied gives back the modes already taken.
        let missing = dir.path().join("missing");
        let error = DeniedAccess::deny(&[&file, &missing]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        assert_eq!(mode(&file), 0o640);

        // A folder left only its listing gives its names, but nothing they name can be looked at
        // or opened through it, by anyone but root.
        let listed = DeniedAccess::listing_only(&[&folder]).unwrap();
        assert_eq!(mode(&folder), 0o400);
        assert_eq!(super::folder_names(&folder).unwrap(), ["inner"]);
        if super::holds_bind(dir.path()).unwrap() {
            let error = fs::symlink_metadata(&inner).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
            let error = fs::read(&inner).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
        } else {
            eprintln!("skipped the listing checks: this user looks into folders it may only list");
        }
        drop(listed);
        assert_eq!(mode(&folder), 0o1750);
        assert_eq!(fs::read(&inner).unwrap(), b"inside");

        // With every mode back, the folder is removed whole.
        dir.close().unwrap();
    }

    #[test]
    fn snapshots_see_what_changes() {
        use std::path::PathBuf;
        use std::time::{Duration, SystemTime};

        use sha2::{Digest, Sha256};

        use super::{link_folder, snapshot};

        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("tree");
        let inner = root.join("inner");
        fs::create_dir_all(&inner).unwrap();
        fs::write(root.join("a.txt"), b"first").unwrap();
        fs::write(inner.join("b.txt"), b"second").unwrap();
        let before = snapshot(&root).unwrap();
        // The root itself, every folder and every file, by path under the root.
        let paths: Vec<PathBuf> = before.keys().cloned().collect();
        let nested = Path::new("inner").join("b.txt");
        assert_eq!(
            paths,
            [
                PathBuf::new(),
                PathBuf::from("a.txt"),
                PathBuf::from("inner"),
                nested.clone()
            ]
        );
        assert!(before[Path::new("")].file_type.is_dir());
        assert!(before[Path::new("inner")].file_type.is_dir());
        assert!(before[Path::new("a.txt")].file_type.is_file());
        assert_eq!(before[Path::new("a.txt")].len, 5);

        // Reading a file, and looking again, change nothing.
        assert_eq!(fs::read(root.join("a.txt")).unwrap(), b"first");
        assert_eq!(snapshot(&root).unwrap(), before);

        // New contents, a new time of change and a new entry each show.
        fs::write(inner.join("b.txt"), b"changed!").unwrap();
        let changed = snapshot(&root).unwrap();
        assert_ne!(changed, before);
        assert_eq!(changed[&nested].len, 8);
        // Other bytes of the same length too, by their digest.
        fs::write(inner.join("b.txt"), b"CHANGED!").unwrap();
        let recased = snapshot(&root).unwrap();
        assert_eq!(recased[&nested].len, 8);
        assert_ne!(recased[&nested].digest, changed[&nested].digest);
        assert_eq!(
            recased[&nested].digest,
            Some(Sha256::digest(b"CHANGED!").into())
        );
        assert_eq!(recased[Path::new("inner")].digest, None);
        let long_ago = SystemTime::UNIX_EPOCH + Duration::from_secs(1);
        let file = fs::File::options()
            .write(true)
            .open(root.join("a.txt"))
            .unwrap();
        file.set_modified(long_ago).unwrap();
        drop(file);
        let touched = snapshot(&root).unwrap();
        assert_eq!(touched[Path::new("a.txt")].modified, long_ago);
        assert_eq!(touched[Path::new("a.txt")].len, 5);
        fs::write(inner.join("c.txt"), b"").unwrap();
        let grown = snapshot(&root).unwrap();
        assert!(grown.contains_key(&Path::new("inner").join("c.txt")));
        assert_eq!(grown.len(), before.len() + 1);

        // A link is recorded as a link, and what it leads to is not walked.
        let outside = dir.path().join("outside");
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("far.txt"), b"far").unwrap();
        link_folder(&root.join("link"), &outside).unwrap();
        let linked = snapshot(&root).unwrap();
        assert!(linked[Path::new("link")].file_type.is_symlink());
        assert!(!linked.keys().any(|path| path.ends_with("far.txt")));

        // A tree that is not there gives nothing to compare.
        let error = snapshot(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
    }

    #[test]
    fn held_entries_cannot_be_opened_until_dropped() {
        use super::{folder_names, hold_closed, holds_bind};

        let dir = tempfile::tempdir().unwrap();
        // Asked of a file of its own, made and removed again, before anything is held.
        let binds = holds_bind(dir.path()).unwrap();
        assert!(folder_names(dir.path()).unwrap().is_empty());
        let file = dir.path().join("file.txt");
        fs::write(&file, b"held").unwrap();
        let folder = dir.path().join("folder");
        fs::create_dir(&folder).unwrap();
        // On Windows a hold that any open passes is an error, so only root's on Unix is not
        // proven, and a hold is proven exactly where holds bind this process.
        let held = hold_closed(&[&file, &folder]).unwrap();
        assert_eq!(held.proven(), binds);
        if held.proven() {
            assert!(fs::read(&file).is_err());
            #[cfg(unix)]
            assert!(fs::read_dir(&folder).is_err());
        } else {
            eprintln!(
                "only the hold was made: this user opens entries of mode 0o000, as root does"
            );
        }
        // The listing of the folder that holds them is not kept from anyone.
        assert_eq!(folder_names(dir.path()).unwrap(), ["file.txt", "folder"]);
        drop(held);
        assert_eq!(fs::read(&file).unwrap(), b"held");
        assert!(folder_names(&folder).unwrap().is_empty());

        // Nothing there cannot be held.
        let missing = dir.path().join("missing");
        let error = hold_closed(&[&missing]).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
    }

    /// What a folder's own permissions are, to compare before and after a change: on Windows the
    /// entries icacls lists for it.
    #[cfg(windows)]
    fn permissions_of(folder: &Path) -> String {
        let icacls = super::icacls_program().unwrap();
        let listed = super::run_icacls(&icacls, &[folder.as_os_str()]).unwrap();
        String::from_utf8_lossy(&listed).into_owned()
    }

    /// What a folder's own permissions are, to compare before and after a change: on Unix its
    /// mode, the set-id and sticky bits included.
    #[cfg(unix)]
    fn permissions_of(folder: &Path) -> u32 {
        use std::os::unix::fs::PermissionsExt;

        fs::symlink_metadata(folder).unwrap().permissions().mode() & 0o7777
    }

    /// The program beside icacls in the system's own folder that lists the privileges of the
    /// token it runs with, a copy of this process's.
    #[cfg(windows)]
    const WHOAMI: &str = "whoami.exe";

    /// Whether this process's restore privilege is enabled, as whoami lists it: in the state it
    /// lists for bypassing traverse checking, which every token holds enabled, so that the
    /// system's language does not matter.
    #[cfg(windows)]
    fn restore_privilege_enabled() -> bool {
        use std::os::windows::process::CommandExt;
        use std::process::Command;

        let whoami = super::icacls_program().unwrap().with_file_name(WHOAMI);
        let output = Command::new(whoami)
            .args(["/priv", "/fo", "csv", "/nh"])
            .creation_flags(super::CREATE_NO_WINDOW)
            .output()
            .unwrap();
        assert!(output.status.success(), "{output:?}");
        let listed = String::from_utf8_lossy(&output.stdout);
        let state = |privilege: &str| {
            let quoted = format!("\"{privilege}\",");
            listed
                .lines()
                .find(|line| line.starts_with(&quoted))
                .and_then(|line| line.rsplit(',').next())
                .map(str::to_owned)
        };
        let bypass = state("SeChangeNotifyPrivilege");
        assert!(bypass.is_some(), "{listed}");
        state("SeRestorePrivilege") == bypass
    }

    #[test]
    fn denied_writes_are_restored() {
        use super::{WRITE_PROBE, deny_writes, folder_names, link_folder};

        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("folder");
        let inner = folder.join("inner");
        fs::create_dir_all(&inner).unwrap();
        fs::write(folder.join("kept.txt"), b"kept").unwrap();
        let gone = folder.join("gone.txt");
        fs::write(&gone, b"gone").unwrap();
        // The sticky bit shows that a mode comes back whole, not only its permissions.
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;

            fs::set_permissions(&folder, fs::Permissions::from_mode(0o1750)).unwrap();
        }
        let before = permissions_of(&folder);

        let denied = deny_writes(&folder).unwrap();
        assert_ne!(permissions_of(&folder), before);
        // The deny entry binds every account on Windows; on Unix root alone passes the mode.
        #[cfg(windows)]
        assert!(denied.proven());
        #[cfg(unix)]
        assert_eq!(permissions_of(&folder), 0o555);
        // Its names are still listed and what it holds read, and its own check left nothing.
        assert_eq!(
            folder_names(&folder).unwrap(),
            ["gone.txt", "inner", "kept.txt"]
        );
        assert_eq!(fs::read(folder.join("kept.txt")).unwrap(), b"kept");
        if denied.proven() {
            // No file is made in it, nor on Unix a folder, which on Windows depends on this
            // process's privileges, as the next test shows; ...
            let error = fs::File::create_new(folder.join("new.txt")).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
            #[cfg(unix)]
            {
                let error = fs::create_dir(folder.join("new")).unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
            }
            // ... but what it holds keeps its own rights: a folder inside takes a new file, and a
            // file inside is written again.
            fs::write(inner.join("deeper.txt"), b"deeper").unwrap();
            fs::write(folder.join("kept.txt"), b"kept again").unwrap();
            // Removing what it holds is refused on Unix alone.
            let removed = fs::remove_file(&gone);
            if cfg!(windows) {
                removed.unwrap();
            } else {
                assert_eq!(removed.unwrap_err().kind(), io::ErrorKind::PermissionDenied);
            }
        } else {
            eprintln!(
                "skipped the refusals: this user makes entries in a folder of mode 0o555, as root \
                 does"
            );
        }

        // Dropped, it gives the folder its permissions back, and the folder takes new entries.
        drop(denied);
        assert_eq!(permissions_of(&folder), before);
        fs::write(folder.join("new.txt"), b"new").unwrap();
        fs::create_dir(folder.join("new")).unwrap();
        let names = folder_names(&folder).unwrap();
        assert!(names.iter().any(|name| name == "new.txt"), "{names:?}");
        assert!(names.iter().any(|name| name == "new"), "{names:?}");

        // Anything but a folder is refused before anything changes: a file, a link to the folder,
        // nothing at all, and a folder that holds the name its own check takes.
        let file = folder.join("kept.txt");
        let link = dir.path().join("link");
        link_folder(&link, &folder).unwrap();
        for path in [&file, &link] {
            let error = deny_writes(path).unwrap_err();
            assert_eq!(
                error.kind(),
                io::ErrorKind::InvalidInput,
                "{}",
                path.display()
            );
        }
        let error = deny_writes(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
        let taken = folder.join(WRITE_PROBE);
        fs::write(&taken, b"taken").unwrap();
        let error = deny_writes(&folder).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::AlreadyExists);
        assert_eq!(fs::read(&taken).unwrap(), b"taken");
        assert_eq!(permissions_of(&folder), before);
        fs::remove_file(&taken).unwrap();

        // With its permissions back, the folder is removed whole.
        dir.close().unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn a_folder_denied_writes_still_lists() {
        use super::{ERROR_ACCESS_DENIED, deny_writes};

        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("folder");
        fs::create_dir_all(folder.join("inner")).unwrap();
        fs::write(folder.join("kept.txt"), b"kept").unwrap();
        let denied = deny_writes(&folder).unwrap();
        // The deny entry binds every account, an administrator's full token included.
        assert!(denied.proven());

        // The folder and what it holds are listed and looked at, and its files read, as a folder
        // is judged before anything is made in it.
        let mut names: Vec<String> = fs::read_dir(&folder)
            .unwrap()
            .map(|entry| entry.unwrap().file_name().into_string().unwrap())
            .collect();
        names.sort();
        assert_eq!(names, ["inner", "kept.txt"]);
        assert!(fs::symlink_metadata(&folder).unwrap().is_dir());
        assert!(fs::symlink_metadata(folder.join("inner")).unwrap().is_dir());
        assert!(
            fs::symlink_metadata(folder.join("kept.txt"))
                .unwrap()
                .is_file()
        );
        assert_eq!(fs::read(folder.join("kept.txt")).unwrap(), b"kept");

        // A new file is refused with the system's own denial of access, as std makes one, and as
        // tempfile makes one too, whose error keeps that kind.
        let error = fs::File::create_new(folder.join("new.txt")).unwrap_err();
        assert_eq!(error.raw_os_error(), Some(ERROR_ACCESS_DENIED), "{error}");
        let error = tempfile::Builder::new()
            .prefix("new-")
            .tempfile_in(&folder)
            .unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied, "{error}");
        // So is a new folder, but for a process whose restore privilege is enabled: the system
        // makes a folder with backup intent, which that privilege lets past the entry.
        let new = folder.join("new");
        match fs::create_dir(&new) {
            Err(error) => assert_eq!(error.raw_os_error(), Some(ERROR_ACCESS_DENIED), "{error}"),
            Ok(()) => {
                assert!(
                    restore_privilege_enabled(),
                    "a folder was made past the entry without the restore privilege"
                );
                fs::remove_dir(&new).unwrap();
            }
        }
        assert_eq!(super::folder_names(&folder).unwrap(), ["inner", "kept.txt"]);

        drop(denied);
        fs::File::create_new(folder.join("new.txt")).unwrap();
    }

    #[test]
    fn backdated_trees_show_any_later_write() {
        use std::path::PathBuf;
        use std::time::SystemTime;

        use super::{LONG_AGO, backdate, link_folder, snapshot};

        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().join("tree");
        let inner = root.join("inner");
        fs::create_dir_all(&inner).unwrap();
        let file = inner.join("same.txt");
        fs::write(&file, b"same").unwrap();
        let outside = dir.path().join("outside");
        fs::create_dir(&outside).unwrap();
        link_folder(&root.join("link"), &outside).unwrap();
        let outside_before = fs::metadata(&outside).unwrap().modified().unwrap();

        // Every folder and file is dated long ago; a link is not followed.
        let long_ago = SystemTime::UNIX_EPOCH + LONG_AGO;
        backdate(&root).unwrap();
        let before = snapshot(&root).unwrap();
        for (path, state) in &before {
            if !state.file_type.is_symlink() {
                assert_eq!(state.modified, long_ago, "{}", path.display());
            }
        }
        assert_eq!(
            fs::metadata(&outside).unwrap().modified().unwrap(),
            outside_before
        );

        // The same bytes written again show by their time, though not by their digest.
        fs::write(&file, b"same").unwrap();
        let rewritten = snapshot(&root).unwrap();
        let same = PathBuf::from("inner").join("same.txt");
        assert_eq!(rewritten[&same].digest, before[&same].digest);
        assert_ne!(rewritten[&same].modified, before[&same].modified);
        assert_ne!(rewritten, before);

        // So does an entry made and removed again, in the folder that held it.
        backdate(&root).unwrap();
        let before = snapshot(&root).unwrap();
        let passing = inner.join("passing");
        fs::create_dir(&passing).unwrap();
        fs::remove_dir(&passing).unwrap();
        let after = snapshot(&root).unwrap();
        assert_eq!(after.len(), before.len());
        assert_ne!(
            after[Path::new("inner")].modified,
            before[Path::new("inner")].modified
        );

        // A tree that is not there cannot be dated.
        let error = backdate(&dir.path().join("missing")).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
    }

    #[test]
    fn the_outside_snapshot_leaves_out_the_folder_and_its_staging() {
        use std::path::PathBuf;

        use super::{backdate, snapshot, snapshot_outside};

        let dir = tempfile::tempdir().unwrap();
        let parent = dir.path().join("parent");
        let dump = parent.join("dump");
        // Kept: a name below the top that begins with the prefix, a name at the top that holds the
        // prefix without beginning with it, and one that begins with it in other letter case.
        let deeper_name = format!("{STAGING_PREFIX}deeper");
        let holding_name = format!("old{STAGING_PREFIX}1");
        let cased_name = format!("{}1", STAGING_PREFIX.to_ascii_uppercase());
        let deeper = dump.join(&deeper_name);
        fs::create_dir_all(&deeper).unwrap();
        fs::write(dump.join("rom.z64"), b"not a real rom").unwrap();
        fs::write(parent.join("notes.txt"), b"kept").unwrap();
        fs::write(parent.join(&holding_name), b"kept too").unwrap();
        fs::write(parent.join(&cased_name), b"kept as well").unwrap();
        // Dated long ago, so that an entry made in the folder or removed from it moves its time.
        backdate(&parent).unwrap();
        let whole = snapshot(&parent).unwrap();
        let before = snapshot_outside(&parent, STAGING_PREFIX).unwrap();
        // With nothing under the prefix, the whole snapshot less the folder's own entry.
        let mut expected = whole.clone();
        expected.remove(Path::new(""));
        assert_eq!(before, expected);
        let kept: BTreeSet<PathBuf> = before.keys().cloned().collect();
        let listed: BTreeSet<PathBuf> = [
            PathBuf::from("dump"),
            Path::new("dump").join(&deeper_name),
            Path::new("dump").join("rom.z64"),
            PathBuf::from("notes.txt"),
            PathBuf::from(&holding_name),
            PathBuf::from(&cased_name),
        ]
        .into_iter()
        .collect();
        assert_eq!(kept, listed);

        // An entry made and removed under the prefix moves the folder's own time, which the whole
        // snapshot shows and the outside one leaves out.
        let passing = parent.join(format!("{STAGING_PREFIX}AbC123xy"));
        fs::create_dir(&passing).unwrap();
        fs::remove_dir(&passing).unwrap();
        let moved = snapshot(&parent).unwrap();
        assert_ne!(moved[Path::new("")].modified, whole[Path::new("")].modified);
        assert_eq!(snapshot_outside(&parent, STAGING_PREFIX).unwrap(), before);

        // A staging that is still there, with what a job wrote in it, is left out whole.
        let staging = tempfile::Builder::new()
            .prefix(STAGING_PREFIX)
            .rand_bytes(8)
            .tempdir_in(&parent)
            .unwrap();
        let levels = staging.path().join("n64").join("levels");
        fs::create_dir_all(&levels).unwrap();
        fs::write(levels.join("a.wad"), b"staged").unwrap();
        let outside = snapshot_outside(&parent, STAGING_PREFIX).unwrap();
        assert_eq!(outside, before);
        // The whole snapshot less the folder's own entry and every path whose first component
        // begins with the prefix.
        let mut whole_now = snapshot(&parent).unwrap();
        assert_eq!(whole_now.len(), whole.len() + 4);
        whole_now.retain(|path, _| {
            path.iter()
                .next()
                .and_then(|first| first.to_str())
                .is_some_and(|first| !first.starts_with(STAGING_PREFIX))
        });
        assert_eq!(outside, whole_now);

        // A changed byte in a kept file shows, there alone.
        let notes = Path::new("notes.txt");
        fs::write(parent.join(notes), b"kepT").unwrap();
        let changed = snapshot_outside(&parent, STAGING_PREFIX).unwrap();
        assert!(changed.keys().eq(before.keys()));
        let differing: Vec<&PathBuf> = before
            .keys()
            .filter(|path| changed[*path] != before[*path])
            .collect();
        assert_eq!(differing, [notes]);
        assert_eq!(changed[notes].len, before[notes].len);
        assert_ne!(changed[notes].digest, before[notes].digest);

        // So does an entry made below the top, under a name that begins with the prefix.
        fs::write(deeper.join("inside"), b"").unwrap();
        let grown = snapshot_outside(&parent, STAGING_PREFIX).unwrap();
        let inside = Path::new("dump").join(&deeper_name).join("inside");
        assert!(grown.contains_key(&inside));
        assert_eq!(grown.len(), before.len() + 1);
    }

    #[test]
    fn the_outside_snapshot_never_reads_what_it_leaves_out() {
        use super::{hold_closed, holds_bind, snapshot, snapshot_outside};

        let dir = tempfile::tempdir().unwrap();
        // Asked of a file of its own, made and removed again, before anything is held.
        let binds = holds_bind(dir.path()).unwrap();
        let parent = dir.path().join("parent");
        let staging = parent.join(format!("{STAGING_PREFIX}AbC123xy"));
        fs::create_dir_all(&staging).unwrap();
        fs::write(parent.join("kept.txt"), b"kept").unwrap();
        let held = staging.join("held.bin");
        fs::write(&held, b"held").unwrap();
        let before = snapshot_outside(&parent, STAGING_PREFIX).unwrap();

        let hold = hold_closed(&[&held]).unwrap();
        assert_eq!(hold.proven(), binds);
        if hold.proven() {
            // The whole tree cannot be read while a file in the staging is held, ...
            assert!(snapshot(&parent).is_err());
        } else {
            eprintln!(
                "only the outside snapshot was taken: this user opens entries of mode 0o000, as \
                 root does"
            );
        }
        // ... but what lies outside the staging can, and is as it was.
        assert_eq!(snapshot_outside(&parent, STAGING_PREFIX).unwrap(), before);
        drop(hold);
        assert!(snapshot(&parent).is_ok());
    }

    #[test]
    fn an_outside_snapshot_needs_a_folder_and_a_prefix() {
        use std::path::PathBuf;

        use super::{link_folder, snapshot, snapshot_outside};

        let dir = tempfile::tempdir().unwrap();
        let folder = dir.path().join("folder");
        fs::create_dir(&folder).unwrap();
        let file = folder.join("kept.txt");
        fs::write(&file, b"kept").unwrap();
        let link = dir.path().join("link");
        link_folder(&link, &folder).unwrap();

        // A folder gives what it holds.
        let kept: Vec<PathBuf> = snapshot_outside(&folder, STAGING_PREFIX)
            .unwrap()
            .into_keys()
            .collect();
        assert_eq!(kept, [PathBuf::from("kept.txt")]);

        // An empty prefix would leave out every name, and a file or a link, which a snapshot
        // records as its own entry alone, would leave nothing once that entry is left out: each
        // is refused, so that no comparison of nothing passes for one of the folder.
        let error = snapshot_outside(&folder, "").unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
        for root in [&file, &link] {
            assert_eq!(snapshot(root).unwrap().len(), 1, "{}", root.display());
            let error = snapshot_outside(root, STAGING_PREFIX).unwrap_err();
            assert_eq!(
                error.kind(),
                io::ErrorKind::InvalidInput,
                "{}",
                root.display()
            );
        }
        // A folder that is not there is an error of its own.
        let error = snapshot_outside(&dir.path().join("missing"), STAGING_PREFIX).unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::NotFound);
    }

    #[test]
    fn install_fixtures_lay_out_an_installation() {
        use serde_json::{Value, json};

        use super::{InstallFixture, folder_names};
        use crate::runtime::{RuntimeFiles, sha256_hex};

        let dir = tempfile::tempdir().unwrap();
        let folds_case = names_fold_case(dir.path());
        for (platform, executable, launcher) in [
            (
                "windows-x64",
                "gauntlet.exe",
                "GauntletDarkLegacy-Launcher.exe",
            ),
            ("linux-x64", "gauntlet", "GauntletDarkLegacy-Launcher"),
        ] {
            let windows = platform == "windows-x64";
            // The folder is made, with the folders above it.
            let root = dir.path().join(platform).join("install");
            let runtime = RuntimeFiles::standard(platform, "1.0.0-beta");
            if folds_case && !windows {
                // The Linux executable and the disc's folder are one name here, so the layout is
                // refused rather than merged.
                let error = InstallFixture::write(&root, runtime).unwrap_err();
                assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{platform}");
                eprintln!(
                    "only the refusal of the {platform} layout was checked: this file system takes \
                     gauntlet and Gauntlet for one name"
                );
                continue;
            }
            let fixture = InstallFixture::write(&root, runtime.clone()).unwrap();
            assert_eq!(fixture.root, root);
            let mut names = vec![
                ".gdl-launcher",
                "Gauntlet",
                launcher,
                "VERSION",
                "build-info.json",
                "carddemo",
                "config",
                "data",
                executable,
                "portable.flag",
                "saves",
                "shaders",
            ];
            names.sort_unstable();
            assert_eq!(folder_names(&root).unwrap(), names, "{platform}");
            let read = |name: &str| {
                let path = name
                    .split('/')
                    .fold(root.clone(), |path, part| path.join(part));
                fs::read(path).unwrap()
            };

            // The runtime as it ships, but for the Windows executable, which carries the disc's
            // icon after its own bytes.
            for (name, data) in &runtime.files {
                let installed = read(name);
                if windows && name == executable {
                    assert!(installed.len() > data.len(), "{name}");
                    assert!(installed.starts_with(data), "{name}");
                } else {
                    assert_eq!(installed, *data, "{platform} {name}");
                }
            }
            assert_eq!(read("build-info.json"), runtime.build_info_bytes());
            assert!(!read("Gauntlet/PDATA/WAR.WAD").is_empty());
            assert!(!read("carddemo/icon.tpl").is_empty());
            for folder in ["saves", "config"] {
                assert!(folder_names(&root.join(folder)).unwrap().is_empty());
            }
            assert_eq!(
                folder_names(&root.join(".gdl-launcher")).unwrap(),
                ["receipt.json"]
            );

            // The receipt it writes is the one it gives, and pins what it laid out.
            let bytes = read(".gdl-launcher/receipt.json");
            let receipt: Value = serde_json::from_slice(&bytes).unwrap();
            assert_eq!(receipt, fixture.receipt(), "{platform}");
            let digest = |name: &str| Value::String(sha256_hex(&read(name)));
            let mut expected = json!({
                "schema": 1,
                "minLauncher": "1.0.0-beta",
                "version": "1.0.0-beta",
                "commit": "test-commit",
                "platform": platform,
                "disc": "GUNE5D",
                "discRevision": 1,
                "buildInfoSha256": digest("build-info.json"),
                "installedDigests": {},
                "launcher": {"name": launcher, "sha256": digest(launcher)},
            });
            if windows {
                expected["installedDigests"] = json!({executable: digest(executable)});
                expected["discIcon"] = json!({
                    "path": "carddemo/icon.tpl",
                    "sha256": digest("carddemo/icon.tpl"),
                    "releaseExecutableSha256": runtime.build_info()["files"][executable],
                });
            }
            assert_eq!(receipt, expected, "{platform}");
            // Written as the launcher writes JSON.
            let text = std::str::from_utf8(&bytes).unwrap();
            assert!(text.starts_with("{\n  \""), "{text}");
            assert!(text.ends_with("\n}\n"), "{text}");
            assert!(!text.contains('\r'), "{text}");

            // Another receipt takes its place whole.
            fixture.write_receipt(&json!({"schema": 2})).unwrap();
            assert_eq!(
                read(".gdl-launcher/receipt.json"),
                b"{\n  \"schema\": 2\n}\n"
            );
            // Nothing is laid over an installation that is there.
            let error = InstallFixture::write(&root, runtime).unwrap_err();
            assert_eq!(error.kind(), io::ErrorKind::AlreadyExists, "{platform}");
        }

        // The executable's digest as the release shipped it is the one its record lists: none,
        // when the record lists none.
        let mut runtime = RuntimeFiles::standard("windows-x64", "1.0.0-beta");
        runtime.extra.insert("files".to_owned(), json!({}));
        let fixture = InstallFixture::write(&dir.path().join("unlisted"), runtime).unwrap();
        assert_eq!(
            fixture.receipt()["discIcon"]["releaseExecutableSha256"],
            Value::Null
        );
    }
}
