const legacyRates = Object.freeze([44100, 48000]);

export function supportedSampleRates(snapshot) {
    const rates = snapshot?.runtime?.sampleRates;
    if (
        !Array.isArray(rates) ||
        !rates.length ||
        rates.length > 32 ||
        rates.some(
            (rate) =>
                !Number.isSafeInteger(rate) || rate < 8000 || rate > 192000,
        ) ||
        new Set(rates).size !== rates.length
    )
        return legacyRates;
    return [...rates].sort((a, b) => a - b);
}

export function initialDeviceRate(state) {
    const rates = state.sampleRates;
    const output = state.inventory?.devices?.filter(
        (device) => device.defaultOutput === true,
    );
    if (
        state.inventory?.devicesFresh !== false &&
        !state.inventory?.devicesError &&
        output?.length === 1 &&
        rates.includes(output[0].rate)
    )
        return output[0].rate;
    return rates.includes(48000) ? 48000 : rates[0];
}

export const sampleRateLabel = (rate) => `${rate / 1000} kHz`;
