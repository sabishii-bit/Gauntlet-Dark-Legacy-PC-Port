"""The launcher script runs cargo where the workspace's own settings apply, and builds outside src."""
import contextlib
import io
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


def main_runs(argv, codes):
    """The script's exit code for `argv`, and the calls it made to run, each run standing in for
    a command that exits with the next of `codes`."""
    with mock.patch.object(launcher, "environment", return_value={"PATH": ""}), \
            mock.patch.object(launcher, "stop_on_signals"), \
            mock.patch.object(launcher, "run", side_effect=codes) as run:
        result = launcher.main(argv)
    return result, run.call_args_list


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

    def test_build_of_named_packages_stays_within_them(self):
        headless = ["--package", "gdl-launcher-headless"]
        self.assertEqual(launcher.plan("build", packages=["gdl-launcher-headless"], release=True),
                         [["cargo", "build", "--locked", *headless, "--release"]])
        named = ["--package", "gdl-media", "--package", "gdl-extract"]
        self.assertEqual(launcher.plan("build", packages=["gdl-media", "gdl-extract"]),
                         [["cargo", "build", "--locked", *named]])
        # The command line names them as check's does.
        result, calls = main_runs(["build", "--release", "--package", "gdl-launcher-headless"],
                                  [0])
        self.assertEqual(result, 0)
        self.assertEqual([call.args[0] for call in calls],
                         [["cargo", "build", "--locked", *headless, "--release"]])

    def test_parity_runs_the_gate_with_this_python(self):
        gate = ROOT / "scripts" / "checks" / "launcher_parity.py"
        self.assertEqual(launcher.PARITY_GATE, gate)
        self.assertEqual(launcher.plan("parity", arguments=["--verify", "--keep"]),
                         [[sys.executable, str(gate), "--verify", "--keep"]])
        self.assertEqual(launcher.plan("parity"), [[sys.executable, str(gate)]])

    def test_an_unknown_command_is_refused(self):
        with self.assertRaises(ValueError):
            launcher.plan("unpack")


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
        result, calls = main_runs(argv, codes)
        return result, [call.args[0] for call in calls]

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
            # A command told to start elsewhere starts there, None being this process's folder,
            # as the parity gate starts.
            for folder in (None, ROOT / "scripts"):
                with self.subTest(folder=folder):
                    self.assertEqual(launcher.run(["cargo", "build"], {"PATH": "x"}, cwd=folder),
                                     0)
                    self.assertEqual(start.call_args.kwargs["cwd"], folder)

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

    def test_extract_runs_the_headless_program_on_absolute_paths(self):
        # Cargo starts the program in the workspace, so a dump and an output named from the
        # developer's own folder are made absolute first. The program holds the dump as the
        # extraction scripts hold one unless --install asks for an installation's strictness.
        program = ["cargo", "run", "--locked", "--package", "gdl-launcher-headless"]
        here = os.getcwd()
        rom = os.path.join("dumps", "Gauntlet Legends (USA).z64")
        disc = os.path.join("dumps", "Gauntlet Legends (USA)", "Gauntlet Legends (USA).cue")
        output = os.path.join("trees", "legends")
        result, steps = self.run_main(["extract", "n64", rom, "--output", output], [0])
        self.assertEqual(result, 0)
        self.assertEqual(steps, [[*program, "--", "--extract", "n64", os.path.join(here, rom),
                                  "--output", os.path.join(here, output), "--strict", "script"]])
        result, steps = self.run_main(["extract", "dreamcast", disc, "--output", output,
                                       "--install", "--release"], [0])
        self.assertEqual(result, 0)
        self.assertEqual(steps, [[*program, "--release", "--", "--extract", "dreamcast",
                                  os.path.join(here, disc), "--output",
                                  os.path.join(here, output)]])
        self.assertTrue(os.path.isabs(steps[0][-3]))
        self.assertTrue(os.path.isabs(steps[0][-1]))
        # A name that ends in a dot or a space reaches the program as it was typed, on every
        # system, so that the program refuses it rather than make a folder of another name, the
        # one Windows would give it as it made the path absolute.
        for name in ("n64.", "n64 ", "n64. "):
            with self.subTest(name=name):
                typed = os.path.join("trees", name)
                _, steps = self.run_main(["extract", "n64", rom, "--output", typed], [0])
                given = steps[0][steps[0].index("--output") + 1]
                self.assertEqual(given, os.path.join(here, typed))
                self.assertTrue(given.endswith(name))
                self.assertTrue(os.path.isabs(given))
        if os.name == "nt":
            # A drive's own relative path starts from that drive's folder, here this one.
            drive = os.path.splitdrive(here)[0]
            _, steps = self.run_main(["extract", "n64", drive + rom, "--output", output], [0])
            self.assertEqual(steps[0][steps[0].index("--output") - 1], os.path.join(here, rom))
        # A refusal is the program's exit code.
        self.assertEqual(self.run_main(["extract", "n64", rom, "--output", output], [1])[0], 1)
        # Only an expansion the program extracts, and never without --output.
        for argv in (["extract", "gamecube", rom, "--output", output], ["extract", "n64", rom]):
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as refused:
                    self.run_main(argv, [])
                self.assertEqual(refused.exception.code, 2)

    def test_extract_and_parity_pass_their_arguments_through(self):
        # A path that is absolute already reaches the program as it was given, spaces and all,
        # each one argument; cargo, which runs from the workspace, takes --release.
        disc = os.path.abspath(os.path.join("Gauntlet Legends (USA)",
                                            "Gauntlet Legends (USA).cue"))
        output = os.path.abspath(os.path.join("Legends trees", "dreamcast"))
        result, calls = main_runs(["extract", "dreamcast", disc, "--output", output, "--release"],
                                  [0])
        self.assertEqual(result, 0)
        self.assertEqual([call.args[0] for call in calls], [[
            "cargo", "run", "--locked", "--package", "gdl-launcher-headless", "--release", "--",
            "--extract", "dreamcast", disc, "--output", output, "--strict", "script"]])
        self.assertEqual(calls[0].kwargs["cwd"], launcher.WORKSPACE)
        # Every argument after parity is the gate's, as it was given, the options this script
        # takes itself and a separator included; the gate runs from the folder this script was
        # started in, where a relative path given to it means what was typed.
        given = ["--verify", "--platform", "n64", "--n64-rom", os.path.join("dumps", "rom.z64"),
                 "--release", "--package", "gdl-media", "--help", "--", "--keep"]
        result, calls = main_runs(["parity", *given], [0])
        self.assertEqual(result, 0)
        self.assertEqual([call.args[0] for call in calls],
                         [[sys.executable, str(launcher.PARITY_GATE), *given]])
        self.assertIsNone(calls[0].kwargs["cwd"])
        # The gate's exit code is the run's, and an argument before the word is refused rather
        # than handed to the gate.
        self.assertEqual(main_runs(["parity", "--verify"], [1])[0], 1)
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as refused:
            main_runs(["--verify", "parity"], [])
        self.assertEqual(refused.exception.code, 2)

    def test_parity_runs_the_gate_in_this_process_environment(self):
        # The gate makes cargo's environment itself, as it does when started by hand. Made again
        # inside cargo's, the developer environment would put back the PATH it was first given,
        # and rustup's folder, added for a shell older than rustup, would be gone.
        with mock.patch.object(launcher, "environment",
                               return_value={"PATH": "cargo's"}) as cargo, \
                mock.patch.object(launcher, "stop_on_signals"), \
                mock.patch.object(launcher, "run", return_value=0) as run, \
                mock.patch.dict(os.environ, {"GDL_LAUNCHER_CALLER": "the caller's"}):
            self.assertEqual(launcher.main(["parity", "--verify"]), 0)
            given = run.call_args.args[1]
            self.assertEqual(given, dict(os.environ))
            self.assertIsNot(given, os.environ)
        cargo.assert_not_called()
        self.assertEqual(given["GDL_LAUNCHER_CALLER"], "the caller's")
        # Cargo's own commands still run in cargo's environment.
        for argv in (["build"], ["extract", "n64", "rom.z64", "--output", "n64"]):
            with self.subTest(argv=argv):
                result, calls = main_runs(argv, [0])
                self.assertEqual(result, 0)
                self.assertEqual(calls[0].args[1], {"PATH": ""})

    def test_a_run_past_its_timeout_ends_cargos_tree(self):
        # Cargo runs in a session of its own, so only the run that holds it can end its tree.
        # This cargo never exits, and the clock moves only as the run waits on it.
        now = [0.0]

        def wait(timeout):
            now[0] += timeout
            raise subprocess.TimeoutExpired("cargo", timeout)

        started = mock.Mock()
        started.wait.side_effect = wait
        timeout = 2.5 * launcher.WAIT_SLICE
        with mock.patch.object(launcher.shutil, "which", return_value="cargo"), \
                mock.patch.object(launcher.process_tree, "start", return_value=started), \
                mock.patch.object(launcher.process_tree, "stop_when_gone"), \
                mock.patch.object(launcher.process_tree, "stop") as stop, \
                mock.patch.object(launcher, "time") as clock, \
                contextlib.redirect_stderr(io.StringIO()) as err:
            clock.monotonic.side_effect = lambda: now[0]
            result = launcher.run(["cargo", "build", "--release"], {"PATH": "x"}, timeout=timeout)
        self.assertEqual(result, launcher.TIMED_OUT)
        stop.assert_called_once_with(started)
        # It waited a slice at a time up to the first slice past its deadline, and no further.
        self.assertEqual(started.wait.call_count, 3)
        self.assertEqual(started.wait.call_args.kwargs, {"timeout": launcher.WAIT_SLICE})
        self.assertEqual(err.getvalue(),
                         f"cargo build --release ran longer than {timeout:g} s and was stopped\n")

    def test_a_run_within_its_timeout_gives_cargos_code(self):
        started = mock.Mock()
        started.wait.side_effect = [subprocess.TimeoutExpired("cargo", launcher.WAIT_SLICE), 3]
        with mock.patch.object(launcher.shutil, "which", return_value="cargo"), \
                mock.patch.object(launcher.process_tree, "start", return_value=started), \
                mock.patch.object(launcher.process_tree, "stop_when_gone"), \
                mock.patch.object(launcher.process_tree, "stop") as stop, \
                mock.patch.object(launcher, "time") as clock:
            clock.monotonic.return_value = 100.0
            self.assertEqual(launcher.run(["cargo", "test"], {"PATH": "x"}, timeout=60), 3)
        stop.assert_not_called()

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
