const helpers = `
const checks = [];
const check = (value, message) => { if (!value) throw new Error(message); checks.push(message); };
const text = () => document.body.innerText;
const until = async predicate => {
    const end = performance.now() + 10000;
    while (!await predicate()) {
        if (performance.now() > end) throw new Error(JSON.stringify({checks, body: text().slice(0, 2000)}));
        await new Promise(resolve => setTimeout(resolve, 20));
    }
};
const state = () => window.webkit.messageHandlers.loopz.postMessage({version: 1, operation: "state"});
const fixture = envelope => window.webkit.messageHandlers.fixture.postMessage({operation: "replace-envelope", envelope});
const canonical = value => Array.isArray(value) ? value.map(canonical) : value && typeof value === "object" ? Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])])) : value;
await until(() => text().includes("Settings"));
`;

export const beforeDocumentRecoveryReload = `${helpers}
const data = await state();
const saved = JSON.stringify(canonical(data.snapshot.document));
data.snapshot.restoration.recoveredBackup = true;
await fixture(data);
await until(() => text().includes("Routing restored from backup"));
check(text().includes("may be older") && text().includes("Review your devices and connections"), "Backup guidance explains that recovered routing may be older");
check(document.querySelectorAll(".lz-sources, .lz-outputs, .lz-monitors").length === 3, "Recovered routing keeps the Inputs, Outputs and Monitors columns available for review");
data.snapshot.runtime.documentError = "Owned invalid saved document";
await fixture(data);
await until(() => text().includes("Your files are preserved"));
check(!text().includes("Routing restored from backup"), "A later document error takes precedence over a stale backup flag");
data.snapshot.runtime.documentError = "";
data.snapshot.restoration.recoveredBackup = false;
await fixture(data);
await until(() => !text().includes("Your files are preserved"));
check(!text().includes("Routing restored from backup"), "Primary-file recovery clears backup guidance automatically");
check(JSON.stringify(canonical((await state()).snapshot.document)) === saved, "Recovery guidance preserves every source, control and connection");
return {checks};
`;

export const afterDocumentRecoveryReload = `${helpers}
const data = await state();
data.snapshot.restoration.recoveredBackup = true;
data.connection.connected = false;
data.connection.ready = false;
await fixture(data);
await until(() => text().includes("Connecting to LoopZ"));
check(!text().includes("Routing restored from backup"), "Disconnected cached state cannot claim current backup recovery");
data.connection.connected = true;
data.connection.ready = true;
data.snapshot.document = null;
data.snapshot.canCreateDocument = false;
data.snapshot.runtime.documentError = "Owned unreadable document at first launch";
await fixture(data);
await until(() => text().includes("Your files are preserved"));
check(!text().includes("The last working graph is kept"), "Cold-start damage does not claim a graph is already running");
const commands = await window.webkit.messageHandlers.fixture.postMessage({operation: "commands"});
check(commands.length === 0, "Recovery guidance never overwrites files or silently edits routing");
return {checks, snapshotContext: "Unreadable saved document at first launch in a private fixture"};
`;
