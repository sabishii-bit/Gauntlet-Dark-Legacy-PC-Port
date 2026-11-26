//! The headless program built with its test hooks, end to end: the variable that names the fault
//! point it kills itself at, refused before any mode runs when it names none, and killing an update
//! at the pass it names, which a recovery then takes back; the release feed read from a loopback
//! server in place of GitHub's, trusting the tests' own key alone; and updates from such a feed's
//! release, by the installation's launcher copy and by a downloaded launcher outside it, each
//! handing over to the copy it put in place, beside a plan that takes the program's own file when
//! the feed cannot be read; a fresh install of a GameCube disc's image without a window, from
//! the runtime the program's own file carries, killed at each of the install's points and
//! recovered as the next start recovers it; and a checksum list signed with the tests' own key,
//! whose signature the feed then takes as a release's.
#![cfg(feature = "test-hooks")]

mod common;
mod launchers;
mod outside_updates;
mod updates;

use std::collections::BTreeMap;
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};

use gdl_files::Cancel;
use gdl_fixtures::{
    LoopbackServer, ReleaseRow, Reply, Request, RuntimeFiles, TarEntry, append_sections,
    folder_names, icon_disc_receipt, icon_iso, page, sha256_hex, sign_with_test_key, tar_gz,
    tree_entries,
};
use gdl_install::{
    BUILD_INFO, CHECK_DISC, CHECK_GAME, COPY_DISC_FILES, COPY_LAUNCHER, DISC_FILES, FAULT_VARIABLE,
    GET_RUNTIME, JOURNAL_DIR, LOCK_FILE, PERSONALISE, PREPARE, PUBLISH, RECEIPT_FILE,
    RUNTIME_SECTION, STATE_DIR, UPDATE_JOURNAL, VERSION_FILE, WINDOW_SECTION,
    installed_launcher_name, probe_game_version,
};
use gdl_release::{
    CheckedList, Feed, FeedConfig, Platform, REPOSITORY, ReleaseError, Version, checksums_asset,
    launcher_asset, linux_member, signature_asset,
};
use serde_json::Value;

use common::{PROGRAM, VERSION, inspect_json, run_to_end};
use launchers::{
    Launched, OLDER, installation, launcher_file, one_run_at_a_time, past_progress, this_platform,
    waited, write_launcher,
};
use outside_updates::{INSTALLED_COPY, launcher_keeping_the_game};
use updates::{
    BUNDLED_PHASES, RECOVERY_PHASES, assert_only_kept_state, outside_state, phases_of, receipt_of,
};

/// A release later than the program, which a downloaded launcher of the program's own release
/// updates an installation to.
const LATER: &str = "999.0.0";

/// An alpha release, as the feed holds ten, which the launcher passes over.
const ALPHA: &str = "4.0.0-alpha.3";

/// What a launcher copy that carries no runtime holds in its one section, where the window's
/// program goes.
const WINDOW_STAND_IN: &[u8] = b"the window's program";

/// What a release's launcher asset holds where the feed is only read: no launcher file at all,
/// which nothing downloads.
const UNREAD_LAUNCHER: &[u8] = b"a launcher file";

/// When the tests' signatures say they were made, as the release signing writes it.
const SIGNED_AT: &str = "timestamp:1764003600";

/// The status of the redirect GitHub answers a download with.
const FOUND: u16 = 302;

/// Where the loopback feed stores the files its downloads redirect to, under its base.
const STORAGE: &str = "assets/";

/// The header that names the API version a request asks GitHub for.
const API_VERSION_HEADER: &str = "x-github-api-version";

/// The header that names what a request accepts.
const ACCEPT_HEADER: &str = "accept";

/// What a request for a file accepts.
const FILE_ACCEPT: &str = "application/octet-stream";

/// The header that names the program that sends a request.
const USER_AGENT_HEADER: &str = "user-agent";

/// The header an authenticated request carries, which no request to the feed does.
const AUTHORIZATION_HEADER: &str = "authorization";

/// The phases an update from a feed's release goes through, in turn.
const RELEASE_PHASES: [&str; 7] = [
    "checkInstalled",
    "download",
    "verifyDownload",
    "stage",
    "checkGame",
    "swap",
    "cleanup",
];

/// The pass of a fault point the kill test ends an update at: its journal moved to swapping, before
/// the first move.
const SWAP_BEGUN: &str = "update.commit:1";

/// What starts the lines a hold writes as it begins and ends: a killed program's lock may stay
/// held for a moment after it ended, which a run that follows waits out.
const HOLD_LINES: [&str; 2] = ["hold: ", "resumed"];

/// The exit code of a program the fault variable ended at the pass it names.
const KILLED: i32 = 71;

/// The disc image an install installs from, in the folder a test lays out the install's inputs in.
const IMAGE: &str = "game.iso";

/// The folder beside it that holds the launcher file an install runs, outside every installation.
const SOURCE: &str = "source";

/// The folder a test installs into, which the install makes, in the folder it lays the inputs out
/// in.
const GAME: &str = "game";

/// How many passes of the install's publish the kill test tries at most: more than an install of
/// the tests' runtime passes, so that an install still killed past them fails the test rather than
/// keep it going.
const MOST_PUBLISH_PASSES: u32 = 64;

/// What a program wrote to one of its streams, as text.
fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).unwrap()
}

/// The version `text` names.
fn version(text: &str) -> Version {
    Version::parse(text).unwrap_or_else(|error| panic!("{error}"))
}

