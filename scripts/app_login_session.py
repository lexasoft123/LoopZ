"""Read the macOS login session without changing authorization or login choices."""

import ctypes
import re
import subprocess


def validate_session(value):
    if (
        not isinstance(value, dict)
        or set(value) != {"boot", "audit"}
        or not isinstance(value["boot"], str)
        or not re.fullmatch(r"[A-F0-9]{8}(?:-[A-F0-9]{4}){3}-[A-F0-9]{12}", value["boot"])
        or type(value["audit"]) is not int
        or not 0 < value["audit"] < 0xFFFFFFFE
    ):
        raise ValueError("Invalid application recovery login session")
    return value


def login_session():
    boot = (
        subprocess.run(
            ["/usr/sbin/sysctl", "-n", "kern.bootsessionuuid"],
            capture_output=True,
            text=True,
            check=True,
            timeout=3,
        )
        .stdout.strip()
        .upper()
    )
    security = ctypes.CDLL("/System/Library/Frameworks/Security.framework/Security")
    operation = security.SessionGetInfo
    operation.argtypes = [
        ctypes.c_uint32,
        ctypes.POINTER(ctypes.c_uint32),
        ctypes.POINTER(ctypes.c_uint32),
    ]
    operation.restype = ctypes.c_int32
    session, attributes = ctypes.c_uint32(), ctypes.c_uint32()
    # AuthSession.h: callerSecuritySession asks for the caller's actual login ID.
    # https://developer.apple.com/documentation/security/sessiongetinfo(_:_:_:)
    if operation(0xFFFFFFFF, ctypes.byref(session), ctypes.byref(attributes)):
        raise RuntimeError("The current login session could not be established")
    if attributes.value & 1 or not attributes.value & 0x10:
        raise RuntimeError("Application recovery requires a graphical login session")
    return validate_session({"boot": boot, "audit": session.value})


def restore_audio(recorded, current, login_enabled, session_loaded):
    validate_session(recorded)
    validate_session(current)
    if type(login_enabled) is not bool or type(session_loaded) is not bool:
        raise ValueError("Recovery startup choices must be explicit booleans")
    return recorded == current or login_enabled or session_loaded
