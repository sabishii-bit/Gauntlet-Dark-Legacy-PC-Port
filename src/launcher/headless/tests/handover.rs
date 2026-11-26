//! The new copy's side of a hand-over through the built headless program: `--after-update` waits
//! for a launcher that still runs rather than going on, and refuses as misuse a process id that
//! is not a number of one or more, and its own process's.

mod common;
mod launchers;
mod outside_updates;

use std::env::consts::EXE_SUFFIX;
use std::ffi::OsStr;
use std::path::Path;
use std::process::{Command, Output};
use std::thread;
use std::time::{Duration, Instant};

use gdl_install::{InstallFolder, installed_launcher_name};

use common::{PROGRAM, VERSION, inspect_json, run_to_end};
use launchers::{
    Launched, Next, OLDER, Stream, installation, launcher_file, one_run_at_a_time, past_progress,
    this_platform, waited, write_launcher,
};
use outside_updates::{INSTALLED_COPY, launcher_keeping_the_game};

/// How long a waiter is started after the launcher it waits for wrote that it runs: more than two
/// clock ticks of either system, so that the two never share a creation time.
const START_TICK_GAP: Duration = Duration::from_millis(50);

/// How long a new copy started while the old launcher still runs is watched for writing nothing.
const QUIET: Duration = Duration::from_secs(1);

/// The line the old launcher writes once another job's lock keeps it waiting.
const LOCK_HOLD: &str = "hold: lock.busy";

/// The test a child of this test program runs alone to become the headless program, asked to
/// wait for its own process.
#[cfg(unix)]
const OWN_WAITER: &str = "a_child_becomes_the_program_asked_to_wait_for_itself";

/// The variable that makes a child of this test program running [`OWN_WAITER`] become the
/// headless program. Unset, as in a run of the tests, that test does nothing.
#[cfg(unix)]
const OWN_ID: &str = "GDL_HEADLESS_OWN_ID";

/// What a program wrote to one of its streams, as text.
fn text(bytes: &[u8]) -> &str {
    std::str::from_utf8(bytes).unwrap()
}

/// Runs the headless program cargo built with `args`, nothing to read and without a console
/// window, to its end.
fn run_program<A: AsRef<OsStr>>(args: &[A]) -> Output {
    let mut command = Command::new(PROGRAM);
    command.args(args);
    run_to_end(command, Path::new(PROGRAM))
}

/// The line the program answers arguments it does not take with: what it writes when it is given
/// none, which asks for no mode at all.
fn usage() -> String {
    let output = run_program::<&str>(&[]);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    let usage = text(&output.stderr).to_owned();
    assert!(usage.starts_with("usage: "), "{usage:?}");
    usage
}

/// The next line `launched` writes on its output that begins with `start`, waited for until the
/// run's deadline, past which the run fails.
fn line_starting(launched: &mut Launched, start: &str) -> String {
    loop {
        match launched.next_until(Instant::now() + QUIET) {
            Next::Line(Stream::Out, line) if line.starts_with(start) => return line,
            Next::Line(..) | Next::Quiet => {}
            Next::Ended => panic!("the run ended without a line {start:?}"),
        }
    }
}