/// Runs the program at `program` with `args`, and with [`FAULT_VARIABLE`] set to `fault` when one
/// is given, else unset, to its end.
fn run_program<A: AsRef<OsStr>>(program: &Path, fault: Option<&str>, args: &[A]) -> Output {
    let mut command = Command::new(program);
    command.args(args);
    match fault {
        Some(fault) => command.env(FAULT_VARIABLE, fault),
        None => command.env_remove(FAULT_VARIABLE),
    };
    run_to_end(command, program)
}

/// The tree of the installation at `root` outside its state folder once an update from
/// `before`, that tree as an installation of [`OLDER`]'s standard runtime for `platform`, to the
/// standard runtime of `to` left the game in place, its launcher copy holding `copy`: the files
/// that name the release changed, and the copy.
fn updated_tree(
    before: &BTreeMap<String, Option<String>>,
    platform: Platform,
    to: &str,
    copy: &[u8],
) -> BTreeMap<String, Option<String>> {
    let runtime = RuntimeFiles::standard(platform.as_str(), to);
    let version_file = runtime
        .files
        .iter()
        .find(|(name, _)| name == VERSION_FILE)
        .map(|(_, data)| sha256_hex(data))
        .expect("the runtime names its version");
    let mut expected = before.clone();
    expected.insert(VERSION_FILE.to_owned(), Some(version_file));
    expected.insert(
        BUILD_INFO.to_owned(),
        Some(sha256_hex(&runtime.build_info_bytes())),
    );
    expected.insert(
        installed_launcher_name(platform).to_owned(),
        Some(sha256_hex(copy)),
    );
    expected
}

/// What a release's launcher asset for `platform` holds when its launcher file is `launcher`: the
/// file itself on Windows, and on Linux an archive of the one member that is the file.
fn launcher_asset_bytes(platform: Platform, release: &Version, launcher: &[u8]) -> Vec<u8> {
    match platform {
        Platform::WindowsX64 => launcher.to_vec(),
        Platform::LinuxX64 => tar_gz(&[TarEntry::File {
            name: &linux_member(release),
            data: launcher,
        }]),
    }
}

/// A release of the new line as a feed lists it, and what each of its files holds.
struct Published {
    /// The release's row, its launcher asset, its checksum list and any signature in that order.
    row: ReleaseRow,
    /// What each of those files holds, in the same order.
    files: Vec<Vec<u8>>,
}

/// A release feed served from a loopback server, as GitHub serves its releases list and their
/// downloads: each download answered with a redirect to where its file is stored.
struct LoopbackFeed {
    /// The server.
    server: LoopbackServer,
}

impl LoopbackFeed {
    /// A feed that serves nothing yet: every request is answered 404.
    fn start() -> LoopbackFeed {
        LoopbackFeed {
            server: LoopbackServer::start().unwrap(),
        }
    }

    /// The feed's base, which `--feed` names.
    fn base(&self) -> String {
        self.server.base()
    }

    /// The release `release` for `platform` whose launcher asset holds `launcher`, its checksum
    /// list signed by the tests' own key as the release signing signs one when `signed`, as the
    /// feed lists it.
    fn release(
        &self,
        release: &str,
        platform: Platform,
        launcher: &[u8],
        signed: bool,
    ) -> Published {
        let base = self.base();
        let unsigned = ReleaseRow::new_line(&base, release, platform.as_str(), launcher, None);
        let list = unsigned.checksum_list();
        if !signed {
            return Published {
                row: unsigned,
                files: vec![launcher.to_vec(), list],
            };
        }
        let list_name = checksums_asset(&version(release), platform);
        let comment = format!("{SIGNED_AT}\tfile:{list_name}\tversion:{release}");
        let signature = sign_with_test_key(&list, &comment).into_bytes();
        let row = ReleaseRow::new_line(
            &base,
            release,
            platform.as_str(),
            launcher,
            Some(&signature),
        );
        Published {
            row,
            files: vec![launcher.to_vec(), list, signature],
        }
    }

    /// Serves `releases`, then an alpha release, as the list's one page, and every file of each,
    /// its download redirected to where the file is stored.
    fn publish(&self, releases: &[&Published]) {
        let mut rows: Vec<ReleaseRow> =
            releases.iter().map(|release| release.row.clone()).collect();
        rows.push(ReleaseRow::alpha(ALPHA));
        self.server.route(&page_target(), Reply::ok(page(&rows)));
        let base = self.base();
        for release in releases {
            assert_eq!(release.row.assets.len(), release.files.len());
            for (asset, bytes) in release.row.assets.iter().zip(&release.files) {
                let url = asset.url.as_str().expect("a download URL");
                let target = url
                    .strip_prefix(base.trim_end_matches('/'))
                    .unwrap_or_else(|| panic!("{url} is not served from {base}"));
                let stored = format!("{STORAGE}{}", asset.name);
                let to_storage = Reply::redirect(FOUND, &format!("{base}{stored}"));
                self.server.route(target, to_storage);
                self.server
                    .route(&format!("/{stored}"), Reply::ok(bytes.clone()));
            }
        }
    }

    /// Every request the feed has read, in turn.
    fn requests(&self) -> Vec<Request> {
        self.server.requests()
    }
}

/// The target of the releases list's first page, as the feed asks for it.
fn page_target() -> String {
    format!("/repos/{REPOSITORY}/releases?per_page=100&page=1")
}

