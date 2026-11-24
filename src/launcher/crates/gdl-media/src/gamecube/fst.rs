//! The file table of a GameCube disc, walked as the installer walks it: big-endian rows of twelve
//! bytes, the root's first, each a kind and a place among the names after the rows, then a file's
//! offset and size or a folder's parent and the index past its last entry, every folder holding
//! the rows up to that index. Each name is judged by itself and joined to the path of the folder
//! it lies in; what lies under the disc's `Gauntlet` and `carddemo` folders is kept, as the disc
//! spells it.

use std::collections::HashSet;
use std::hash::{Hash, Hasher};

use gdl_files::{DISC_NAMES, MAX_COMPONENT_CHARS, NameRule, RelPath, check_component};

use super::DISC_SIZE;
use crate::error::{long_start, quoted_lossy};
use crate::{GameCubeRule, MediaError};

/// The most entries the disc's `Gauntlet` and `carddemo` folders may hold together, themselves
/// included: some three times the game's own, and few enough that the list of what was installed
/// stays well within the most the launcher reads of a state file.
pub const MAX_DISC_ENTRIES: usize = 10_000;

/// The length of a row of the table: three big-endian words.
pub(crate) const FST_ROW_LEN: usize = 12;

/// The length of a word of a row.
const WORD_LEN: usize = 4;

/// Where a row gives a file's offset or a folder's parent.
const ROW_OFFSET_AT: usize = 4;

/// Where a row gives a file's size or the index past a folder's last entry.
const ROW_SIZE_AT: usize = 8;

/// The first word of the root's row: a folder's kind and the empty name at the names' start.
const ROOT_WORD: u32 = 0x0100_0000;

/// The root's parent, as its row gives it.
const ROOT_PARENT: u32 = 0;

/// The index of the root's row, the parent the folders at the top of the disc name.
const ROOT_INDEX: usize = 0;

/// How far the first word of a row is shifted to give its kind.
const KIND_SHIFT: u32 = 24;

/// The bits of the first word of a row that give where its name starts among the names.
const NAME_OFFSET_MASK: u32 = 0x00FF_FFFF;

/// The kind of a file's row.
const FILE_KIND: u32 = 0;

/// The kind of a folder's row.
const FOLDER_KIND: u32 = 1;

/// What ends a name among the names.
const NAME_END: u8 = 0;

/// The separator of the components of a path.
const SEPARATOR: char = '/';

/// The folders at the top of the disc whose trees are installed, in ASCII lower case.
const KEPT_ROOTS: [&str; 2] = ["gauntlet", "carddemo"];

/// The files the game cannot start without, by their paths in ASCII lower case: the war's WAD,
/// the tower's, and the icon the game shows while it saves.
const REQUIRED_FILES: [&str; 3] = [
    "gauntlet/pdata/war.wad",
    "gauntlet/wdata/tower.wad",
    "carddemo/icon.tpl",
];

/// What the installer answers for a table whose first row is not a root folder's.
const INVALID_ROOT: &str = "Invalid GameCube filesystem root";

/// What it answers for a row of an unknown kind or whose name cannot be found.
const INVALID_ENTRY: &str = "Invalid GameCube filesystem entry";

/// What it answers for a name the installed tree may not hold, before the name.
const UNSAFE_COMPONENT: &str = "Unsafe disc path component";

/// What it answers for a path taken twice or too long, before the path.
const DUPLICATE_OR_OVERLONG: &str = "Duplicate or overlong disc path";

/// What it answers for a folder nested other than the rows around it say.
const INVALID_NESTING: &str = "Invalid filesystem directory nesting";

/// What it answers for a file that runs past the disc, before the file's path.
const TRUNCATED_FILE: &str = "Truncated disc file";

/// What it answers for a file at the top of the disc where a kept folder should be.
const ROOT_NOT_FOLDER: &str = "Asset root must be a directory";

