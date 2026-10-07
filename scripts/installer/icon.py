"""Convert the user's disc icon to Windows resources, without distributing retail art.

The first TPL frame and colour expansion match formats/TplFile.cpp and
Gauntlet::applyWindowIcon. Only the three direct-colour formats that reader
supports are accepted. No Pillow, Qt or external resource editor is needed.
"""

import os
import struct

MAX_TPL_BYTES = 1024 * 1024
ICON_SIZES = (16, 32, 48, 64, 128, 256)


def decode_first_frame(data):
    """Return width, height, top-down RGBA; TPL texels are big-endian 4x4 tiles."""
    if len(data) < 12 or len(data) > MAX_TPL_BYTES:
        raise ValueError("Invalid disc icon TPL size")
    magic, count, table = struct.unpack_from(">III", data)
    if magic != 0x0020AF30 or not 1 <= count <= 256 or table < 12 or table + count * 8 > len(data):
        raise ValueError("Invalid disc icon TPL directory")
    header, palette = struct.unpack_from(">II", data, table)
    if header + 36 > len(data):
        raise ValueError("Truncated disc icon TPL header")
    height, width, encoding, offset = struct.unpack_from(">HHII", data, header)
    if not 1 <= width <= 256 or not 1 <= height <= 256:
        raise ValueError("Invalid disc icon dimensions")
    if encoding not in (4, 5, 6) or palette != 0:
        raise ValueError("Unsupported disc icon TPL format")
    tile_bytes = 64 if encoding == 6 else 32
    tiles_x = (width + 3) // 4
    if offset + tiles_x * ((height + 3) // 4) * tile_bytes > len(data):
        raise ValueError("Truncated disc icon TPL pixels")
    rgba = bytearray()
    for y in range(height):
        for x in range(width):
            tile = offset + ((y // 4) * tiles_x + x // 4) * tile_bytes
            pixel = ((y % 4) * 4 + x % 4) * 2
            if encoding == 6:
                alpha, red = data[tile + pixel:tile + pixel + 2]
                green, blue = data[tile + 32 + pixel:tile + 34 + pixel]
            else:
                value, = struct.unpack_from(">H", data, tile + pixel)
                if encoding == 4:
                    red, green, blue = value >> 11, (value >> 5) & 63, value & 31
                    red, green, blue = (red << 3) | (red >> 2), (green << 2) | (green >> 4), (blue << 3) | (blue >> 2)
                    alpha = 255
                elif value & 0x8000:
                    red, green, blue = (value >> 10) & 31, (value >> 5) & 31, value & 31
                    red, green, blue = [(channel << 3) | (channel >> 2) for channel in (red, green, blue)]
                    alpha = 255
                else:
                    red, green, blue = [((value >> shift) & 15) * 17 for shift in (8, 4, 0)]
                    alpha = value >> 12
                    alpha = (alpha << 5) | (alpha << 2) | (alpha >> 1)
            rgba.extend((red, green, blue, alpha))
    return width, height, bytes(rgba)


def icon_resources(width, height, rgba):
    """RT_GROUP_ICON plus bottom-up BGRA DIBs, with alpha and legacy AND masks."""
    if not 1 <= width <= 256 or not 1 <= height <= 256 or len(rgba) != width * height * 4:
        raise ValueError("Invalid decoded icon")
    group = bytearray(struct.pack("<HHH", 0, 1, len(ICON_SIZES)))
    images = []
    for index, size in enumerate(ICON_SIZES, 1):
        pixels = bytearray()
        mask_stride = ((size + 31) // 32) * 4
        mask = bytearray(mask_stride * size)
        for row in range(size):
            y = (size - row - 1) * height // size
            for x in range(size):
                offset = (y * width + x * width // size) * 4
                red, green, blue, alpha = rgba[offset:offset + 4]
                pixels.extend((blue, green, red, alpha))
                if alpha == 0:
                    mask[row * mask_stride + x // 8] |= 0x80 >> (x % 8)
        bitmap = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(pixels), 0, 0, 0, 0)
        bitmap += pixels + mask
        group.extend(struct.pack("<BBBBHHIH", size % 256, size % 256, 0, 0, 1, 32, len(bitmap), index))
        images.append(bitmap)
    return bytes(group), images


def embed_icon(executable, group, images):
    """Edit only a staged unsigned game executable; preserve its manifest and other resources.

    Resource IDs 1..6 and group 1 belong to the game icon. Public runtime builds
    contain no retail icons. If binaries are signed in future, this local resource
    customization needs revisiting because PE resource edits invalidate signatures.
    """
    if os.name != "nt":
        raise OSError("Windows icon resources can only be installed on Windows")
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.BeginUpdateResourceW.argtypes = (wintypes.LPCWSTR, wintypes.BOOL)
    kernel.BeginUpdateResourceW.restype = wintypes.HANDLE
    kernel.UpdateResourceW.argtypes = (wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                      wintypes.WORD, ctypes.c_void_p, wintypes.DWORD)
    kernel.UpdateResourceW.restype = wintypes.BOOL
    kernel.EndUpdateResourceW.argtypes = (wintypes.HANDLE, wintypes.BOOL)
    kernel.EndUpdateResourceW.restype = wintypes.BOOL
    handle = kernel.BeginUpdateResourceW(str(executable.resolve()), False)
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        for kind, identity, content in [(3, index, data) for index, data in enumerate(images, 1)] + [(14, 1, group)]:
            buffer = ctypes.create_string_buffer(content)
            if not kernel.UpdateResourceW(handle, kind, identity, 0, buffer, len(content)):
                raise ctypes.WinError(ctypes.get_last_error())
    except BaseException:
        kernel.EndUpdateResourceW(handle, True)
        raise
    if not kernel.EndUpdateResourceW(handle, False):
        raise ctypes.WinError(ctypes.get_last_error())


def refresh_shell_icon(executable):
    """Invalidate Explorer's cached icon after the installed file is committed."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes
    shell = ctypes.WinDLL("shell32")
    shell.SHChangeNotify.argtypes = (wintypes.LONG, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p)
    shell.SHChangeNotify.restype = None
    # SHCNE_UPDATEITEM, SHCNF_PATHW: the path now refers to a new resource image.
    shell.SHChangeNotify(0x2000, 0x0005, ctypes.c_wchar_p(str(executable.resolve())), None)