/// The value of the header `name` that `request` carries, its name's letter case aside.
fn header<'a>(request: &'a Request, name: &str) -> Option<&'a str> {
    request
        .headers
        .iter()
        .find(|(header, _)| header.eq_ignore_ascii_case(name))
        .map(|(_, value)| value.as_str())
}

/// Checks that every request `feed` read was sent as the launcher sends one: as the program of
/// [`VERSION`], with no credentials, the API's version asked for on the list's page alone, and a
/// file's bytes accepted for every other request.
#[track_caller]
fn assert_sent_as_the_launcher_sends(feed: &LoopbackFeed) {
    let agent = format!("GauntletDarkLegacy-Launcher/{VERSION}");
    for request in feed.requests() {
        assert_eq!(request.method, "GET", "{request:?}");
        assert_eq!(
            header(&request, USER_AGENT_HEADER),
            Some(agent.as_str()),
            "{request:?}"
        );
        assert_eq!(header(&request, AUTHORIZATION_HEADER), None, "{request:?}");
        let to_the_api = request.target == page_target();
        assert_eq!(
            header(&request, API_VERSION_HEADER).is_some(),
            to_the_api,
            "{request:?}"
        );
        if !to_the_api {
            assert_eq!(
                header(&request, ACCEPT_HEADER),
                Some(FILE_ACCEPT),
                "{request:?}"
            );
        }
    }
}

#[test]
fn a_fault_variable_that_names_no_pass_is_refused() {
    let program = Path::new(PROGRAM);
    // A point no job passes, no pass, nought, a count that is no decimal, another letter case, and
    // a value that would begin a line of its own, which the refusal writes as its escape.
    for (value, shown) in [
        ("nowhere:1", "nowhere:1"),
        ("update.move", "update.move"),
        ("update.move:0", "update.move:0"),
        ("update.move:x", "update.move:x"),
        ("Update.Move:1", "Update.Move:1"),
        (
            "update.move:1\nphase: stage",
            "update.move:1\\nphase: stage",
        ),
    ] {
        let output = run_program(program, Some(value), &["--version"]);
        assert_eq!(output.status.code(), Some(2), "{value:?}: {output:?}");
        assert_eq!(
            text(&output.stderr),
            format!("error: GDL_LAUNCHER_FAULT names no fault point and pass: {shown}\n"),
            "{value:?}"
        );
        assert_eq!(text(&output.stdout), "", "{value:?}");
    }
    // Arguments that ask for a mode in a form it does not take are refused for the variable
    // first, and as misuse only once the variable names a pass.
    let misused = ["--check-feed", "extra"];
    let output = run_program(program, Some("nowhere:1"), &misused);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    assert_eq!(
        text(&output.stderr),
        "error: GDL_LAUNCHER_FAULT names no fault point and pass: nowhere:1\n",
        "{output:?}"
    );
    assert_eq!(text(&output.stdout), "", "{output:?}");
    let output = run_program(program, Some("update.move:3"), &misused);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    assert!(text(&output.stderr).starts_with("usage: "), "{output:?}");
    assert_eq!(text(&output.stdout), "", "{output:?}");
    // Unset, empty, or naming a pass a mode never reaches, the variable lets the mode run.
    for value in [
        None,
        Some(""),
        Some("update.move:3"),
        Some("launcher.replace:1"),
    ] {
        let output = run_program(program, value, &["--version"]);
        assert_eq!(output.status.code(), Some(0), "{value:?}: {output:?}");
        assert_eq!(text(&output.stdout), format!("{VERSION}\n"), "{value:?}");
        assert_eq!(text(&output.stderr), "", "{value:?}");
    }
}

#[test]
fn a_named_fault_kills_an_update_there_and_a_recovery_takes_it_back() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let fixture = installation(dir.path(), platform, INSTALLED_COPY);
    let root = fixture.root.as_path();
    let before = outside_state(root);
    let receipt = fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap();
    // A launcher file of the program's own release, outside the installation, which hands nothing
    // over.
    let launcher = dir
        .path()
        .join("elsewhere")
        .join(installed_launcher_name(platform));
    write_launcher(&launcher, &launcher_keeping_the_game(platform, VERSION));

    // The update ends itself as its journal moves to swapping, before its first move.
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--offline"),
    ];
    let killed = run_program(&launcher, Some(SWAP_BEGUN), &args);
    assert_eq!(killed.status.code(), Some(KILLED), "{killed:?}");
    // The kill ends the job's thread while lines it sent may still wait to be written, so the
    // phases written are those an update goes through, in turn, up to the swap at most, but may
    // stop short of it: the journal tells where the kill came.
    let through_swap = BUNDLED_PHASES
        .iter()
        .position(|phase| *phase == "swap")
        .unwrap();
    assert!(
        BUNDLED_PHASES[..=through_swap].starts_with(&phases_of(text(&killed.stdout))),
        "{killed:?}"
    );
    let journal = root.join(STATE_DIR).join(JOURNAL_DIR).join(UPDATE_JOURNAL);
    let journal: Value = serde_json::from_slice(&fs::read(journal).unwrap()).unwrap();
    assert_eq!(journal["phase"], "swapping", "{journal}");

    // The next start's recovery takes it back: the installation as it was before.
    let recovered = run_program(
        &launcher,
        None,
        &[OsStr::new("--recover"), root.as_os_str()],
    );
    assert_eq!(recovered.status.code(), Some(0), "{recovered:?}");
    assert_eq!(text(&recovered.stderr), "", "{recovered:?}");
    let out = text(&recovered.stdout);
    assert_eq!(phases_of(out), RECOVERY_PHASES, "{recovered:?}");
    let lines: Vec<&str> = past_progress(out)
        .into_iter()
        .filter(|line| !HOLD_LINES.iter().any(|start| line.starts_with(start)))
        .collect();
    assert_eq!(
        lines,
        ["install: nothing", "update: rolledBack"],
        "{recovered:?}"
    );
    assert!(outside_state(root) == before, "the tree is not as it was");
    assert_eq!(
        fs::read(root.join(STATE_DIR).join(RECEIPT_FILE)).unwrap(),
        receipt
    );
    assert_only_kept_state(root);
}