/// What it answers for files that hold more than a disc together.
const IMPLAUSIBLE_TOTAL: &str = "Implausible total extracted size";

/// What it answers for a disc without both kept folders.
const MISSING_ROOTS: &str = "Disc is missing Gauntlet or carddemo assets";

/// What it answers for a disc without a file the game cannot start without.
const MISSING_DATA: &str = "Disc is missing required native game data";

/// What an entry of the disc's tree is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DiscEntryKind {
    /// A folder.
    Folder,
    /// A file.
    File {
        /// Where its bytes start on the disc.
        offset: u64,
        /// How many bytes it holds.
        size: u64,
    },
}

/// An entry of the disc's `Gauntlet` or `carddemo` tree: its path, spelt as the disc spells it,
/// and what it is.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DiscEntry {
    /// Its path from the top of the disc, every component of it a name the installed tree may
    /// hold.
    pub path: RelPath,
    /// Whether it is a folder or a file, and where a file's bytes lie.
    pub kind: DiscEntryKind,
}

/// What the walk of a file table keeps.
pub(crate) struct Walked {
    /// The entries of the kept folders' trees, in the table's order.
    pub(crate) entries: Vec<DiscEntry>,
    /// How many bytes their files hold together.
    pub(crate) files_len: u64,
}

/// A row of the table.
struct Row {
    /// Its kind in the top byte and where its name starts in the rest.
    word: u32,
    /// A file's offset or a folder's parent.
    offset: u32,
    /// A file's size or the index past a folder's last entry.
    size: u32,
}

/// A folder the walk is inside, whose names are borrowed from the table `'t`.
struct OpenFolder<'t> {
    /// The index of its row.
    index: usize,
    /// The index past its last entry.
    end: usize,
    /// Its path, or `None` for the root.
    path: Option<RelPath>,
    /// The names its entries have taken. No two entries of one folder may share a name in any
    /// ASCII letter case, and no two folders share a path, so this keeps every path of the tree
    /// apart from every other in any letter case. Each name is the table's own bytes, so that a
    /// folder of as many rows as the table can hold costs a few words a row, not a copy of each
    /// name.
    names: HashSet<FoldedName<'t>>,
}

/// A name of the table as a folder's names hold it: its bytes, borrowed from the table, equal to
/// another's in any ASCII letter case, as the names of one file on a case-insensitive file system
/// are. Only names held to the names the installed tree may hold, and so only ASCII, are kept.
#[derive(Clone, Copy)]
struct FoldedName<'t>(&'t [u8]);

impl PartialEq for FoldedName<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.0.eq_ignore_ascii_case(other.0)
    }
}

impl Eq for FoldedName<'_> {}

impl Hash for FoldedName<'_> {
    /// Hashes the name in ASCII lower case, so that names equal in any letter case hash alike.
    fn hash<H: Hasher>(&self, state: &mut H) {
        state.write_usize(self.0.len());
        for byte in self.0 {
            state.write_u8(byte.to_ascii_lowercase());
        }
    }
}

