"""Version ordering shared by the installer and alpha release publisher."""

import re

ALPHA_VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)-alpha\.[1-9][0-9]*")


def alpha_order(value):
    """Compare numeric alpha identifiers, not lexicographic tag names or dates."""
    if not isinstance(value, str):
        return None
    value = value.removeprefix("v")
    if not ALPHA_VERSION.fullmatch(value):
        return None
    core, alpha = value.split("-alpha.")
    return (*map(int, core.split(".")), int(alpha))