#[test]
fn check_feed_reads_a_loopback_feed() {
    let Some(platform) = this_platform() else {
        return;
    };
    let feed = LoopbackFeed::start();
    let base = feed.base();
    let signed = feed.release(VERSION, platform, UNREAD_LAUNCHER, true);
    feed.publish(&[&signed]);

    // The release of the program's own version, the alpha release passed over; nothing is
    // downloaded.
    let output = run_program(
        Path::new(PROGRAM),
        None,
        &["--check-feed", "--feed", base.as_str()],
    );
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "", "{output:?}");
    let release = version(VERSION);
    let asset = |index: usize, name: String| {
        let bytes = &signed.files[index];
        format!("{name} {} {}", bytes.len(), sha256_hex(bytes))
    };
    let expected = [
        format!("platform: {}", platform.as_str()),
        format!("running: {VERSION}"),
        "releases: 2".to_owned(),
        "passedOver: 1".to_owned(),
        format!("newest: {VERSION}"),
        "newer: no".to_owned(),
        format!("launcher: {}", asset(0, launcher_asset(&release, platform))),
        format!(
            "checksums: {}",
            asset(1, checksums_asset(&release, platform))
        ),
        format!(
            "signature: {}",
            asset(2, signature_asset(&release, platform))
        ),
    ];
    assert_eq!(text(&output.stdout).lines().collect::<Vec<_>>(), expected);
    let targets: Vec<String> = feed
        .requests()
        .into_iter()
        .map(|request| request.target)
        .collect();
    assert_eq!(targets, [page_target()]);
    assert_sent_as_the_launcher_sends(&feed);

    // The build trusts the tests' own key, so a release without a signature is refused.
    let unsigned = feed.release(VERSION, platform, UNREAD_LAUNCHER, false);
    feed.publish(&[&unsigned]);
    let output = run_program(
        Path::new(PROGRAM),
        None,
        &["--check-feed", "--feed", base.as_str()],
    );
    assert_eq!(output.status.code(), Some(1), "{output:?}");
    assert_eq!(
        text(&output.stderr),
        format!(
            "error: release.incomplete (incomplete): Release asset is incomplete or lacks its \
             GitHub SHA-256 digest: {}\n",
            signature_asset(&release, platform)
        )
    );
    assert_eq!(text(&output.stdout), "", "{output:?}");
}

#[test]
fn an_update_from_a_loopback_feed_brings_its_release() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    // The installation's launcher copy is the program with no runtime of its own, so that the
    // feed's release of the program's own version is newer than anything the copy carries.
    let mut copy = fs::read(PROGRAM).unwrap();
    append_sections(&mut copy, &[(WINDOW_SECTION, WINDOW_STAND_IN)], 0);
    let fixture = installation(dir.path(), platform, &copy);
    let root = fixture.root.as_path();
    let before = outside_state(root);
    let release = version(VERSION);
    let launcher = launcher_keeping_the_game(platform, VERSION);
    let feed = LoopbackFeed::start();
    let published = feed.release(
        VERSION,
        platform,
        &launcher_asset_bytes(platform, &release, &launcher),
        true,
    );
    feed.publish(&[&published]);

    // The installed copy updates its installation from the feed's release, then hands over to the
    // copy that release put in place.
    let base = feed.base();
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--feed"),
        OsStr::new(&base),
    ];
    let mut run = Launched::start(&root.join(installed_launcher_name(platform)), &args);
    let finished = run.finish();
    assert!(finished.status.success(), "{finished:#?}");
    assert_eq!(finished.err, "", "{finished:#?}");
    assert_eq!(phases_of(&finished.out), RELEASE_PHASES, "{finished:#?}");
    let handed_over = finished.handed_over.expect("the update handed over");
    assert_ne!(handed_over, run.pid());
    let lines = past_progress(&finished.out);
    let [updated, probed, retired, handed, previous, swept] = lines[..] else {
        panic!("{finished:#?}");
    };
    assert_eq!(updated, format!("updated: {OLDER} -> {VERSION} (feed)"));
    assert_eq!(probed, "probed: notStaged");
    assert_eq!(handed, format!("handedOver: {handed_over}"));
    assert!(waited(previous).is_some(), "{previous:?}");
    // On Windows the installed copy could not be removed while it ran the update, and the new copy
    // removed it once the old one exited; Linux removes a running program's file.
    if platform == Platform::WindowsX64 {
        assert!(retired.starts_with("retired: left "), "{retired:?}");
        assert_eq!(swept, "retired: removed 1, kept 0");
    } else {
        assert_eq!(retired, "retired: removed");
        assert_eq!(swept, "retired: removed 0, kept 0");
    }

    // The release's files in place, its launcher file the installation's copy, and nothing the
    // update downloaded, staged or moved aside left.
    assert!(
        outside_state(root) == updated_tree(&before, platform, VERSION, &launcher),
        "the tree is not the release's"
    );
    let receipt = receipt_of(root);
    assert_eq!(receipt["version"], VERSION);
    assert_eq!(receipt["launcher"]["sha256"], sha256_hex(&launcher));
    assert_only_kept_state(root);
    let report = inspect_json(root);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["installation"]["version"], VERSION, "{report}");

    // The list's one page, then the release's checksum list, its signature and its launcher file,
    // each through the redirect a download answers.
    let targets: Vec<String> = feed
        .requests()
        .into_iter()
        .map(|request| request.target)
        .collect();
    let mut downloads: Vec<String> = published
        .row
        .assets
        .iter()
        .flat_map(|asset| {
            let url = asset.url.as_str().expect("a download URL");
            let target = url.strip_prefix(base.trim_end_matches('/')).unwrap();
            [target.to_owned(), format!("/{STORAGE}{}", asset.name)]
        })
        .collect();
    assert_eq!(targets.first(), Some(&page_target()), "{targets:#?}");
    let mut fetched = targets[1..].to_vec();
    fetched.sort();
    downloads.sort();
    assert_eq!(fetched, downloads);
    assert_sent_as_the_launcher_sends(&feed);
}

