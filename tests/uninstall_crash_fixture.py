"""Private test child. Parent kills only this child at a named checkpoint."""

import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import uninstall_files
from uninstall_files import RemovalFiles
from uninstall_journal import RemovalJournal
from uninstall_recovery import recover_product
from uninstall_workspace import DiagnosticWorkspaces
from uninstall_tests import Host


def pause():
    print("ready", flush=True)
    while True:
        time.sleep(1)


def main():
    root, checkpoint = Path(sys.argv[1]).resolve(), sys.argv[2]
    # This fixture requires its parent's private marker and only fixed children.
    if (
        not root.name.startswith("loopz-journal-test-")
        or (root / "fixture.marker").read_text() != "private-test"
    ):
        raise ValueError("Crash fixture requires its isolated parent directory")
    targets = {"app": root / "Applications/LoopZ.app", "broker": root / "Support/broker"}
    work = root / "loopz-uninstall-work"

    class Journal(RemovalJournal):
        def record_quarantine(self, *args):
            super().record_quarantine(*args)
            if checkpoint == "quarantine-recorded":
                pause()

        def begin_purge(self):
            super().begin_purge()
            if checkpoint == "commit-recorded":
                pause()

        def save(self, value, **options):
            super().save(value, **options)
            if checkpoint == "recovery-workspace-recorded" and value["workspace"]["path"] != str(
                work
            ):
                pause()
            if checkpoint == "restored-recorded" and value["phase"] == "restored":
                pause()

        def finish(self, phase):
            if checkpoint == "recovery-workspace-cleaned":
                pause()
            super().finish(phase)

    with (
        RemovalFiles(targets) as files,
        Journal(root / "Support", targets, workspace_root=root) as journal,
    ):
        inventory = files.prepare()
        before = {"fixture": True}
        if checkpoint.startswith("recovery-"):
            document = root / "recovery-routes.json"
            document.write_text("preserve desired graph")
            before = Host(document).state()
        plan = {
            "version": 1,
            "mode": "loopz-uninstall",
            "targets": {k: str(v) for k, v in targets.items()},
            "inventory": inventory,
            "before": before,
            "preserve": [],
        }
        journal.begin(plan, files, work)
        if checkpoint == "journal-recorded":
            pause()
        rename = uninstall_files.rename_exclusive

        def rename_and_pause(source, destination, name, *args):
            rename(source, destination, name, *args)
            if checkpoint == "first-rename" and name == "LoopZ.app":
                pause()
            if checkpoint == "rollback-rename" and name == "broker":
                pause()

        if checkpoint == "first-rename":
            uninstall_files.rename_exclusive = rename_and_pause
        files.apply()
        if checkpoint.startswith("recovery-"):
            document = root / "recovery-routes.json"

            class RecoveryHost(Host):
                def require_recovery_stopped(self, state, before):
                    self.require_stopped(state)

                def restore_services(self, before):
                    super().restore_services(before)
                    if checkpoint == "recovery-files-restored":
                        pause()

                def restore_driver(self, _):
                    pass  # This process-loss fixture never accesses native jobs or audio.

            host = RecoveryHost(document)
            report = recover_product(
                journal, lambda *_: host, DiagnosticWorkspaces(root, processes=lambda _: [])
            )
            raise RuntimeError("Recovery checkpoint did not pause: " + json.dumps(report))
        if checkpoint == "all-renames":
            pause()
        if checkpoint in ("rollback-rename", "restored-recorded"):
            if checkpoint == "rollback-rename":
                uninstall_files.rename_exclusive = rename_and_pause
            files.rollback()
            journal.finish("restored")
            return
        if checkpoint == "partial-purge":
            original_remove = uninstall_files.remove_at

            def partial(parent, name, expected):
                if name == "LoopZ.app":
                    child = os.open(
                        name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=parent
                    )
                    try:
                        os.unlink("first", dir_fd=child)
                        os.fsync(child)
                    finally:
                        os.close(child)
                    pause()
                return original_remove(parent, name, expected)

            uninstall_files.remove_at = partial
        failures = files.purge()
        if failures:
            raise RuntimeError(json.dumps(failures))
        if checkpoint == "purge-complete":
            pause()
        raise ValueError("Unknown crash checkpoint")


if __name__ == "__main__":
    main()