/// The entries of the file table `table` of a disc of `disc_size` bytes, walked in the table's
/// order and judged as the installer judges them: the root's row first, counting the rows; then
/// each row's kind and name, the name held to the names the installed tree may hold, its path to
/// [`MAX_DISC_PATH_CHARS`](gdl_files::MAX_DISC_PATH_CHARS) characters and apart from every other
/// path in any ASCII letter case, a folder to the folder it lies in, and a file, kept or not, to
/// the disc. What lies under the `Gauntlet` and `carddemo` folders at the top is kept, no more
/// than [`MAX_DISC_ENTRIES`] of it, its files holding no more than a disc together, and both
/// folders must be there with the files the game cannot start without. The first rule broken is
/// the refusal.
pub(crate) fn walk(table: &[u8], disc_size: u64) -> Result<Walked, MediaError> {
    let root = row(table, ROOT_INDEX).ok_or_else(invalid_root)?;
    let count = usize::try_from(root.size).unwrap_or(usize::MAX);
    if root.word != ROOT_WORD
        || root.offset != ROOT_PARENT
        || count == 0
        || count > table.len() / FST_ROW_LEN
    {
        return Err(invalid_root());
    }
    let names_at = count * FST_ROW_LEN;
    let mut open = vec![OpenFolder {
        index: ROOT_INDEX,
        end: count,
        path: None,
        names: HashSet::new(),
    }];
    let mut entries = Vec::new();
    let mut files_len: u64 = 0;
    for index in ROOT_INDEX + 1..count {
        // The root ends where the table does, so it is never left.
        while open.len() > 1 && open.last().is_some_and(|folder| index >= folder.end) {
            open.pop();
        }
        let row = row(table, index).ok_or_else(invalid_root)?;
        let kind = row.word >> KIND_SHIFT;
        let name_at = names_at + (row.word & NAME_OFFSET_MASK) as usize;
        let name = table
            .get(name_at..)
            .and_then(|rest| {
                let end = rest.iter().position(|&byte| byte == NAME_END)?;
                Some(&rest[..end])
            })
            .filter(|_| kind == FILE_KIND || kind == FOLDER_KIND)
            .ok_or_else(|| MediaError::gamecube(GameCubeRule::FstEntry, INVALID_ENTRY))?;
        let parent = open.last_mut().expect("the root is always open");
        let path = take(parent, name)?;
        let folder = kind == FOLDER_KIND;
        if folder {
            let parent_index = usize::try_from(row.offset).unwrap_or(usize::MAX);
            let end = usize::try_from(row.size).unwrap_or(usize::MAX);
            if parent_index != parent.index || end <= index || end > parent.end {
                return Err(MediaError::gamecube(GameCubeRule::Nesting, INVALID_NESTING));
            }
            open.push(OpenFolder {
                index,
                end,
                path: Some(path.clone()),
                names: HashSet::new(),
            });
        } else if u64::from(row.offset) + u64::from(row.size) > disc_size {
            let detail = format!("{TRUNCATED_FILE}: {}", path.as_str());
            return Err(MediaError::gamecube(GameCubeRule::TruncatedFile, detail));
        }
        if !is_kept(&path) {
            continue;
        }
        if path.depth() == 1 && !folder {
            return Err(MediaError::gamecube(
                GameCubeRule::RootNotFolder,
                ROOT_NOT_FOLDER,
            ));
        }
        if entries.len() == MAX_DISC_ENTRIES {
            let detail = format!(
                "The disc's Gauntlet and carddemo folders hold more than {MAX_DISC_ENTRIES} entries"
            );
            return Err(MediaError::gamecube(GameCubeRule::TooManyEntries, detail));
        }
        let kind = if folder {
            DiscEntryKind::Folder
        } else {
            files_len += u64::from(row.size);
            DiscEntryKind::File {
                offset: u64::from(row.offset),
                size: u64::from(row.size),
            }
        };
        entries.push(DiscEntry { path, kind });
    }
    if files_len > DISC_SIZE {
        return Err(MediaError::gamecube(
            GameCubeRule::TotalSize,
            IMPLAUSIBLE_TOTAL,
        ));
    }
    let holds = |folder: bool, folded: &str| {
        entries.iter().any(|entry| {
            (entry.kind == DiscEntryKind::Folder) == folder
                && entry.path.as_str().eq_ignore_ascii_case(folded)
        })
    };
    if !KEPT_ROOTS.iter().all(|root| holds(true, root)) {
        return Err(MediaError::gamecube(
            GameCubeRule::MissingRoots,
            MISSING_ROOTS,
        ));
    }
    if !REQUIRED_FILES.iter().all(|file| holds(false, file)) {
        return Err(MediaError::gamecube(
            GameCubeRule::MissingData,
            MISSING_DATA,
        ));
    }
    Ok(Walked { entries, files_len })
}