#[test]
fn a_downloaded_launcher_that_updates_the_installation_hands_over_to_the_new_copy() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let fixture = installation(dir.path(), platform, INSTALLED_COPY);
    let root = fixture.root.as_path();
    let before = outside_state(root);
    // The launcher file a player downloaded, of the program's own release, outside the
    // installation, under the name the release gives it.
    let downloaded_name = match platform {
        Platform::WindowsX64 => launcher_asset(&version(VERSION), platform),
        Platform::LinuxX64 => linux_member(&version(VERSION)),
    };
    let downloaded = dir.path().join("downloads").join(downloaded_name);
    let downloaded_bytes = launcher_keeping_the_game(platform, VERSION);
    write_launcher(&downloaded, &downloaded_bytes);
    // The feed's release is later than that file.
    let later = version(LATER);
    let launcher = launcher_keeping_the_game(platform, LATER);
    let feed = LoopbackFeed::start();
    let published = feed.release(
        LATER,
        platform,
        &launcher_asset_bytes(platform, &later, &launcher),
        true,
    );
    feed.publish(&[&published]);

    // The downloaded file updates the installation from the feed's release, after which it is
    // older than the installation, and so hands over to the copy that release put in place.
    let base = feed.base();
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--feed"),
        OsStr::new(&base),
    ];
    let mut run = Launched::start(&downloaded, &args);
    let finished = run.finish();
    assert!(finished.status.success(), "{finished:#?}");
    assert_eq!(phases_of(&finished.out), RELEASE_PHASES, "{finished:#?}");
    let handed_over = finished.handed_over.expect("the update handed over");
    assert_ne!(handed_over, run.pid());
    let lines = past_progress(&finished.out);
    let [updated, probed, retired, handed, previous] = lines[..] else {
        panic!("{finished:#?}");
    };
    assert_eq!(updated, format!("updated: {OLDER} -> {LATER} (feed)"));
    assert_eq!(probed, "probed: notStaged");
    // No launcher ran from the copy installed before, so the update removed it.
    assert_eq!(retired, "retired: removed");
    assert_eq!(handed, format!("handedOver: {handed_over}"));
    // The new copy waited for the downloaded file to exit. The one program these tests have is
    // the copy the release put in place, and it is of an earlier release than the one it carries:
    // so, as every launcher older than an installation refuses to change it, it then refuses to
    // sweep the installation's retired copies, of which the update left none. A real release's
    // copy is of that release.
    assert!(waited(previous).is_some(), "{previous:?}");
    assert_eq!(
        finished.err,
        format!(
            "error: launcher.older (olderLauncher): This launcher file ({VERSION}) is older than \
             the installation ({LATER})\n"
        ),
        "{finished:#?}"
    );

    // The release's files in place and its launcher file the installation's copy; the file the
    // player ran is as it was.
    assert!(
        outside_state(root) == updated_tree(&before, platform, LATER, &launcher),
        "the tree is not the release's"
    );
    assert_eq!(receipt_of(root)["version"], LATER);
    assert_only_kept_state(root);
    assert!(
        fs::read(&downloaded).unwrap() == downloaded_bytes,
        "the downloaded file changed"
    );
    assert_sent_as_the_launcher_sends(&feed);
}

