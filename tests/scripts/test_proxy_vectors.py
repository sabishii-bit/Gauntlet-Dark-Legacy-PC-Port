"""The shared proxy vectors hold to CPython's own proxy rules.

The launcher chooses a request's proxy as urllib, which the alpha installer reached GitHub through,
chose one, and its Rust tests read the same cases (tests/scripts/data/launcher/proxy-vectors.json).
Here urllib.request answers every case itself, reading the case's variables and, on Windows, its
per-user Internet Settings from stand-ins: no variable of this process is set, the registry is never
read and no host is looked up.
"""

from __future__ import annotations

from collections.abc import Mapping
import contextlib
from functools import partial
import json
import os
from pathlib import Path
import platform
import socket
import sys
import types
import unittest
from unittest import mock
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads((ROOT / "tests" / "scripts" / "data" / "launcher" / "proxy-vectors.json")
                     .read_text(encoding="utf-8"))

WINDOWS = os.name == "nt"

# The system a case's "system" field names this one by, and why a case marked for the other cannot
# be held to urllib here.
SYSTEM = "windows" if WINDOWS else "linux"
OTHER_SYSTEM = {
    "linux": "urllib gives this answer on Linux alone, whose os.environ keeps names' letter case",
    "windows": "urllib gives this answer on Windows alone, whose os.environ and fnmatch ignore "
               "letter case",
}

# The fields each kind of case may carry: a field outside them would go untested.
ENVIRONMENT_FIELDS = frozenset({"name", "system", "env", "url", "proxy", "refused", "urllib"})
REGISTRY_FIELDS = ENVIRONMENT_FIELDS | {"enabled", "server", "bypass"}
REFUSALS = ("socks",)

# The per-user key urllib reads the Internet Settings from, and the types of the values it reads.
INTERNET_SETTINGS = r"Software\Microsoft\Windows\CurrentVersion\Internet Settings"
REG_SZ, REG_DWORD = 1, 4

# urllib's matching of an Internet Settings bypass list by shell patterns, which it has from 3.11.9
# on; before it, the registry's bypass looks the host up and matches by regular expression.
BYPASS_OVERRIDE = getattr(urllib.request, "_proxy_bypass_winreg_override", None)
NO_BYPASS_OVERRIDE = (f"Python {platform.python_version()} has no "
                      "urllib.request._proxy_bypass_winreg_override (3.11.9 and later): its "
                      "registry bypass looks the host up and matches by regular expression")
REGISTRY_ON_WINDOWS = "urllib reads the Internet Settings on Windows alone"


def system_name(name: str) -> str:
    """A variable's name as this system's os.environ holds it: upper-cased on Windows, where one
    variable answers to every letter case of its name."""
    return name.upper() if WINDOWS else name


class Environment(Mapping):
    """A case's variables, held as this system's os.environ holds them."""

    def __init__(self, pairs) -> None:
        self._values = {}
        for name, value in pairs:
            self._values[system_name(name)] = value

    def __getitem__(self, name):
        return self._values[system_name(name)]

    def __iter__(self):
        return iter(self._values)

    def __len__(self) -> int:
        return len(self._values)


class OsStandIn:
    """The os module as urllib.request reads it, with a case's variables for its environment."""

    def __init__(self, environ: Environment) -> None:
        self.environ = environ

    def __getattr__(self, name):
        return getattr(os, name)


class SocketStandIn:
    """The socket module as urllib.request reads it, failing the host lookups an older registry
    bypass makes."""

    def __getattr__(self, name):
        return getattr(socket, name)

    @staticmethod
    def gethostbyname(host):
        raise AssertionError(f"a proxy rule looked up {host!r}")

    @staticmethod
    def getfqdn(name=""):
        raise AssertionError(f"a proxy rule looked up {name!r}")


def internet_settings(case: dict) -> types.ModuleType:
    """A winreg module whose per-user Internet Settings hold a registry case's values, and which
    has no other key."""
    values = {"ProxyEnable": (int(case["enabled"]), REG_DWORD)}
    if case.get("server") is not None:
        values["ProxyServer"] = (case["server"], REG_SZ)
    if case.get("bypass") is not None:
        values["ProxyOverride"] = (case["bypass"], REG_SZ)
    module = types.ModuleType("winreg")
    module.HKEY_CURRENT_USER = current_user = object()

    class Key:
        def Close(self) -> None:
            pass

    def open_key(root, path, *arguments):
        if root is not current_user or path != INTERNET_SETTINGS:
            raise FileNotFoundError(path)
        return Key()

    def query_value(key, name):
        if name not in values:
            raise FileNotFoundError(name)
        return values[name]

    module.OpenKey = open_key
    module.QueryValueEx = query_value
    return module


@contextlib.contextmanager
def urllib_reads(case: dict, registry: bool):
    """urllib.request reading a case's variables and, for a registry case, its Internet Settings,
    in place of this process's own."""
    with contextlib.ExitStack() as stack:
        environ = Environment(case.get("env", []))
        stack.enter_context(mock.patch.object(urllib.request, "os", OsStandIn(environ)))
        stack.enter_context(mock.patch.object(urllib.request, "socket", SocketStandIn()))
        if registry and WINDOWS:
            stack.enter_context(mock.patch.dict(sys.modules, {"winreg": internet_settings(case)}))
        yield


def chosen(proxies: dict, bypassed, url: str) -> str | None:
    """The proxy urllib's ProxyHandler sends a request for `url` through: its scheme's entry,
    unless the host is bypassed."""
    request = urllib.request.Request(url)
    entry = proxies.get(request.type)
    if entry is None or (request.host and bypassed(request.host)):
        return None
    return entry


