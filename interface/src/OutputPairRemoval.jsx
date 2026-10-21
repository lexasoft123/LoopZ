import React, { useMemo } from "react";
import { Banner, Button, ModalActions } from "@singz/ui";
import { outputPairRemoval } from "./output-pairs.js";

export default function OutputPairRemoval({
    state,
    device,
    start,
    review,
    writable,
    cancel,
    commit,
}) {
    const devices = state.devices;
    const deviceId = device.id;
    const current = useMemo(() => {
        try {
            return outputPairRemoval({ devices }, deviceId, start);
        } catch {
            return null; // The saved layout no longer contains the reviewed pair.
        }
    }, [devices, deviceId, start]);
    const available =
        state.canRemoveOutputPair && current && review?.basis === current.basis;
    return (
        <>
            <p className="modal-body">
                Remove this pair from {device.name}. Connections attached to
                these channels will be removed. Remaining channels keep their
                order and are renumbered from 1 to {review?.channels}.
            </p>
            <p>
                {review?.removedConnections}{" "}
                {review?.removedConnections === 1
                    ? "connection"
                    : "connections"}{" "}
                removed. {review?.shiftedConnections} surviving{" "}
                {review?.shiftedConnections === 1
                    ? "connection follows"
                    : "connections follow"}{" "}
                the renumbered channels.
                {review?.dependentDevices > 0 && (
                    <>
                        {" "}
                        Sources in {review.dependentDevices} other virtual{" "}
                        {review.dependentDevices === 1
                            ? "device are"
                            : "devices are"}{" "}
                        updated too.
                    </>
                )}
            </p>
            {!available && (
                <Banner tone="warning" heading="Review this pair again">
                    {state.canRemoveOutputPair
                        ? "Routing changed. Cancel and reopen this pair to review the current connections."
                        : "Update LoopZ before removing individual output pairs."}
                </Banner>
            )}
            <ModalActions>
                {cancel}
                <Button
                    variant="danger"
                    disabled={!writable || !available}
                    onClick={() =>
                        commit({
                            type: "remove-output-pair",
                            start,
                            basis: review?.basis,
                        })
                    }
                >
                    Remove pair
                </Button>
            </ModalActions>
        </>
    );
}
