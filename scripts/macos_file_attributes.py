"""Bounded, read-only Darwin xattrs through the public sys/xattr.h interface."""

import ctypes
import os


def extended_attributes(path):
    library = ctypes.CDLL(None, use_errno=True)
    listing = library.listxattr
    listing.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    listing.restype = ctypes.c_ssize_t
    reading = library.getxattr
    reading.argtypes = [
        ctypes.c_char_p,
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.c_size_t,
        ctypes.c_uint32,
        ctypes.c_int,
    ]
    reading.restype = ctypes.c_ssize_t
    encoded = os.fsencode(path)
    nofollow = 0x0001  # XATTR_NOFOLLOW; Darwin sys/xattr.h, never bypass authorization.

    def checked(value):
        if value < 0:
            code = ctypes.get_errno()
            raise OSError(code, os.strerror(code), os.fspath(path))
        return value

    size = checked(listing(encoded, None, 0, nofollow))
    if not size:
        return []
    if size > 65536:
        raise ValueError("A signed component has too much attribute name data")
    names = ctypes.create_string_buffer(size)
    used = checked(listing(encoded, names, size, nofollow))
    if used > size or names.raw[used - 1 : used] != b"\0":
        raise RuntimeError("Component metadata changed during enumeration")
    keys = names.raw[:used].split(b"\0")[:-1]
    if len(keys) > 64 or len(keys) != len(set(keys)):
        raise ValueError("Invalid or excessive component attribute names")
    result = []
    total = 0
    for key in sorted(keys):
        size = checked(reading(encoded, key, None, 0, 0, nofollow))
        total += size
        if size > 1024 * 1024 or total > 4 * 1024 * 1024:
            raise ValueError("A signed component exceeds its attribute payload bound")
        if size:
            value = ctypes.create_string_buffer(size)
            used = checked(reading(encoded, key, value, size, 0, nofollow))
            if used != size:
                raise RuntimeError("Component metadata changed during reading")
            content = value.raw
        else:
            content = b""
        result.append((key, content))
    return result
