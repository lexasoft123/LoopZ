// A recovered backup can be older than the user's last saved routing.
export function backupRecoveryNotice(state) {
    if (
        !state.connected ||
        !state.snapshot?.document ||
        state.snapshot.runtime?.documentError ||
        state.snapshot.restoration?.recoveredBackup !== true
    )
        return null;
    return {
        heading: "Routing restored from backup",
        detail: "The latest saved routing could not be read. LoopZ loaded its last valid backup, which may be older. Review your devices and connections before making changes.",
    };
}
