"""The launcher script runs cargo where the workspace's own settings apply, and builds outside src."""
import json
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

SCRIPTS = pathlib.Path(__file__).resolve().parents[2] / "scripts"
sys.path.insert(0, str(SCRIPTS))
import launcher  # noqa: E402
import process_tree  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKSPACE = ROOT / "src" / "launcher"


def listed(pid, expected, seconds=5):
    """Whether the process table lists `pid`, polled until it matches `expected`."""
    deadline = time.monotonic() + seconds
    while True:
        present = pid in process_tree.table()
        if present == expected or time.monotonic() >= deadline:
            return present
        time.sleep(0.1)


def locked_dependencies(package):
    """The names of the dependencies the workspace's Cargo.lock lists under each locked version of
    `package`, without the version cargo adds to a name while two versions of it are locked."""
    lock = (WORKSPACE / "Cargo.lock").read_text(encoding="utf-8")
    found = []
    for block in re.split(r"^\[\[package\]\]$", lock, flags=re.MULTILINE)[1:]:
        if re.search(r'^name = "(.*)"$', block, re.MULTILINE).group(1) == package:
            dependencies = re.search(r"^dependencies = \[$(.*?)^\]$", block,
                                     re.MULTILINE | re.DOTALL)
            found.append(re.findall(r'^ "([^ "]+)', dependencies.group(1), re.MULTILINE)
                         if dependencies else [])
    return found


class PlanTests(unittest.TestCase):
    def test_check_formats_lints_and_tests_the_whole_workspace(self):
        self.assertEqual(launcher.plan("check"), [
            ["cargo", "fmt", "--all", "--check"],
            ["cargo", "clippy", "--locked", "--workspace", "--all-targets", "--", "-D", "warnings"],
            ["cargo", "test", "--locked", "--workspace"],
        ])

    def test_check_of_one_crate_stays_within_it(self):
        self.assertEqual(launcher.plan("check", packages=["gdl-files"]), [
            ["cargo", "fmt", "--package", "gdl-files", "--check"],
            ["cargo", "clippy", "--locked", "--package", "gdl-files", "--all-targets", "--",
             "-D", "warnings"],
            ["cargo", "test", "--locked", "--package", "gdl-files"],
        ])

    def test_check_of_several_crates_names_each(self):
        named = ["--package", "gdl-files", "--package", "gdl-media"]
        self.assertEqual(launcher.plan("check", packages=["gdl-files", "gdl-media"]), [
            ["cargo", "fmt", *named, "--check"],
            ["cargo", "clippy", "--locked", *named, "--all-targets", "--", "-D", "warnings"],
            ["cargo", "test", "--locked", *named],
        ])

    def test_build_and_run_take_the_lock_as_it_is(self):
        self.assertEqual(launcher.plan("build", release=True),
                         [["cargo", "build", "--locked", "--workspace", "--release"]])
        self.assertEqual(launcher.plan("run", arguments=["--check-window"]),
                         [["cargo", "run", "--locked", "--package", "gdl-launcher", "--",
                           "--check-window"]])

    def test_an_unknown_command_is_refused(self):
        with self.assertRaises(ValueError):
            launcher.plan("extract")