/// The path of the entry whose name is `name`, bytes of the table, in `folder`, the name taken
/// into the folder's names: read as UTF-8, a byte that is no UTF-8 as U+FFFD, it must be a name
/// the installed tree may hold by itself, ASCII alone; joined to the folder's path it must make a
/// path of no more than [`MAX_DISC_PATH_CHARS`](gdl_files::MAX_DISC_PATH_CHARS) characters; and
/// no other entry of the folder may have taken it in any ASCII letter case.
fn take<'t>(folder: &mut OpenFolder<'t>, name: &'t [u8]) -> Result<RelPath, MediaError> {
    let unsafe_name = || {
        let detail = quoted_lossy(UNSAFE_COMPONENT, name);
        MediaError::gamecube(GameCubeRule::UnsafeName, detail)
    };
    // More bytes than a component may hold characters break one of its rules whatever they are:
    // ASCII, they are too many characters, and otherwise they are no ASCII. So a name that long,
    // which may run to the end of the table, is refused before it is read as text, which would
    // take some three times its bytes, and the rule's refusal a copy of that.
    const { assert!(DISC_NAMES.ascii_only) };
    if name.len() > MAX_COMPONENT_CHARS {
        return Err(unsafe_name());
    }
    let text = String::from_utf8_lossy(name);
    check_component(&text, &DISC_NAMES).map_err(|_| unsafe_name())?;
    let joined = match &folder.path {
        Some(path) => format!("{}{SEPARATOR}{text}", path.as_str()),
        None => text.as_ref().to_owned(),
    };
    let path = RelPath::parse(&joined, &DISC_NAMES).map_err(|refused| {
        if refused.rule == NameRule::PathTooLong {
            let detail = taken_or_overlong(&joined);
            MediaError::gamecube(GameCubeRule::PathTooLong, detail)
        } else {
            unsafe_name()
        }
    })?;
    // The name passed the rules of one component, so it is ASCII, and its bytes are its text's.
    if !folder.names.insert(FoldedName(name)) {
        let detail = taken_or_overlong(&joined);
        return Err(MediaError::gamecube(GameCubeRule::DuplicatePath, detail));
    }
    Ok(path)
}

/// Whether `path` is a kept folder or lies under one: its first component is one of them in any
/// ASCII letter case.
fn is_kept(path: &RelPath) -> bool {
    KEPT_ROOTS
        .iter()
        .any(|root| path.first().eq_ignore_ascii_case(root))
}

/// The row at `index` of `table`, or `None` when the table ends before it.
fn row(table: &[u8], index: usize) -> Option<Row> {
    let start = index.checked_mul(FST_ROW_LEN)?;
    let bytes = table.get(start..)?.first_chunk::<FST_ROW_LEN>()?;
    let word = |at: usize| {
        let field = bytes.get(at..)?.first_chunk::<WORD_LEN>()?;
        Some(u32::from_be_bytes(*field))
    };
    Some(Row {
        word: word(0)?,
        offset: word(ROW_OFFSET_AT)?,
        size: word(ROW_SIZE_AT)?,
    })
}

/// The refusal of a table whose root's row is not a root folder's.
fn invalid_root() -> MediaError {
    MediaError::gamecube(GameCubeRule::FstRoot, INVALID_ROOT)
}

/// What the refusal of `path`, taken twice or too long, says of it: the installer's words and the
/// path, or past [`MAX_QUOTED_CHARS`](crate::error::MAX_QUOTED_CHARS) characters its length and
/// its quoted start, so that the refusal never runs on with it.
fn taken_or_overlong(path: &str) -> String {
    match long_start(path) {
        Some((len, start)) => {
            format!("{DUPLICATE_OR_OVERLONG} of {len} characters beginning {start:?}")
        }
        None => format!("{DUPLICATE_OR_OVERLONG}: {path}"),
    }
}

