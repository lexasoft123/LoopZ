#!/usr/bin/env python3
"""Explicit, guarded existing-slot migration; never restarts global Core Audio."""

import signal
import subprocess

from install_routing import main


if __name__ == "__main__":

    def interrupted(signum, frame):
        raise KeyboardInterrupt("Routing migration interrupted")

    signal.signal(signal.SIGTERM, interrupted)
    try:
        main(legacy_slot=True)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error)) from None
