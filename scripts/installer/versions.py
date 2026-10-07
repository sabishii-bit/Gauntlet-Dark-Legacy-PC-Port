"""SemVer 2.0 ordering shared by the installer and release publisher.

Precedence follows https://semver.org/: numeric identifiers compare numerically,
stable versions follow their prereleases, and build metadata does not affect order.
"""

import re

NUMBER = r"(?:0|[1-9][0-9]*)"
IDENTIFIER = rf"(?:{NUMBER}|[0-9]*[A-Za-z-][0-9A-Za-z-]*)"
SEMVER = re.compile(
    rf"(?P<major>{NUMBER})\.(?P<minor>{NUMBER})\.(?P<patch>{NUMBER})"
    rf"(?:-(?P<prerelease>{IDENTIFIER}(?:\.{IDENTIFIER})*))?"
    r"(?:\+(?P<build>[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?")


def version_order(value):
    """Return (major, minor, patch, stable, typed identifiers), ignoring build metadata.

    Accept the optional v prefix used by Git tags. Inventory/VERSION validation uses
    SEMVER directly so stored versions cannot accidentally include that prefix.
    """
    if not isinstance(value, str):
        return None
    value = value.removeprefix("v")
    match = SEMVER.fullmatch(value)
    if not match:
        return None
    prerelease = match["prerelease"]
    identifiers = tuple((0, int(part)) if part.isdigit() else (1, part)
                        for part in prerelease.split(".")) if prerelease is not None else ()
    return (*(int(match[key]) for key in ("major", "minor", "patch")),
            prerelease is None, identifiers)


def is_prerelease(value):
    order = version_order(value)
    if order is None:
        raise ValueError("Invalid semantic version")
    return not order[3]