def urllib_answer(case: dict) -> str | None:
    """What urllib chooses for a case: its own answer where it differs from the launcher's, else
    the launcher's."""
    return case["urllib"] if "urllib" in case else case.get("proxy")


class ProxyVectorTests(unittest.TestCase):
    def held_here(self, case: dict) -> bool:
        """Whether urllib can be asked a case on this system; one marked for the other is reported
        skipped."""
        system = case.get("system")
        if system is None or system == SYSTEM:
            return True
        with self.subTest(case=case["name"]):
            self.skipTest(OTHER_SYSTEM[system])
        return False

    def test_the_vectors_keep_their_shape(self):
        self.assertEqual(set(VECTORS), {"schema", "environment", "registry"})
        self.assertEqual(VECTORS["schema"], 1)
        names = set()
        for kind, fields in (("environment", ENVIRONMENT_FIELDS), ("registry", REGISTRY_FIELDS)):
            self.assertTrue(VECTORS[kind])
            for case in VECTORS[kind]:
                with self.subTest(case=case.get("name")):
                    self.assertLessEqual(set(case), fields)
                    self.assertNotIn(case["name"], names)
                    names.add(case["name"])
                    self.assertIn(case.get("system", SYSTEM), OTHER_SYSTEM)
                    self.assertIsInstance(case["url"], str)
                    pairs = case["env"] if kind == "environment" else case.get("env", [])
                    for pair in pairs:
                        self.assertEqual([type(part) for part in pair], [str, str])
                    if kind == "registry":
                        self.assertIsInstance(case["enabled"], bool)
                    if "refused" in case:
                        self.assertIn(case["refused"], REFUSALS)
                        self.assertIsNone(case.get("proxy"))
                        # What urllib would reach in place of the refusal.
                        self.assertIsInstance(case.get("urllib"), str)
                    elif "urllib" in case:
                        self.assertNotEqual(case["urllib"], case.get("proxy"))

    def test_the_environment_stand_in_names_variables_as_this_system_does(self):
        # Read only: on Windows os.environ lists names upper-cased and answers to any letter case;
        # elsewhere a name answers to itself alone.
        listed = set(os.environ)
        for name in sorted(listed):
            other = name.swapcase()
            if not name.isascii() or other == name:
                continue
            with self.subTest(variable=name):
                if WINDOWS:
                    self.assertEqual(name, name.upper())
                    self.assertEqual(os.environ[other], os.environ[name])
                else:
                    self.assertEqual(other in os.environ, other in listed)
        environ = Environment([("Https_Proxy", "first"), ("https_proxy", "second")])
        if WINDOWS:
            self.assertEqual(dict(environ), {"HTTPS_PROXY": "second"})
            self.assertEqual(environ["https_proxy"], "second")
        else:
            self.assertEqual(dict(environ), {"Https_Proxy": "first", "https_proxy": "second"})
            self.assertNotIn("HTTPS_PROXY", environ)

    def test_environment_cases_follow_urllib(self):
        for case in VECTORS["environment"]:
            if not self.held_here(case):
                continue
            with self.subTest(case=case["name"]):
                with urllib_reads(case, registry=False):
                    proxies = urllib.request.getproxies_environment()
                    bypassed = partial(urllib.request.proxy_bypass_environment, proxies=proxies)
                    answer = chosen(proxies, bypassed, case["url"])
                self.assertEqual(answer, urllib_answer(case))

    def test_registry_cases_follow_urllib(self):
        for case in VECTORS["registry"]:
            if not self.held_here(case):
                continue
            name, url, expected = case["name"], case["url"], urllib_answer(case)
            request = urllib.request.Request(url)
            bypass_in_force = case["enabled"] and bool(case.get("bypass"))
            with urllib_reads(case, registry=True):
                environment = urllib.request.getproxies_environment()
                if environment:
                    with self.subTest(case=name, part="environment"):
                        # Any entry, a lone no_proxy's or ftp_proxy's too, hides the Internet
                        # Settings, their bypass list included.
                        proxies = urllib.request.getproxies()
                        self.assertEqual(proxies, environment)
                        self.assertEqual(chosen(proxies, urllib.request.proxy_bypass, url),
                                         expected)
                    continue

                with self.subTest(case=name, part="server"):
                    if not WINDOWS:
                        self.skipTest(REGISTRY_ON_WINDOWS)
                    entry = urllib.request.getproxies_registry().get(request.type)
                    if bypass_in_force:
                        # The bypass decides such a case only when the server proxies its URL, so
                        # its answer alone gives the bypass's verdict on every system.
                        self.assertIsNotNone(entry, "the server does not proxy the case's URL")
                        if expected is not None:
                            self.assertEqual(entry, expected)
                    else:
                        self.assertEqual(entry, expected)

                if bypass_in_force:
                    with self.subTest(case=name, part="bypass"):
                        if BYPASS_OVERRIDE is None:
                            self.skipTest(NO_BYPASS_OVERRIDE)
                        self.assertEqual(BYPASS_OVERRIDE(request.host, case["bypass"]),
                                         expected is None)

                with self.subTest(case=name, part="choice"):
                    if not WINDOWS:
                        self.skipTest(REGISTRY_ON_WINDOWS)
                    if bypass_in_force and BYPASS_OVERRIDE is None:
                        self.skipTest(NO_BYPASS_OVERRIDE)
                    # urllib's whole choice, which looks no host up once no bypass is in force or
                    # the override matches the list.
                    self.assertEqual(
                        chosen(urllib.request.getproxies(), urllib.request.proxy_bypass, url),
                        expected)


if __name__ == "__main__":
    unittest.main()