#[cfg(test)]
pub(super) mod tests {
    use gdl_files::MAX_COMPONENT_CHARS;
    use gdl_fixtures::{FstEntry, fst};

    use super::{DiscEntryKind, walk};
    use crate::error::MAX_QUOTED_CHARS;
    use crate::{DISC_SIZE, MAX_FST_LEN, MediaError};

    /// The index of the first row [`game_rows`] gives the entries it is asked for, inside the
    /// `Gauntlet` folder at index 1.
    pub(crate) const EXTRAS_AT: u32 = 6;

    /// How many entries [`game_rows`]' kept trees hold without the entries it is asked for.
    pub(crate) const GAME_ENTRIES: usize = 7;

    /// The rows of the smallest tree the walk takes, as the installer's tests lay theirs out: the
    /// `Gauntlet` folder holding `PDATA` with `WAR.WAD` and `WDATA` with `TOWER.WAD`, then `extras`
    /// from [`EXTRAS_AT`] on, then `carddemo` holding `icon.tpl`. Every file is empty.
    pub(crate) fn game_rows(extras: &[FstEntry]) -> Vec<FstEntry> {
        let extras_len = u32::try_from(extras.len()).unwrap();
        let carddemo = EXTRAS_AT + extras_len;
        let mut rows = vec![
            FstEntry::folder("Gauntlet", 0, carddemo),
            FstEntry::folder("PDATA", 1, 4),
            FstEntry::file("WAR.WAD", 0, 0),
            FstEntry::folder("WDATA", 1, EXTRAS_AT),
            FstEntry::file("TOWER.WAD", 0, 0),
        ];
        rows.extend_from_slice(extras);
        rows.push(FstEntry::folder("carddemo", 0, carddemo + 2));
        rows.push(FstEntry::file("icon.tpl", 0, 0));
        rows
    }

    /// The file table of [`game_rows`].
    pub(crate) fn game_table(extras: &[FstEntry]) -> Vec<u8> {
        fst(&game_rows(extras))
    }

    /// The rule and the words of the walk's refusal of `table`, on a disc of a whole disc's size.
    fn refused(table: &[u8]) -> (&'static str, String) {
        let error = walk(table, DISC_SIZE)
            .err()
            .expect("the walk refuses the table");
        assert!(matches!(error, MediaError::GameCube { .. }), "{error:?}");
        (error.rule(), error.to_string())
    }

    /// The paths the walk keeps of `table`.
    fn kept(table: &[u8]) -> Vec<String> {
        let walked = walk(table, DISC_SIZE).unwrap_or_else(|error| panic!("{error}"));
        walked
            .entries
            .iter()
            .map(|entry| entry.path.as_str().to_owned())
            .collect()
    }

    /// `table` with the big-endian word `value` written at `at`.
    fn with_word(table: &[u8], at: usize, value: u32) -> Vec<u8> {
        let mut changed = table.to_vec();
        changed[at..at + 4].copy_from_slice(&value.to_be_bytes());
        changed
    }

    #[test]
    fn the_smallest_tree_is_kept_in_its_order() {
        let table = game_table(&[]);
        assert_eq!(
            kept(&table),
            [
                "Gauntlet",
                "Gauntlet/PDATA",
                "Gauntlet/PDATA/WAR.WAD",
                "Gauntlet/WDATA",
                "Gauntlet/WDATA/TOWER.WAD",
                "carddemo",
                "carddemo/icon.tpl",
            ]
        );
        let walked = walk(&table, DISC_SIZE).unwrap();
        assert_eq!(walked.entries.len(), GAME_ENTRIES);
        assert_eq!(walked.entries[1].kind, DiscEntryKind::Folder);
        assert_eq!(
            walked.entries[2].kind,
            DiscEntryKind::File { offset: 0, size: 0 }
        );
        assert_eq!(walked.files_len, 0);
    }