#[test]
fn a_plan_whose_feed_cannot_be_read_comes_from_the_programs_own_file() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let fixture = installation(dir.path(), platform, INSTALLED_COPY);
    let root = fixture.root.as_path();
    let before = tree_entries(root);
    let launcher = dir
        .path()
        .join("elsewhere")
        .join(installed_launcher_name(platform));
    let (_, file) = launcher_file(platform, VERSION);
    write_launcher(&launcher, &file);
    // A feed that serves nothing, whose list's page is not found.
    let feed = LoopbackFeed::start();
    let base = feed.base();

    // The plan tells why the feed could not be read, then plans the update from the program's own
    // file, which brings a newer release and a new game, changing nothing.
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--feed"),
        OsStr::new(&base),
    ];
    let output = run_program(&launcher, None, &args);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "", "{output:?}");
    let list = format!("{}{}", base.trim_end_matches('/'), page_target());
    let expected = [
        format!("feed: net.failed (status): GitHub answered HTTP 404 for {list}"),
        format!("plan: {OLDER} -> {VERSION} (bundled)"),
        format!("plan: replace {VERSION_FILE}"),
        format!("plan: replace {BUILD_INFO}"),
        format!("plan: replace {}", platform.executable()),
    ];
    assert_eq!(text(&output.stdout).lines().collect::<Vec<_>>(), expected);
    assert!(
        tree_entries(root) == before,
        "the plan changed the installation"
    );
    let targets: Vec<String> = feed
        .requests()
        .into_iter()
        .map(|request| request.target)
        .collect();
    assert_eq!(targets, [page_target()]);
}

/// What an install without a window installs from, in a folder of the test's outside every
/// installation: the icon disc's image, and a launcher file of the program's own carrying the
/// standard runtime of the program's release, with a game that answers the game's version probe.
struct InstallInputs {
    /// The disc image.
    image: PathBuf,
    /// The launcher file, which the install runs.
    launcher: PathBuf,
    /// The runtime it carries.
    runtime: RuntimeFiles,
    /// Its bytes.
    bytes: Vec<u8>,
}

impl InstallInputs {
    /// The inputs of an install for `platform`, written into `dir`, the runtime's executable
    /// holding `game`. The executable is stored in the runtime's archive rather than deflated, so
    /// that a program's megabytes build fast; the release record names its bytes.
    fn new(dir: &Path, platform: Platform, game: &[u8]) -> InstallInputs {
        let image = dir.join(IMAGE);
        fs::write(&image, icon_iso()).unwrap();
        let mut runtime = RuntimeFiles::standard(platform.as_str(), VERSION);
        let zip = runtime.zip_with_program(game);
        let mut bytes = fs::read(PROGRAM).unwrap();
        append_sections(&mut bytes, &[(RUNTIME_SECTION, &zip)], 0);
        let launcher = dir.join(SOURCE).join(installed_launcher_name(platform));
        write_launcher(&launcher, &bytes);
        InstallInputs {
            image,
            launcher,
            runtime,
            bytes,
        }
    }

    /// Runs the launcher file's install of the image into `destination`, killed at the pass
    /// `fault` names when one is given, to its end.
    fn install(&self, destination: &Path, fault: Option<&str>) -> Output {
        let args = [
            OsStr::new("--install"),
            self.image.as_os_str(),
            destination.as_os_str(),
        ];
        run_program(&self.launcher, fault, &args)
    }

    /// Runs the launcher file's recovery of the folder `destination`, as the next start runs one,
    /// to its end.
    fn recover(&self, destination: &Path) -> Output {
        let args = [OsStr::new("--recover"), destination.as_os_str()];
        run_program(&self.launcher, None, &args)
    }

    /// Checks that the image and the launcher file are as the test wrote them: an install only
    /// reads them.
    #[track_caller]
    fn assert_untouched(&self) {
        assert!(
            fs::read(&self.image).unwrap() == icon_iso(),
            "the image changed"
        );
        assert!(
            fs::read(&self.launcher).unwrap() == self.bytes,
            "the launcher file changed"
        );
    }
}

/// The phases an install for `platform` goes through, in turn: on Windows the disc's icon is set
/// into the executable, a phase of its own.
fn install_phases(platform: Platform) -> Vec<&'static str> {
    let mut phases = vec![CHECK_DISC, PREPARE, GET_RUNTIME, COPY_DISC_FILES];
    if platform == Platform::WindowsX64 {
        phases.push(PERSONALISE);
    }
    phases.extend([COPY_LAUNCHER, CHECK_GAME, PUBLISH]);
    phases.iter().map(|phase| phase.0).collect()
}

/// The line an install into `destination` that stands ends with.
fn installed_line(destination: &Path) -> String {
    format!("installed: {VERSION} into {}", destination.display())
}

/// The game the kill test's installs carry for `platform`: on Windows the program, since an install
/// sets the disc's icon into the executable, which only a program takes; on Linux a script that
/// answers the game's version probe as the program does, so that each of the test's many installs
/// copies and hashes a few bytes for the game rather than the program's megabytes.
fn kill_test_game(platform: Platform) -> Vec<u8> {
    match platform {
        Platform::WindowsX64 => fs::read(PROGRAM).unwrap(),
        Platform::LinuxX64 => format!("#!/bin/sh\necho {VERSION}\n").into_bytes(),
    }
}

/// What a recovery of an install a kill cut short leaves of the folder the install made.
#[derive(Clone, Copy, Debug)]
enum Left {
    /// The folder, empty: the state folder was given up before a journal said who made the folder.
    Empty,
    /// Nothing: the install was taken back with the folder it made.
    Nothing,
    /// The installation an install nothing stops leaves.
    Whole,
}

