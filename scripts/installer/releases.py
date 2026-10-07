"""Read public, published releases and fetch a verified runtime, never game data.

Trust comes from HTTPS to this project's GitHub release account. Asset digests
detect corruption; they are not an independent publisher signature. Do not use
the latest-release endpoint: it excludes the prereleases used by this project.
"""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import urllib.parse
import urllib.request

from .install import MAX_PAYLOAD, check_cancel
from .versions import version_order

REPOSITORY = "sabishii-bit/Gauntlet-Dark-Legacy-PC-Port"
API = f"https://api.github.com/repos/{REPOSITORY}/releases"
MAX_RESPONSE = 8 * 1024 * 1024
MAX_PAGES = 20
TIMEOUT = 15


def runtime_name(version, system):
    return f"GauntletDarkLegacy-{version}-{system}-runtime.zip"


def setup_name(version, system):
    suffix = ".exe" if system == "windows-x64" else ".tar.gz"
    return f"GauntletDarkLegacy-{version}-{system}-setup{suffix}"


def trusted_url(url):
    parsed = urllib.parse.urlsplit(url)
    if (parsed.scheme != "https" or parsed.username or parsed.password or
            parsed.port not in (None, 443) or parsed.hostname not in
            {"api.github.com", "github.com", "release-assets.githubusercontent.com",
             "objects.githubusercontent.com"}):
        raise ValueError("Update download left GitHub's secure release hosts")


class ReleaseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, newurl):
        trusted_url(newurl)
        return super().redirect_request(request, response, code, message, headers, newurl)


def open_release(url):
    trusted_url(url)
    request = urllib.request.Request(url, headers={
        "User-Agent": "GauntletDarkLegacy-Updater",
        "Accept": "application/vnd.github+json" if url.startswith(API) else "application/octet-stream",
        "X-GitHub-Api-Version": "2026-03-10",
    })
    return urllib.request.build_opener(ReleaseRedirect()).open(request, timeout=TIMEOUT)


@dataclass(frozen=True)
class Release:
    version: str
    platform: str
    url: str
    size: int
    sha256: str
    installer: "Release | None" = None


def release_asset(assets, version, system, name):
    matches = [asset for asset in assets if isinstance(asset, dict) and asset.get("name") == name]
    if not matches:
        return None
    if len(matches) != 1:
        raise ValueError("Ambiguous release asset")
    asset = matches[0]
    url = asset.get("browser_download_url")
    expected_path = f"/{REPOSITORY}/releases/download/v{version}/{name}"
    parsed = urllib.parse.urlsplit(url) if isinstance(url, str) else None
    # GitHub escapes '+' in tags/asset names containing SemVer build metadata.
    correct_url = (parsed is not None and parsed.scheme == "https" and
                   parsed.netloc == "github.com" and not parsed.query and not parsed.fragment and
                   urllib.parse.unquote(parsed.path) == expected_path)
    digest = asset.get("digest", "")
    size = asset.get("size")
    if (asset.get("state") != "uploaded" or not correct_url or
            not isinstance(digest, str) or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest) or
            type(size) is not int or not 0 < size <= MAX_PAYLOAD):
        raise ValueError("Release asset is incomplete or lacks its GitHub SHA-256 digest")
    return Release(version, system, url, size, digest[7:])


def select_release(releases, installed_version, system):
    current = version_order(installed_version)
    if current is None:
        raise ValueError("The installed version is not a valid semantic version")
    if system not in ("windows-x64", "linux-x64"):
        raise ValueError("No updater package exists for this platform")
    newest = None
    for release in releases:
        if not isinstance(release, dict) or release.get("draft") is not False:
            continue
        tag = release.get("tag_name", "")
        order = version_order(tag)
        if order is None or not tag.startswith("v") or order <= current:
            continue
        version = tag[1:]
        assets = release.get("assets", [])
        if not isinstance(assets, list):
            raise ValueError("Invalid GitHub release asset list")
        runtime = release_asset(assets, version, system, runtime_name(version, system))
        # An installer alone is not an in-place runtime update.
        if runtime is None:
            continue
        if newest is None or order > version_order(newest.version):
            installer = release_asset(assets, version, system, setup_name(version, system))
            newest = Release(version, system, runtime.url, runtime.size, runtime.sha256, installer)
    return newest


def check_updates(installed_version, system, cancel=lambda: False):
    releases = []
    for page in range(1, MAX_PAGES + 1):
        check_cancel(cancel)
        with open_release(f"{API}?per_page=100&page={page}") as response:
            data = response.read(MAX_RESPONSE + 1)
        if len(data) > MAX_RESPONSE:
            raise ValueError("GitHub release response is too large")
        rows = json.loads(data)
        if not isinstance(rows, list):
            raise ValueError("Invalid GitHub release response")
        releases.extend(rows)
        if len(rows) < 100:
            check_cancel(cancel)
            return select_release(releases, installed_version, system)
    raise ValueError("Too many releases to check safely")


def download_release(release, target: Path, progress=lambda _done, _total, _name: None,
                     cancel=lambda: False):
    """Stream to a private file; a cancellation/truncation/hash failure leaves no payload."""
    check_cancel(cancel)
    # Exclusive creation ensures error cleanup never deletes an existing file.
    with target.open("xb") as output:
        try:
            digest = hashlib.sha256()
            done = 0
            with open_release(release.url) as response:
                while block := response.read(1024 * 1024):
                    check_cancel(cancel)
                    done += len(block)
                    if done > release.size:
                        raise ValueError("Update download exceeds its published size")
                    output.write(block)
                    digest.update(block)
                    progress(done, release.size, "Downloading " + release.version)
            check_cancel(cancel)
            if done != release.size or digest.hexdigest() != release.sha256:
                raise ValueError("Update download is incomplete or failed its SHA-256 check")
        except BaseException:
            output.close()
            target.unlink()
            raise
    return target