    #[test]
    fn the_root_row_is_held_to_the_root_folder() {
        let table = game_table(&[]);
        let root = ("fstRoot", "Invalid GameCube filesystem root".to_owned());
        // A name for the root, a file's kind, a parent, no rows, or more rows than the table
        // holds.
        for (at, value) in [(0, 0x0100_0001), (0, 0), (4, 1), (8, 0), (8, 0x1000_0000)] {
            assert_eq!(refused(&with_word(&table, at, value)), root, "{at} {value}");
        }
        // As many rows as the table has room for, its names read as rows, are walked.
        let rows = u32::try_from(table.len() / 12).unwrap();
        assert_ne!(refused(&with_word(&table, 8, rows)), root);
        assert_eq!(refused(&with_word(&table, 8, rows + 1)), root);
        // A table shorter than the root's row has no root.
        assert_eq!(refused(&table[..11]), root);
        assert_eq!(refused(&[]), root);
        // A root that counts itself alone holds nothing the game needs.
        assert_eq!(refused(&with_word(&table, 8, 1)).0, "missingRoots");
    }

    #[test]
    fn each_row_is_held_to_its_kind_and_its_name() {
        let table = game_table(&[]);
        let entry = ("fstEntry", "Invalid GameCube filesystem entry".to_owned());
        // Kinds other than a file's and a folder's.
        for kind in [2_u8, 0x80, 0xFF] {
            let mut changed = table.clone();
            changed[12] = kind;
            assert_eq!(refused(&changed), entry, "{kind}");
        }
        // A name at or past the table's end, or with no NUL after it.
        let names_at = (GAME_ENTRIES + 1) * 12;
        let past = u32::try_from(table.len() - names_at).unwrap();
        for offset in [past, past + 1, 0x00FF_FFFF] {
            let changed = with_word(&table, 12, 0x0100_0000 | offset);
            assert_eq!(refused(&changed), entry, "{offset}");
        }
        assert_eq!(refused(&table[..table.len() - 1]), entry);
        // The last NUL, read as a name, is an empty name, which no tree may hold.
        let empty = with_word(&table, 12, 0x0100_0000 | (past - 1));
        let said = "Unsafe disc path component: \"\"".to_owned();
        assert_eq!(refused(&empty), ("unsafeName", said));
    }

    #[test]
    fn folders_nest_as_their_rows_say() {
        let nesting = ("nesting", "Invalid filesystem directory nesting".to_owned());
        // PDATA, at index 2, names Gauntlet's row as its parent and ends at 4: another parent,
        // an end at or before itself, or past Gauntlet's end, is no nesting.
        let table = game_table(&[]);
        for (at, value) in [(28, 0), (28, 2), (32, 2), (32, 1), (32, 7)] {
            let changed = with_word(&table, at, value);
            assert_eq!(refused(&changed), nesting, "{at} {value}");
        }
        // A folder that ends where its parent does holds the rest of it.
        let deep = game_table(&[
            FstEntry::folder("SUB", 1, EXTRAS_AT + 2),
            FstEntry::file("X.BIN", 0, 0),
        ]);
        assert!(kept(&deep).contains(&"Gauntlet/SUB/X.BIN".to_owned()));
        // A file lies in the folder whose rows hold it, whatever offset and size it gives.
        let anywhere = game_table(&[FstEntry::file("Y.BIN", 7, 9)]);
        assert!(kept(&anywhere).contains(&"Gauntlet/Y.BIN".to_owned()));
    }

