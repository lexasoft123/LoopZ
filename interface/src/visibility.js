// Native window occlusion is distinct from application focus. A background
// window can remain visible and must keep its meters running.
export function observeUIVisibility(
    changed,
    { page = globalThis.window, document = globalThis.document } = {},
) {
    let previous;
    const update = () => {
        const visible =
            !document?.hidden && page?.__loopzNativeVisible !== false;
        if (visible === previous) return;
        previous = visible;
        if (document?.documentElement)
            document.documentElement.dataset.loopzUiVisible = String(visible);
        changed(visible);
    };
    page?.addEventListener("loopz-visibilitychange", update);
    document?.addEventListener("visibilitychange", update);
    update();
    return () => {
        page?.removeEventListener("loopz-visibilitychange", update);
        document?.removeEventListener("visibilitychange", update);
    };
}

export function startVisiblePolling(
    read,
    { interval, observe = observeUIVisibility, changed = () => {} },
) {
    let stopped = false;
    let visible = false;
    let generation = 0;
    let reading = false;
    let timer;
    const poll = async () => {
        if (stopped || !visible || reading) return;
        reading = true;
        const current = generation;
        const isCurrent = () => !stopped && visible && current === generation;
        let accepted;
        try {
            accepted = await read(isCurrent);
        } catch {
            // Consumers own their error UI. An unexpected rejection must not
            // create an unhandled promise or an immediate busy retry loop.
        } finally {
            reading = false;
            if (!stopped && visible) {
                // A show event during an older read must refresh immediately,
                // not reuse the old response or wait another polling interval.
                if (current !== generation || accepted === false) void poll();
                else timer = setTimeout(poll, interval);
            }
        }
    };
    const detach = observe((next) => {
        if (stopped || (next === visible && generation !== 0)) return;
        visible = next;
        ++generation;
        clearTimeout(timer);
        changed(visible);
        if (visible) void poll();
    });
    return () => {
        if (stopped) return;
        stopped = true;
        ++generation;
        clearTimeout(timer);
        detach();
        changed(false);
    };
}