/// Checks that the install of `inputs` into the new folder `destination`, which the pass `fault`
/// names ended as `killed` tells, was killed there and left a job to recover; then recovers the
/// folder as the next start does, and checks that the recovery reports `outcome` for the install
/// and nothing for an update, and leaves `left`, `whole` being what an install nothing stops
/// leaves.
#[track_caller]
fn assert_killed_and_recovered(
    inputs: &InstallInputs,
    destination: &Path,
    (fault, killed): (&str, &Output),
    (outcome, left): (&str, Left),
    whole: &BTreeMap<String, Option<String>>,
) {
    assert_eq!(killed.status.code(), Some(KILLED), "{fault}: {killed:?}");
    assert_eq!(text(&killed.stderr), "", "{fault}: {killed:?}");
    assert!(
        !text(&killed.stdout).contains("installed: "),
        "{fault}: {killed:?}"
    );
    let report = inspect_json(destination);
    assert_eq!(report["state"], "pending", "{fault}: {report}");

    let recovered = inputs.recover(destination);
    assert_eq!(recovered.status.code(), Some(0), "{fault}: {recovered:?}");
    assert_eq!(text(&recovered.stderr), "", "{fault}: {recovered:?}");
    let lines: Vec<&str> = past_progress(text(&recovered.stdout))
        .into_iter()
        .filter(|line| !HOLD_LINES.iter().any(|start| line.starts_with(start)))
        .collect();
    assert_eq!(
        lines,
        [format!("install: {outcome}").as_str(), "update: nothing"],
        "{fault}: {recovered:?}"
    );
    match left {
        Left::Empty => {
            let names = folder_names(destination).unwrap();
            assert!(names.is_empty(), "{fault}: {names:?}");
        }
        Left::Nothing => assert!(!destination.exists(), "{fault}: the folder is left"),
        Left::Whole => {
            assert!(
                tree_entries(destination) == *whole,
                "{fault}: the tree is not an install's"
            );
            let report = inspect_json(destination);
            assert_eq!(report["state"], "installed", "{fault}: {report}");
        }
    }
}

#[test]
fn install_installs_the_image_with_this_file() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let inputs = InstallInputs::new(dir.path(), platform, &fs::read(PROGRAM).unwrap());
    let destination = dir.path().join(GAME);

    // The install's phases in turn, then the release it installed and where.
    let output = inputs.install(&destination, None);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "", "{output:?}");
    let out = text(&output.stdout);
    assert_eq!(phases_of(out), install_phases(platform), "{output:?}");
    assert_eq!(past_progress(out), [installed_line(&destination)]);

    // The installation stands, of the program's release, its launcher copy this file and its
    // receipt the one an install of the disc writes; the state folder holds what an installation
    // keeps once its jobs are done.
    let report = inspect_json(&destination);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["journals"], Value::Array(Vec::new()), "{report}");
    assert_eq!(report["installation"]["version"], VERSION, "{report}");
    assert_eq!(report["installation"]["relation"], "same", "{report}");
    let copy = fs::read(destination.join(installed_launcher_name(platform))).unwrap();
    assert!(copy == inputs.bytes, "the launcher copy is not this file");
    drop(copy);
    let executable = destination.join(platform.executable());
    let state = destination.join(STATE_DIR);
    assert_eq!(
        fs::read_to_string(state.join(RECEIPT_FILE)).unwrap(),
        icon_disc_receipt(
            &inputs.runtime,
            &inputs.bytes,
            &fs::read(&executable).unwrap()
        )
    );
    assert_eq!(
        folder_names(&state).unwrap(),
        [DISC_FILES, JOURNAL_DIR, LOCK_FILE, RECEIPT_FILE]
    );
    assert!(folder_names(&state.join(JOURNAL_DIR)).unwrap().is_empty());

    // The installed game answers with the release's version, and the inputs are as they were.
    let probe = probe_game_version(&executable).unwrap();
    assert!(probe.succeeded(), "{probe:?}");
    assert_eq!(probe.first_line.as_deref(), Some(VERSION), "{probe:?}");
    inputs.assert_untouched();
}

#[test]
fn an_install_killed_at_each_install_point_recovers() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let inputs = InstallInputs::new(dir.path(), platform, &kill_test_game(platform));
    // What an install nothing stops leaves, which a finished recovery must leave too: its game
    // answered the probe, so the install met nothing to tell.
    let reference = dir.path().join(GAME);
    let output = inputs.install(&reference, None);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    let out = text(&output.stdout);
    assert_eq!(
        past_progress(out),
        [installed_line(&reference)],
        "{output:?}"
    );
    let whole = tree_entries(&reference);

    // The first pass of each point: the state folder made, given up and left empty with nothing
    // to say who made the folder; the runtime staged, taken back; the journal committed before the
    // staging went, cleaned up.
    for (fault, recovered) in [
        ("install.prepare:1", ("abandonedRemoved", Left::Empty)),
        ("install.stage:1", ("rolledBack", Left::Nothing)),
        ("install.cleanup:1", ("cleanedUp", Left::Whole)),
    ] {
        let destination = dir.path().join(fault.replace([':', '.'], "-"));
        let killed = inputs.install(&destination, Some(fault));
        assert_killed_and_recovered(&inputs, &destination, (fault, &killed), recovered, &whole);
    }

    // Every pass of the publish, until the install runs past the last: the journal moved to
    // publishing, each root put in place, every root recorded and the receipt written, each
    // finished by the recovery.
    let mut at = 1;
    loop {
        let fault = format!("install.publish:{at}");
        let destination = dir.path().join(format!("publish-{at}"));
        let ran = inputs.install(&destination, Some(&fault));
        if ran.status.code() == Some(0) {
            let out = text(&ran.stdout);
            assert_eq!(
                past_progress(out),
                [installed_line(&destination)],
                "{fault}"
            );
            assert!(
                tree_entries(&destination) == whole,
                "{fault}: the tree is not an install's"
            );
            break;
        }
        let finished = ("finished", Left::Whole);
        assert_killed_and_recovered(&inputs, &destination, (&fault, &ran), finished, &whole);
        at += 1;
        assert!(
            at <= MOST_PUBLISH_PASSES,
            "the publish never ran to its end"
        );
    }
    // At least the journal, a root, the roots recorded and the receipt.
    assert!(at > 4, "the publish passed {} times", at - 1);
    inputs.assert_untouched();
}

