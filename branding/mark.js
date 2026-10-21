// One route geometry for the Dock forge and the small header mark.
const segments = [
    ["M", 14, 11],
    ["L", 42, 11],
    ["C", 49, 11, 52, 19, 46, 23],
    ["L", 18, 41],
    ["C", 12, 45, 15, 53, 22, 53],
    ["L", 50, 53],
];
export const SIGNAL_PATH = segments
    .map(([command, ...values]) => command + values.join(" "))
    .join(" ");
export function signalPath(context, x, y, scale) {
    for (const [command, ...values] of segments) {
        const points = values.map(
            (value, index) => value * scale + (index % 2 ? y : x),
        );
        context[{ M: "moveTo", L: "lineTo", C: "bezierCurveTo" }[command]](
            ...points,
        );
    }
}