class EnvironmentTests(unittest.TestCase):
    def environment(self, path, rust_bin):
        with mock.patch.object(launcher.devenv, "WINDOWS", True), \
                mock.patch.object(launcher.devenv, "environment", return_value={"PATH": path}), \
                mock.patch.object(launcher.devenv, "rust_bin", return_value=rust_bin):
            return launcher.environment()

    def test_cargo_target_dir_and_home_are_under_build(self):
        env = self.environment("", ROOT / "no-such-folder")
        self.assertEqual(pathlib.Path(env["CARGO_HOME"]), ROOT / "build" / "cargo-home")
        self.assertEqual(pathlib.Path(env["CARGO_TARGET_DIR"]), ROOT / "build" / "launcher")

    def test_rustups_folder_joins_a_path_that_predates_it(self):
        with tempfile.TemporaryDirectory() as folder:
            rust = pathlib.Path(folder)
            env = self.environment("elsewhere", rust)
            self.assertEqual(env["PATH"].split(os.pathsep), [str(rust), "elsewhere"])
            # Already there (in another spelling), it is not added twice.
            spelled = str(rust) + os.sep
            self.assertEqual(self.environment(spelled, rust)["PATH"], spelled)
            # A rustup that is not installed adds nothing.
            self.assertEqual(self.environment("elsewhere", rust / "absent")["PATH"], "elsewhere")

    def test_cargo_started_inside_the_workspace_builds_into_build_too(self):
        config = (WORKSPACE / ".cargo" / "config.toml").read_text(encoding="utf-8")
        target = re.search(r'^target-dir\s*=\s*"([^"]+)"', config, re.MULTILINE)
        self.assertIsNotNone(target)
        self.assertEqual((WORKSPACE / target.group(1)).resolve(), (ROOT / "build" / "launcher"))

    def test_no_launcher_file_counts_towards_the_online_identity(self):
        # scripts/checks/source_identity.py hashes every C and C++ file under src/.
        suffixes = {".h", ".hpp", ".inl", ".c", ".cpp"}
        found = [path for path in WORKSPACE.rglob("*") if path.suffix in suffixes]
        self.assertEqual(found, [])


class WorkspaceTests(unittest.TestCase):
    def test_the_workspace_uses_the_toolchain_the_workflows_pin(self):
        toolchain = (WORKSPACE / "rust-toolchain.toml").read_text(encoding="utf-8")
        channel = re.search(r'^channel\s*=\s*"([^"]+)"', toolchain, re.MULTILINE).group(1)
        workflows = ROOT / ".github" / "workflows"
        pins = set()
        for name in ("ci.yml", "release.yml"):
            text = (workflows / name).read_text(encoding="utf-8")
            pins.update(re.findall(r"""RUSTUP_TOOLCHAIN:\s*["']?([0-9.]+)""", text))
            pins.update(re.findall(r"rustup toolchain install ([0-9.]+)", text))
        self.assertTrue(pins)
        self.assertEqual(pins, {channel})

    def test_the_window_names_its_publisher_rather_than_a_part_of_its_identifier(self):
        # Without a publisher, Tauri names the identifier's second part ("github") as the
        # company in the program's Windows version resource.
        config = json.loads((WORKSPACE / "desktop" / "tauri.conf.json").read_text(encoding="utf-8"))
        publisher = config["bundle"].get("publisher", "")
        self.assertTrue(publisher.strip())
        self.assertNotIn(publisher, config["identifier"].split(".")[:2])

    def test_the_window_takes_its_version_from_the_file_its_build_writes_from_version(self):
        # desktop/build.rs writes gen/version.json from VERSION, and the build fails while the
        # file is missing. A literal here would be a second copy; no version at all would put
        # the crate's 0.0.0 into the Windows version resource and the package information.
        config = json.loads((WORKSPACE / "desktop" / "tauri.conf.json").read_text(encoding="utf-8"))
        self.assertEqual(config.get("version"), "gen/version.json")

    def test_single_instance_starts_one_launcher_for_every_prerelease(self):
        # The single-instance plugin's semver feature adds the version to the name a running
        # launcher registers, a name of its own for every prerelease: a launcher of 1.0.0-beta.1
        # would then open a second window beside one of 1.0.0-beta instead of handing its start
        # over. The lock is resolved with every feature of every workspace crate, so the semver
        # dependency the feature brings shows there whichever manifest turns it on; the plugin's
        # tauri dependency shows that its list was read.
        locked = locked_dependencies("tauri-plugin-single-instance")
        self.assertTrue(locked)
        for names in locked:
            self.assertIn("tauri", names)
            self.assertNotIn("semver", names)