    #[test]
    fn a_name_repeats_only_in_other_folders() {
        // One name in two folders makes two paths.
        let table = game_table(&[
            FstEntry::folder("A", 1, EXTRAS_AT + 2),
            FstEntry::file("X.BIN", 0, 0),
            FstEntry::folder("B", 1, EXTRAS_AT + 4),
            FstEntry::file("X.BIN", 0, 0),
        ]);
        let paths = kept(&table);
        assert!(paths.contains(&"Gauntlet/A/X.BIN".to_owned()));
        assert!(paths.contains(&"Gauntlet/B/X.BIN".to_owned()));
        // In one folder, in any ASCII letter case, it is a path taken twice.
        for second in ["X.BIN", "x.bin", "X.Bin"] {
            let table = game_table(&[FstEntry::file("X.BIN", 0, 0), FstEntry::file(second, 0, 0)]);
            let said = format!("Duplicate or overlong disc path: Gauntlet/{second}");
            assert_eq!(refused(&table), ("duplicatePath", said), "{second}");
        }
        // So is a folder's, where it appears, so nothing under it can repeat a path either.
        let table = game_table(&[
            FstEntry::folder("pdata", 1, EXTRAS_AT + 2),
            FstEntry::file("WAR.WAD", 0, 0),
        ]);
        let said = "Duplicate or overlong disc path: Gauntlet/pdata".to_owned();
        assert_eq!(refused(&table), ("duplicatePath", said));
        // The top of the disc keeps its names apart too, kept or not.
        let mut rows = game_rows(&[]);
        rows.push(FstEntry::file("OTHER", 0, 0));
        rows.push(FstEntry::file("other", 0, 0));
        let said = "Duplicate or overlong disc path: other".to_owned();
        assert_eq!(refused(&fst(&rows)), ("duplicatePath", said));
    }

    #[test]
    fn a_name_of_more_bytes_than_a_component_holds_is_refused_by_its_start() {
        // At the top of the disc a name of as many bytes as a component holds characters is
        // walked, and one of a byte more is refused, quoted whole. So is a name of characters of
        // two bytes, fewer characters than the bound but no ASCII, of as many bytes or one more.
        let at_top = |name: &[u8]| {
            let mut rows = game_rows(&[]);
            rows.push(FstEntry::file("?", 0, 0));
            let mut table = fst(&rows);
            let question = table.len() - 2;
            table.splice(question..=question, name.iter().copied());
            table
        };
        let longest = "n".repeat(MAX_COMPONENT_CHARS);
        assert_eq!(kept(&at_top(longest.as_bytes())).len(), GAME_ENTRIES);
        let wide = "\u{e9}".repeat(MAX_COMPONENT_CHARS / 2);
        for name in [format!("{longest}n"), wide.clone(), format!("{wide}n")] {
            let said = format!("Unsafe disc path component: {name:?}");
            let refusal = refused(&at_top(name.as_bytes()));
            assert_eq!(refusal, ("unsafeName", said), "{}", name.len());
        }
        // A name that runs to the end of a table of the most bytes, of letters or of bytes that
        // are no UTF-8, is told by its length and its start, each such byte one U+FFFD.
        for (filler, shown) in [(b'n', 'n'), (0x80, char::REPLACEMENT_CHARACTER)] {
            let mut table = at_top(b"x");
            let name_at = table.len() - 2;
            table.pop();
            table.resize(MAX_FST_LEN as usize - 1, filler);
            table.push(0);
            assert_eq!(table.len(), MAX_FST_LEN as usize);
            let len = table.len() - 1 - name_at;
            let start: String = ['x']
                .into_iter()
                .chain([shown; MAX_QUOTED_CHARS - 1])
                .collect();
            let said =
                format!("Unsafe disc path component of {len} characters beginning {start:?}");
            assert_eq!(refused(&table), ("unsafeName", said), "{filler}");
        }
    }

    #[test]
    fn a_folder_of_many_rows_keeps_each_name_apart() {
        // A hundred thousand names at the top of the disc beside the kept trees, each its own,
        // are all walked and left out; one taken again in another letter case after them all is
        // still told.
        const MANY: usize = 100_000;
        let mut rows = game_rows(&[]);
        rows.extend((0..MANY).map(|index| FstEntry::file(&format!("F{index:06}"), 0, 0)));
        let walked = walk(&fst(&rows), DISC_SIZE).unwrap_or_else(|error| panic!("{error}"));
        assert_eq!(walked.entries.len(), GAME_ENTRIES);
        rows.push(FstEntry::file("f012345", 0, 0));
        let said = "Duplicate or overlong disc path: f012345".to_owned();
        assert_eq!(refused(&fst(&rows)), ("duplicatePath", said));
    }

