import React, { useEffect, useSyncExternalStore } from "react";
import { createRoot } from "react-dom/client";
import "@singz/ui/kit.css";
import "./layout.css";
import "./routing.css";
import "./dialogs.css";
import "./desktop.css";
import "./components/switch.css";
import "./components/startup-tour.css";
import "./components/level-meter.css";
import { EngineStore, nativeTransport } from "./store.js";
import App from "./App.jsx";

let preferences = {};
try {
    preferences = JSON.parse(localStorage.getItem("loopz-appearance") || "{}");
} catch {}
const store = new EngineStore(nativeTransport, preferences, (value) => {
    try {
        localStorage.setItem("loopz-appearance", JSON.stringify(value));
    } catch {}
});
function ConnectedApp() {
    const state = useSyncExternalStore(store.subscribe, store.getSnapshot);
    useEffect(() => store.start(), []);
    return (
        <App
            state={state}
            send={store.send}
            transport={nativeTransport}
            refresh={() => store.refresh()}
        />
    );
}
createRoot(document.getElementById("root")).render(<ConnectedApp />);
