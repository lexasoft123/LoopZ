"""Read-only macOS kernel generation and running-code signature inspection."""

import ctypes
import errno

from driver_host import BSDInfo


def valid_pid(pid):
    if type(pid) is not int or not 1 < pid < 0x80000000:
        raise ValueError("A valid process identifier is required")


def process_generation(pid, owner=None):
    valid_pid(pid)
    if owner is not None and (type(owner) is not int or not 0 <= owner <= 0xFFFFFFFF):
        raise ValueError("Invalid expected process owner")
    library = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
    library.proc_pidinfo.argtypes = [
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_uint64,
        ctypes.c_void_p,
        ctypes.c_int,
    ]
    library.proc_pidinfo.restype = ctypes.c_int
    info = BSDInfo()
    ctypes.set_errno(0)
    result = library.proc_pidinfo(pid, 3, 0, ctypes.byref(info), ctypes.sizeof(info))
    if result != ctypes.sizeof(info):
        if ctypes.get_errno() == errno.ESRCH:
            raise ProcessLookupError("The inspected process has exited")
        raise RuntimeError("Kernel process generation is unavailable")
    if info.pid != pid or not info.startSeconds or info.startMicroseconds >= 1_000_000:
        raise RuntimeError("Kernel process generation is incomplete")
    if owner is not None and any(uid != owner for uid in (info.uid, info.ruid, info.svuid)):
        raise RuntimeError("The inspected process has an unexpected owner")
    library.proc_pidpath.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    library.proc_pidpath.restype = ctypes.c_int
    path = ctypes.create_string_buffer(4096)
    ctypes.set_errno(0)
    if library.proc_pidpath(pid, path, len(path)) <= 0:
        if ctypes.get_errno() == errno.ESRCH:
            raise ProcessLookupError("The inspected process has exited")
        raise RuntimeError("Kernel process executable is unavailable")
    return {
        "pid": pid,
        "uid": info.uid,
        "startSeconds": info.startSeconds,
        "startMicroseconds": info.startMicroseconds,
        "executable": path.value.decode("utf-8", errors="strict"),
    }


def same_generation(left, right):
    # A vnode rename can change proc_pidpath without changing the process.
    # Callers separately verify its executable image and dynamic signature.
    return all(
        left[key] == right[key] for key in ("pid", "uid", "startSeconds", "startMicroseconds")
    )


def require_process_code(pid, requirement):
    """Validate a live SecCode, not just the current file at its old pathname.

    Public SDK SecCode.h specifies dynamic validity and filesystem-race-safe
    identity validation for SecCodeCheckValidity with default flags. No code is
    executed, entitlement changed, or privacy/authorization dialog requested.
    """
    valid_pid(pid)
    if (
        not isinstance(requirement, str)
        or not requirement
        or len(requirement) > 4096
        or "\0" in requirement
    ):
        raise ValueError("A bounded process signing requirement is required")
    pointer = ctypes.c_void_p
    core = ctypes.CDLL("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
    security = ctypes.CDLL("/System/Library/Frameworks/Security.framework/Security")
    core.CFNumberCreate.argtypes = [pointer, ctypes.c_int, pointer]
    core.CFNumberCreate.restype = pointer
    core.CFDictionaryCreate.argtypes = [pointer, pointer, pointer, ctypes.c_long, pointer, pointer]
    core.CFDictionaryCreate.restype = pointer
    core.CFStringCreateWithCString.argtypes = [pointer, ctypes.c_char_p, ctypes.c_uint32]
    core.CFStringCreateWithCString.restype = pointer
    core.CFRelease.argtypes = [pointer]
    core.CFRelease.restype = None
    security.SecCodeCopyGuestWithAttributes.argtypes = [
        pointer,
        pointer,
        ctypes.c_uint32,
        ctypes.POINTER(pointer),
    ]
    security.SecCodeCopyGuestWithAttributes.restype = ctypes.c_int32
    security.SecRequirementCreateWithString.argtypes = [
        pointer,
        ctypes.c_uint32,
        ctypes.POINTER(pointer),
    ]
    security.SecRequirementCreateWithString.restype = ctypes.c_int32
    security.SecCodeCheckValidity.argtypes = [pointer, ctypes.c_uint32, pointer]
    security.SecCodeCheckValidity.restype = ctypes.c_int32
    owned = []

    def retain_created(value):
        if not value:
            raise RuntimeError("Cannot allocate process signature inspection context")
        owned.append(value)
        return value

    def copied(function, *arguments):
        value = pointer()
        status = function(*arguments, ctypes.byref(value))
        if value.value:
            owned.append(value)
        if status or not value.value:
            raise RuntimeError(f"Running-code identity could not be established ({status})")
        return value

    try:
        number = retain_created(core.CFNumberCreate(None, 9, ctypes.byref(ctypes.c_int(pid))))
        key = pointer.in_dll(security, "kSecGuestAttributePid")
        keys, values = (pointer * 1)(key.value), (pointer * 1)(number)
        attributes = retain_created(
            core.CFDictionaryCreate(
                None,
                keys,
                values,
                1,
                ctypes.addressof(ctypes.c_byte.in_dll(core, "kCFTypeDictionaryKeyCallBacks")),
                ctypes.addressof(ctypes.c_byte.in_dll(core, "kCFTypeDictionaryValueCallBacks")),
            )
        )
        text = retain_created(
            core.CFStringCreateWithCString(None, requirement.encode("utf-8"), 0x08000100)
        )
        code = copied(security.SecCodeCopyGuestWithAttributes, None, attributes, 0)
        rule = copied(security.SecRequirementCreateWithString, text, 0)
        status = security.SecCodeCheckValidity(code, 0, rule)
        if status:
            raise RuntimeError(f"Running-code signature rejected ({status})")
    finally:
        for value in reversed(owned):
            core.CFRelease(value)
