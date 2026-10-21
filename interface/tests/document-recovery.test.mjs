import test from "node:test";
import assert from "node:assert/strict";
import { backupRecoveryNotice } from "../src/document-recovery.js";
import { project } from "../src/model.js";
import { engineFixture } from "./fixtures.mjs";

test("an authenticated backup recovery warns that saved routing can be older", () => {
    const envelope = engineFixture();
    envelope.snapshot.restoration.recoveredBackup = true;
    const before = structuredClone(envelope);
    const notice = backupRecoveryNotice(project(envelope));
    assert.equal(notice.heading, "Routing restored from backup");
    assert.match(notice.detail, /may be older/);
    assert.match(notice.detail, /Review your devices and connections/);
    assert.deepEqual(envelope, before);
});

test("a cached backup flag cannot claim recovery while disconnected or unreadable", () => {
    const envelope = engineFixture();
    envelope.snapshot.restoration.recoveredBackup = true;
    envelope.connection.connected = false;
    envelope.connection.ready = false;
    assert.equal(backupRecoveryNotice(project(envelope)), null);
    envelope.connection.connected = true;
    envelope.connection.ready = true;
    envelope.snapshot.runtime.documentError =
        "Both saved files could not be loaded";
    assert.equal(backupRecoveryNotice(project(envelope)), null);
    envelope.snapshot.runtime.documentError = "";
    envelope.snapshot.document = null;
    assert.equal(backupRecoveryNotice(project(envelope)), null);
});

test("a repaired primary clears backup guidance, and old Engines remain compatible", () => {
    const envelope = engineFixture();
    envelope.snapshot.restoration.recoveredBackup = false;
    assert.equal(backupRecoveryNotice(project(envelope)), null);
    delete envelope.snapshot.restoration.recoveredBackup;
    assert.equal(backupRecoveryNotice(project(envelope)), null);
});
