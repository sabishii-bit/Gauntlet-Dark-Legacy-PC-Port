#!/usr/bin/env python3
"""Report every diagnostic clangd would show in the editor, for all source files.

Runs clangd as a language server over the repository's compile_commands.json
(so configure and build first), opens each file under src/ and tests/, and
prints the diagnostics clangd publishes, clang-tidy findings included. Exit
status is 1 when any diagnostic is found.

    python scripts/clangd-check.py            # all files
    python scripts/clangd-check.py src/game   # a subtree or single file
"""

import json
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
SEVERITY = {1: "error", 2: "warning", 3: "info", 4: "hint"}


def find_clangd() -> str:
    candidates = [
        os.environ.get("CLANGD"),
        shutil.which("clangd"),
        r"C:\Program Files\LLVM\bin\clangd.exe",
        "/usr/bin/clangd",
        "/usr/local/bin/clangd",
    ]
    for candidate in candidates:
        if candidate and pathlib.Path(candidate).exists():
            return candidate
    sys.exit("clangd not found; set CLANGD or put it on PATH")


def collect_files(args: list[str]) -> list[pathlib.Path]:
    targets = [ROOT / a for a in args] if args else [ROOT / "src", ROOT / "tests"]
    files: list[pathlib.Path] = []
    for target in targets:
        if target.is_file():
            files.append(target)
        else:
            files.extend(p for p in target.rglob("*") if p.suffix in (".cpp", ".h"))
    return sorted(set(files))


def normalize_uri(uri: str) -> str:
    path = urllib.parse.unquote(urllib.parse.urlparse(uri).path)
    if os.name == "nt" and path.startswith("/"):
        path = path[1:]
    return str(pathlib.Path(path).resolve()).lower()


class Clangd:
    def __init__(self, exe: str):
        self.proc = subprocess.Popen(
            [exe, f"--compile-commands-dir={ROOT}", "--background-index=false", "--clang-tidy",
             "--log=error", "--header-insertion=never"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=ROOT)
        self.inbox: queue.Queue = queue.Queue()
        self.next_id = 1
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self) -> None:
        stream = self.proc.stdout
        while True:
            headers: dict[str, str] = {}
            while True:
                line = stream.readline()
                if not line:
                    return
                line = line.decode("utf-8").strip()
                if not line:
                    break
                key, _, value = line.partition(":")
                headers[key.strip().lower()] = value.strip()
            body = stream.read(int(headers["content-length"]))
            self.inbox.put(json.loads(body))

    def send(self, message: dict) -> None:
        payload = json.dumps(message).encode("utf-8")
        self.proc.stdin.write(f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii") + payload)
        self.proc.stdin.flush()

    def request(self, method: str, params: dict) -> int:
        request_id = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        return request_id

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def wait_response(self, request_id: int, timeout: float = 60.0) -> dict:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.inbox.get(timeout=0.5)
            except queue.Empty:
                continue
            if message.get("id") == request_id:
                return message
        sys.exit(f"timed out waiting for response to request {request_id}")


def main() -> int:
    files = collect_files(sys.argv[1:])
    if not (ROOT / "compile_commands.json").exists():
        sys.exit("compile_commands.json not found at the repository root; configure and build first")

    clangd = Clangd(find_clangd())
    init_id = clangd.request("initialize", {
        "processId": os.getpid(),
        "rootUri": ROOT.as_uri(),
        "capabilities": {},
    })
    clangd.wait_response(init_id)
    clangd.notify("initialized", {})

    pending = {}
    for path in files:
        pending[str(path.resolve()).lower()] = path
        clangd.notify("textDocument/didOpen", {"textDocument": {
            "uri": path.as_uri(),
            "languageId": "cpp",
            "version": 1,
            "text": path.read_text(encoding="utf-8"),
        }})

    diagnostics: dict[str, list] = {}
    quiet_since = time.monotonic()
    deadline = time.monotonic() + 900.0
    while time.monotonic() < deadline:
        try:
            message = clangd.inbox.get(timeout=0.5)
        except queue.Empty:
            if not pending and time.monotonic() - quiet_since > 3.0:
                break
            continue
        if message.get("method") != "textDocument/publishDiagnostics":
            continue
        key = normalize_uri(message["params"]["uri"])
        diagnostics[key] = message["params"]["diagnostics"]
        pending.pop(key, None)
        quiet_since = time.monotonic()

    for key in pending:
        print(f"{pending[key].relative_to(ROOT)}: no diagnostics received (timeout)")

    total = 0
    for key in sorted(diagnostics):
        found = diagnostics[key]
        if not found:
            continue
        rel = pathlib.Path(key).resolve()
        try:
            rel = rel.relative_to(ROOT.resolve())
        except ValueError:
            pass
        for diag in found:
            start = diag["range"]["start"]
            code = diag.get("code")
            code_text = f" [{code}]" if code else ""
            severity = SEVERITY.get(diag.get("severity", 2), "warning")
            print(f"{rel}:{start['line'] + 1}:{start['character'] + 1}: {severity}: "
                  f"{diag['message']}{code_text}")
            total += 1

    clangd.request("shutdown", {})
    clangd.notify("exit", {})
    clangd.proc.wait(timeout=10)

    print(f"{len(files)} files checked, {total} diagnostics")
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
