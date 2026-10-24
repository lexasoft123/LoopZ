"""Service/workspace finalization around durable uninstall file recovery."""

from uninstall_policy import validate_preserved


def recover_product(journal, host_factory, workspaces):
    """Caller holds the installer lock; source paths come only from validated journal state."""
    value = journal.load()
    report_owner = value["plan"].get("before", {}).get("consoleUID")
    if type(report_owner) is int and report_owner > 0:
        workspaces.report_owner = report_owner
    committed = value["phase"] in ("purging", "removed")
    result = {
        "status": "recoveryRequired",
        "mode": "loopz-uninstall-recovery",
        "committed": committed,
        "journalRetained": True,
    }
    guard = None
    work = value["workspace"]
    try:
        with journal.adopt() as files:
            if committed:
                # The guard was closed before the durable commit. Do not send
                # a stop request to a subsequently launched diagnostic.
                workspaces.quiesce(work, request_stop=False)
                failures = files.purge(allow_partial=True) if files.moved else []
                files.clean_empty_quarantines()
                if failures:
                    result["cleanupRequired"] = failures
                    raise RuntimeError("Committed original-file cleanup remains incomplete")
                workspaces.clean(work)
                journal.finish("removed")
                result.update(
                    status="committedRemovalRecovered",
                    journalRetained=False,
                    newTargetsPreserved=[
                        name
                        for name, path in journal.targets.items()
                        if path.exists() or path.is_symlink()
                    ],
                )
                return result

            before = value["plan"].get("before")
            host = host_factory(files, before)
            workspaces.quiesce(work, request_stop=True)
            workspaces.clean(work)
            new_path = workspaces.create()
            work = workspaces.describe(new_path)
            journal.replace_workspace(new_path)
            host.prepare_tools(new_path)
            baseline = host.state()
            host.require_recovery_stopped(baseline, before)
            guard = host.start_guard(new_path, baseline["catalog"])
            guard.ready()
            current = host.state()
            host.require_recovery_stopped(current, before)
            validate_preserved(baseline, current)
            files.rollback()
            host.verify_components(files.originals)
            host.restore_services(before)
            host.restore_driver(files)
            after = host.state()
            host.require_stopped(after)
            validate_preserved(baseline, after)
            guard.check()
            continuity = guard.stop()
            if continuity.get("passed") is not True:
                raise RuntimeError("Recovery output continuity did not pass")
            guard.close()
            guard = None
            workspaces.clean(work)
            journal.finish("restored")
            result.update(
                status="originalInstallationRecovered",
                journalRetained=False,
                beforeRecovery=baseline,
                afterRecovery=after,
                documentsChangedSinceOriginalReview=before.get("documents")
                != baseline["documents"],
                hardwareContinuity=continuity,
            )
    except BaseException as error:
        result["error"] = str(error) or type(error).__name__
    finally:
        if guard:
            try:
                guard.close()
            except BaseException as error:
                result.setdefault("repairRequired", []).append(
                    "close recovery diagnostic: " + str(error)
                )
    if result["journalRetained"]:
        result["retainedWorkspace"] = work
    return result