/// Runs the program's signing of the checksum list at `list` for the release `release`, to its end.
fn sign_test(list: &Path, release: &str) -> Output {
    let args = [
        OsStr::new("--sign-test"),
        list.as_os_str(),
        OsStr::new(release),
    ];
    run_program(Path::new(PROGRAM), None, &args)
}

/// The checksum list of `platform`'s newest release on the loopback feed at `base`, fetched and
/// checked as an update fetches and checks one, by a launcher of the program's version trusting
/// the keys a build with the test hooks trusts.
fn checked_list(base: &str, platform: Platform) -> Result<CheckedList, ReleaseError> {
    let config = FeedConfig::loopback(base, VERSION).unwrap_or_else(|error| panic!("{error}"));
    let feed = Feed::new(config);
    let cancel = Cancel::default();
    let scan = feed.scan(None, platform, &cancel)?;
    let release = scan.candidate.expect("the feed holds a release");
    feed.check_list(&release, &cancel)
}

#[test]
fn sign_test_writes_a_signature_the_feed_accepts() {
    let Some(platform) = this_platform() else {
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let feed = LoopbackFeed::start();
    let base = feed.base();
    let later = version(LATER);
    // A release's checksum list, as the release writes it, under the name the release gives it.
    let list = ReleaseRow::new_line(&base, LATER, platform.as_str(), UNREAD_LAUNCHER, None)
        .checksum_list();
    let list_name = checksums_asset(&later, platform);
    let list_path = dir.path().join(&list_name);
    fs::write(&list_path, &list).unwrap();

    // The program signs it into a new file beside it, under the name the release gives the
    // signature, and leaves the list as it was.
    let output = sign_test(&list_path, LATER);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    assert_eq!(text(&output.stderr), "", "{output:?}");
    let signature_path = dir.path().join(signature_asset(&later, platform));
    assert_eq!(
        text(&output.stdout),
        format!("signed: {}\n", signature_path.display())
    );
    assert!(fs::read(&list_path).unwrap() == list, "the list changed");
    let signature = fs::read(&signature_path).unwrap();

    // The feed takes a release that carries the list and that signature: it fetches both, the
    // list then its signature, each through the redirect a download answers, and nothing of the
    // launcher file; the signature verifies by the key this build trusts, and the list gives the
    // launcher file the digest GitHub gives it.
    let row = ReleaseRow::new_line(
        &base,
        LATER,
        platform.as_str(),
        UNREAD_LAUNCHER,
        Some(&signature),
    );
    let signed = Published {
        row,
        files: vec![UNREAD_LAUNCHER.to_vec(), list.clone(), signature.clone()],
    };
    feed.publish(&[&signed]);
    let checked = checked_list(&base, platform).unwrap_or_else(|error| panic!("{error}"));
    assert!(checked.signed());
    assert!(
        checked.bytes == list,
        "the list checked is not the list signed"
    );
    assert!(
        checked.signature.as_deref() == Some(signature.as_slice()),
        "the signature checked is not the one the program wrote"
    );
    let targets: Vec<String> = feed
        .requests()
        .into_iter()
        .map(|request| request.target)
        .collect();
    let mut expected = vec![page_target()];
    for asset in &signed.row.assets[1..] {
        let url = asset.url.as_str().expect("a download URL");
        let target = url.strip_prefix(base.trim_end_matches('/')).unwrap();
        expected.extend([target.to_owned(), format!("/{STORAGE}{}", asset.name)]);
    }
    assert_eq!(targets, expected);
    assert_sent_as_the_launcher_sends(&feed);

    // A signature the program made of the same list for another release is refused for this
    // one, since what it was made for is part of what is signed.
    let elsewhere = dir.path().join("elsewhere");
    fs::create_dir(&elsewhere).unwrap();
    let other_list = elsewhere.join(&list_name);
    fs::write(&other_list, &list).unwrap();
    let output = sign_test(&other_list, VERSION);
    assert_eq!(output.status.code(), Some(0), "{output:?}");
    let other_signature = fs::read(elsewhere.join(signature_asset(&later, platform))).unwrap();
    let row = ReleaseRow::new_line(
        &base,
        LATER,
        platform.as_str(),
        UNREAD_LAUNCHER,
        Some(&other_signature),
    );
    feed.publish(&[&Published {
        row,
        files: vec![UNREAD_LAUNCHER.to_vec(), list, other_signature],
    }]);
    let refused = checked_list(&base, platform).unwrap_err();
    assert_eq!(refused.rule(), "signature", "{refused:?}");
    assert_eq!(
        refused.to_string(),
        format!(
            "The release's checksum list is not signed as this launcher requires: its trusted \
             comment gives the version \"{VERSION}\", not {LATER}"
        )
    );
}
