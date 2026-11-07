// WebKitGTK's reply-capable message handler has the same Promise contract as
// WKWebView. The native host alone chooses the Unix socket; UI code never gets
// filesystem or PCM access.
export async function linuxTransport(message, scope = globalThis) {
    const bridge = scope.webkit?.messageHandlers?.loopz;
    if (!bridge || typeof bridge.postMessage !== "function")
        throw new Error(
            "Open LoopZ in its desktop application to connect to audio.",
        );
    return await bridge.postMessage(message);
}