class RunTests(unittest.TestCase):
    def run_main(self, argv, codes):
        with mock.patch.object(launcher, "environment", return_value={"PATH": ""}), \
                mock.patch.object(launcher, "stop_on_signals"), \
                mock.patch.object(launcher, "run", side_effect=codes) as run:
            result = launcher.main(argv)
        return result, [call.args[0] for call in run.call_args_list]

    def test_check_stops_at_the_first_step_that_fails(self):
        result, steps = self.run_main(["check"], [0, 3, 0])
        self.assertEqual(result, 3)
        self.assertEqual([step[1] for step in steps], ["fmt", "clippy"])

    def test_every_named_crate_is_checked(self):
        result, steps = self.run_main(["check", "--package", "gdl-files", "--package", "gdl-media"],
                                      [0, 0, 0])
        self.assertEqual(result, 0)
        for step in steps:
            self.assertIn("gdl-files", step)
            self.assertIn("gdl-media", step)

    def test_run_passes_what_follows_the_separator_to_the_window(self):
        result, steps = self.run_main(["run", "--", "--check-window"], [0])
        self.assertEqual(result, 0)
        self.assertEqual(steps, [["cargo", "run", "--locked", "--package", "gdl-launcher", "--",
                                  "--check-window"]])

    def test_a_missing_cargo_is_reported_rather_than_raised(self):
        with mock.patch.object(launcher.shutil, "which", return_value=None):
            self.assertEqual(launcher.run(["cargo", "build"], {"PATH": ""}), 1)

    def test_cargo_runs_from_the_workspace(self):
        started = mock.Mock()
        started.wait.return_value = 0
        with mock.patch.object(launcher.shutil, "which", return_value="cargo"), \
                mock.patch.object(launcher.process_tree, "start", return_value=started) as start, \
                mock.patch.object(launcher.process_tree, "stop_when_gone"):
            self.assertEqual(launcher.run(["cargo", "build"], {"PATH": "x"}), 0)
        self.assertEqual(start.call_args.kwargs["cwd"], WORKSPACE)

    def test_an_interrupt_while_cargo_runs_ends_its_tree(self):
        # A bare wait would hold the interrupt on Windows until cargo exits; the run waits in
        # slices instead, so the interrupt lands while cargo still runs.
        started = mock.Mock()
        started.wait.side_effect = [subprocess.TimeoutExpired("cargo", launcher.WAIT_SLICE),
                                    KeyboardInterrupt()]
        with mock.patch.object(launcher.shutil, "which", return_value="cargo"), \
                mock.patch.object(launcher.process_tree, "start", return_value=started), \
                mock.patch.object(launcher.process_tree, "stop_when_gone"), \
                mock.patch.object(launcher.process_tree, "stop") as stop:
            with self.assertRaises(KeyboardInterrupt):
                launcher.run(["cargo", "run"], {"PATH": "x"})
        stop.assert_called_once_with(started)
        self.assertEqual(started.wait.call_args.kwargs, {"timeout": launcher.WAIT_SLICE})

    def test_the_signals_that_end_a_run_become_an_interrupt(self):
        with mock.patch.object(launcher.signal, "signal") as handler:
            launcher.stop_on_signals()
        registered = {call.args[0]: call.args[1] for call in handler.call_args_list}
        self.assertIn(signal.SIGTERM, registered)
        if hasattr(signal, "SIGHUP"):
            self.assertIn(signal.SIGHUP, registered)
        for stop in registered.values():
            with self.assertRaises(KeyboardInterrupt):
                stop(signal.SIGTERM, None)


@unittest.skipIf(sys.platform == "win32", "POSIX signals and sessions")
class SignalTreeTests(unittest.TestCase):
    def test_a_terminated_run_ends_cargo_and_what_cargo_started(self):
        # A stand-in for cargo starts a stand-in for rustc and reports its process id; the run
        # that started it is then told to stop, as a closed terminal or a kill would.
        cargo = ("import subprocess, sys, time\n"
                 "rustc = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(60)'])\n"
                 "print(rustc.pid, flush=True)\n"
                 "time.sleep(60)\n")
        runner = ("import os, sys\n"
                  f"sys.path.insert(0, {str(SCRIPTS)!r})\n"
                  "import launcher\n"
                  "launcher.stop_on_signals()\n"
                  f"launcher.run([sys.executable, '-c', {cargo!r}], dict(os.environ))\n")
        with subprocess.Popen([sys.executable, "-c", runner], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True) as process:
            rustc = int(process.stdout.readline())
            self.assertTrue(listed(rustc, True))
            process.send_signal(signal.SIGTERM)
            process.wait(timeout=10)
            self.assertFalse(listed(rustc, False))


if __name__ == "__main__":
    unittest.main()