#[test]
fn a_new_copy_started_while_the_old_one_lives_waits_for_it() {
    let Some(platform) = this_platform() else {
        return;
    };
    let _one = one_run_at_a_time();
    let dir = tempfile::tempdir().unwrap();
    let (_, launcher) = launcher_file(platform, VERSION);
    let fixture = installation(dir.path(), platform, INSTALLED_COPY);
    let root = fixture.root.as_path();
    // The old launcher, a launcher file outside the installation, which updates it and hands
    // nothing over, since it is not the copy the update replaces and is as new as what it
    // installs; and the new copy, a launcher copy in a folder of its own that holds no
    // installation, whose sweep so finds nothing, run once first so that a scanner's first look
    // at it is over before it is watched. The old launcher's update leaves the game as it is, so
    // that once the lock is let go it is done in moments: the new copy waits for it no longer
    // than its wait's limit on a slow machine too.
    let old_file = dir
        .path()
        .join("downloads")
        .join(format!("gdl-launcher-headless{EXE_SUFFIX}"));
    write_launcher(&old_file, &launcher_keeping_the_game(platform, VERSION));
    let new_file = dir
        .path()
        .join("elsewhere")
        .join(installed_launcher_name(platform));
    write_launcher(&new_file, &launcher);
    let mut warm = Command::new(&new_file);
    warm.arg("--version");
    let warmed = run_to_end(warm, &new_file);
    assert_eq!(text(&warmed.stdout), format!("{VERSION}\n"), "{warmed:?}");

    // Another job's lock keeps the old launcher running, waiting for it.
    let lock = InstallFolder::at(root).unwrap().lock().unwrap();
    let args = [
        OsStr::new("--update"),
        root.as_os_str(),
        OsStr::new("--yes"),
        OsStr::new("--offline"),
    ];
    let mut old = Launched::start(&old_file, &args);
    line_starting(&mut old, LOCK_HOLD);
    thread::sleep(START_TICK_GAP);
    let old_pid = old.pid().to_string();
    let mut new = Launched::start(
        &new_file,
        &[OsStr::new("--after-update"), OsStr::new(&old_pid)],
    );
    // It waits, writing nothing and running on, rather than going on as though the old one were
    // gone.
    assert_eq!(new.next_until(Instant::now() + QUIET), Next::Quiet);

    // Once the lock is let go the old launcher updates and exits, and the new copy goes on.
    drop(lock);
    let old_finished = old.finish();
    assert!(old_finished.status.success(), "{old_finished:#?}");
    assert_eq!(old_finished.err, "", "{old_finished:#?}");
    assert_eq!(old_finished.handed_over, None, "{old_finished:#?}");
    let updated = format!("updated: {OLDER} -> {VERSION} (bundled)");
    assert_eq!(
        past_progress(&old_finished.out),
        [
            LOCK_HOLD,
            "resumed",
            updated.as_str(),
            "probed: notStaged",
            "retired: removed"
        ],
        "{old_finished:#?}"
    );
    let new_finished = new.finish();
    assert!(new_finished.status.success(), "{new_finished:#?}");
    assert_eq!(new_finished.err, "", "{new_finished:#?}");
    assert_eq!(new_finished.handed_over, None, "{new_finished:#?}");
    let lines = past_progress(&new_finished.out);
    let [previous, swept] = lines[..] else {
        panic!("{new_finished:#?}");
    };
    // It waited from before the quiet second until the old launcher exited.
    let quiet = u64::try_from(QUIET.as_millis()).unwrap();
    assert!(
        waited(previous).is_some_and(|ms| ms >= quiet),
        "{previous:?}"
    );
    assert_eq!(swept, "retired: removed 0, kept 0");
    let report = inspect_json(root);
    assert_eq!(report["state"], "installed", "{report}");
    assert_eq!(report["installation"]["version"], VERSION, "{report}");
}

/// The child's part of [`an_after_update_of_its_own_pid_is_a_usage_error`], which does nothing
/// unless [`OWN_ID`] is set: the headless program takes this process's place, and so its id,
/// which it is asked to wait for.
#[cfg(unix)]
#[test]
fn a_child_becomes_the_program_asked_to_wait_for_itself() {
    use std::os::unix::process::CommandExt;

    if std::env::var_os(OWN_ID).is_none() {
        return;
    }
    let refused = Command::new(PROGRAM)
        .arg("--after-update")
        .arg(std::process::id().to_string())
        .exec();
    panic!("{PROGRAM}: {refused}");
}

/// A process can be given its own id only once it runs, which Linux allows by a program taking
/// another's place in its process; Windows starts every program in a process of its own, so there
/// the rule is held in the session's own tests alone.
#[cfg(unix)]
#[test]
fn an_after_update_of_its_own_pid_is_a_usage_error() {
    let this = std::env::current_exe().unwrap();
    let mut command = Command::new(&this);
    command
        .args(["--exact", OWN_WAITER, "--test-threads", "1"])
        .env(OWN_ID, "1");
    let output = run_to_end(command, &this);
    assert_eq!(output.status.code(), Some(2), "{output:?}");
    assert_eq!(text(&output.stderr), usage());
    // Nothing but the test harness's own lines, written before the program took its place.
    for line in text(&output.stdout).lines() {
        assert!(
            line.is_empty() || line.starts_with("running ") || line.starts_with("test "),
            "{output:?}"
        );
    }
}

#[test]
fn an_after_update_of_no_number_is_a_usage_error() {
    let usage = usage();
    // A sign, spaces, nought, other digits than ASCII's, other notations, and numbers past what a
    // process id holds.
    for id in [
        "abc",
        "-1",
        "+1",
        "0",
        "00",
        " 1",
        "1 ",
        "1.0",
        "1e3",
        "0x10",
        "\u{0661}",
        "4294967296",
        "99999999999999999999",
    ] {
        let output = run_program(&["--after-update", id]);
        assert_eq!(output.status.code(), Some(2), "{id:?}: {output:?}");
        assert_eq!(text(&output.stderr), usage, "{id:?}");
        assert_eq!(text(&output.stdout), "", "{id:?}");
    }
}