    #[test]
    fn only_the_kept_trees_are_kept_but_every_entry_is_judged() {
        // Entries outside Gauntlet and carddemo are walked and left out.
        let mut rows = game_rows(&[]);
        let other_at = u32::try_from(rows.len() + 1).unwrap();
        rows.push(FstEntry::folder("other", 0, other_at + 2));
        rows.push(FstEntry::file("inside.bin", 0, 0));
        rows.push(FstEntry::file("opening.bnr", 0, 4));
        let walked = walk(&fst(&rows), DISC_SIZE).unwrap();
        assert_eq!(walked.entries.len(), GAME_ENTRIES);
        assert_eq!(walked.files_len, 0);
        // Yet each is held to the disc and to the names the tree may hold.
        let mut past = rows.clone();
        past.last_mut().unwrap().offset = u32::try_from(DISC_SIZE).unwrap();
        let said = "Truncated disc file: opening.bnr".to_owned();
        assert_eq!(refused(&fst(&past)), ("truncatedFile", said));
        let mut device = rows.clone();
        device.last_mut().unwrap().name = "AUX.bnr".to_owned();
        let said = "Unsafe disc path component: \"AUX.bnr\"".to_owned();
        assert_eq!(refused(&fst(&device)), ("unsafeName", said));
        // The kept folders and the files the game needs are told in any ASCII letter case, and
        // kept as the disc spells them.
        let mut spelt = game_rows(&[]);
        spelt[0].name = "GAUNTLET".to_owned();
        spelt[1].name = "pData".to_owned();
        spelt[2].name = "war.wad".to_owned();
        spelt[5].name = "CardDemo".to_owned();
        spelt[6].name = "ICON.TPL".to_owned();
        let paths = kept(&fst(&spelt));
        assert_eq!(paths[2], "GAUNTLET/pData/war.wad");
        assert_eq!(paths[6], "CardDemo/ICON.TPL");
    }

    #[test]
    fn a_kept_name_at_the_top_is_kept_only_as_a_folder() {
        // A file whose name only begins as a kept folder's is no kept entry.
        let mut rows = game_rows(&[]);
        rows.push(FstEntry::file("Gauntlet.bak", 0, 0));
        assert_eq!(kept(&fst(&rows)).len(), GAME_ENTRIES);
        // A file of a kept folder's name is refused.
        let mut rows = game_rows(&[]);
        rows[5] = FstEntry::file("carddemo", 0, 0);
        rows.pop();
        let said = "Asset root must be a directory".to_owned();
        assert_eq!(refused(&fst(&rows)), ("rootNotFolder", said));
    }

    #[test]
    fn the_game_needs_both_folders_and_its_three_files() {
        let roots = (
            "missingRoots",
            "Disc is missing Gauntlet or carddemo assets".to_owned(),
        );
        let data = (
            "missingData",
            "Disc is missing required native game data".to_owned(),
        );
        for (index, name, missing) in [
            (0, "Gauntlex", &roots),
            (5, "carddemx", &roots),
            (2, "WAR.WAX", &data),
            (4, "TOWER.WAX", &data),
            (6, "icon.tpx", &data),
            (1, "PDATX", &data),
        ] {
            let mut rows = game_rows(&[]);
            rows[index].name = name.to_owned();
            assert_eq!(&refused(&fst(&rows)), missing, "{name}");
        }
        // A folder of a required file's name is no file.
        let mut rows = game_rows(&[]);
        rows[6] = FstEntry::folder("icon.tpl", 6, 8);
        assert_eq!(refused(&fst(&rows)), data);
    }
}
