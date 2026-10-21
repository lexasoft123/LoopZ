/*
 * Liquid glass app icon — the drawing kit.
 *
 * A night-studio icon is three layers, always in this order:
 *
 *   tile(ctx, S)              a deep tungsten slab, cut to the macOS squircle
 *   <your mark>               drawn with lit() so it reads as light, not paint
 *   glass(ctx, S, pad, size)  the sheet of glass over both
 *
 * The mark is the only per-app part. Everything else is the language, and it
 * is what makes two different apps look like they came from the same studio.
 *
 * Canvas rather than SVG on purpose: the glass wants real blur, additive
 * bloom and gradient-along-a-stroke, and a hand-authored SVG of that is a
 * wall of filter primitives nobody will edit twice.
 *
 * No imports, no build step — load it from a plain <script type="module"> in
 * whatever browser or Electron window you are rendering with.
 */

/** The kit's accent, and the two ends of the light it throws. */
export const AMBER = '#ffa028'
export const HOT = '#ffe0b0'
export const DEEP = '#a8500f'

/**
 * macOS-style superellipse. Draw it yourself: macOS does NOT mask an .icns,
 * so a plain rounded rect ships looking subtly wrong next to every system
 * icon, and `border-radius` gets the corner curvature wrong in a way people
 * notice without being able to name.
 */
export function squircle(ctx, x, y, size, n = 5) {
  const r = size / 2, cx = x + r, cy = y + r
  ctx.beginPath()
  for (let i = 0; i <= 720; i++) {
    const t = i * Math.PI / 360, c = Math.cos(t), s = Math.sin(t)
    const px = cx + r * Math.sign(c) * Math.pow(Math.abs(c), 2 / n)
    const py = cy + r * Math.sign(s) * Math.pow(Math.abs(s), 2 / n)
    i ? ctx.lineTo(px, py) : ctx.moveTo(px, py)
  }
  ctx.closePath()
}

/**
 * The slab. Returns {pad, size} — the mark and the glass both need them.
 *
 * Kept genuinely dark (#241e18 -> #0b0a08) so the mark is the only lit thing
 * in the frame. The instinct is to lift the tile so it "reads better"; it does
 * the opposite, because the accent then has nothing to be brighter than.
 */
export function tile(ctx, S, opts = {}) {
  const pad = S * (opts.pad ?? 0.094)
  const size = S - pad * 2
  ctx.save()
  ctx.shadowColor = 'rgba(0,0,0,.5)'
  ctx.shadowBlur = S * 0.045
  ctx.shadowOffsetY = S * 0.022
  squircle(ctx, pad, pad, size)
  ctx.fillStyle = '#0f0d0b'
  ctx.fill()
  ctx.restore()

  squircle(ctx, pad, pad, size)
  ctx.save(); ctx.clip()
  const g = ctx.createLinearGradient(pad, pad, pad, pad + size)
  g.addColorStop(0, opts.top ?? '#241e18')
  g.addColorStop(0.42, opts.mid ?? '#15120f')
  g.addColorStop(1, opts.bottom ?? '#0b0a08')
  ctx.fillStyle = g
  ctx.fillRect(pad, pad, size, size)
  ctx.restore()
  return { pad, size }
}

/**
 * Draw a shape as LIGHT. Three passes, and all three earn their place:
 *
 *   1. a blurred amber bloom   — light escaping through the glass
 *   2. the solid form          — gradient top-lit, hot to deep
 *   3. a sheen offset to the light, blurred
 *
 * Pass 3 is the one people skip and then wonder why the mark looks flat. Keep
 * its offset small: far enough to see as a separate line and the shape reads
 * as doubled, which is a different and worse mistake.
 *
 *   lit(ctx, z, c => { c.arc(x, y, r, 0, 7) }, { w: 0.07 })        // stroked
 *   lit(ctx, z, c => { c.roundRect(x, y, w, h, r) }, { fill: true })
 *
 * `z` is the tile's `size`, so every width is a fraction of the tile and the
 * mark scales with it.
 */
export function lit(ctx, z, draw, o = {}) {
  const w = (o.w ?? 0.085) * z
  const mode = o.fill ? 'fill' : 'stroke'
  const run = () => { ctx.beginPath(); draw(ctx); mode === 'fill' ? ctx.fill() : ctx.stroke() }

  ctx.save()
  ctx.lineCap = o.cap ?? 'round'
  ctx.lineJoin = 'round'

  ctx.filter = `blur(${z * (o.bloom ?? 0.030)}px)`
  ctx.strokeStyle = 'rgba(255,150,35,.50)'
  ctx.fillStyle = 'rgba(255,150,35,.50)'
  ctx.lineWidth = w * 1.75
  run()

  ctx.filter = 'none'
  const g = ctx.createLinearGradient(0, z * 0.20, 0, z * 0.88)
  g.addColorStop(0, o.hot ?? HOT)
  g.addColorStop(0.34, o.accent ?? AMBER)
  g.addColorStop(1, o.deep ?? DEEP)
  ctx.strokeStyle = g
  ctx.fillStyle = g
  ctx.lineWidth = w
  run()

  ctx.filter = `blur(${w * 0.17}px)`
  ctx.strokeStyle = 'rgba(255,244,222,.30)'
  ctx.fillStyle = 'rgba(255,244,222,.26)'
  ctx.lineWidth = w * 0.30
  ctx.translate(-w * 0.11, -w * 0.12)
  run()
  ctx.restore()
}

/** A dark recess — a socket, a hole, anything the light sits inside. */
export function bore(ctx, cx, cy, r) {
  const g = ctx.createRadialGradient(cx - r * 0.2, cy - r * 0.25, r * 0.05, cx, cy, r)
  g.addColorStop(0, '#1b1410')
  g.addColorStop(0.6, '#0a0705')
  g.addColorStop(1, '#030202')
  ctx.fillStyle = g
  ctx.beginPath(); ctx.arc(cx, cy, r, 0, 7); ctx.fill()
  ctx.strokeStyle = 'rgba(0,0,0,.6)'
  ctx.lineWidth = r * 0.10
  ctx.beginPath(); ctx.arc(cx, cy, r, 0, 7); ctx.stroke()
}

/**
 * The glass, over everything. Call it LAST.
 *
 * Five things, and the second is the one that actually sells it: the broad
 * specular is COOL (#e2eeff) against warm content. That temperature contrast
 * is what the eye reads as a sheet of glass; a warm highlight over a warm
 * tile just looks like a lighter tile.
 */
export function glass(ctx, S, pad, size) {
  squircle(ctx, pad, pad, size)
  ctx.save(); ctx.clip()

  // 1. caustic — light that came through and pooled opposite the source
  let g = ctx.createRadialGradient(pad + size * .72, pad + size * .80, 0,
                                   pad + size * .72, pad + size * .80, size * .46)
  g.addColorStop(0, 'rgba(255,170,70,.13)')
  g.addColorStop(1, 'rgba(255,170,70,0)')
  ctx.fillStyle = g; ctx.fillRect(pad, pad, size, size)

  // 2. the broad specular, cool against the warm content
  g = ctx.createLinearGradient(pad, pad, pad + size * .62, pad + size * .88)
  g.addColorStop(0, 'rgba(226,238,255,.20)')
  g.addColorStop(0.42, 'rgba(226,238,255,.05)')
  g.addColorStop(1, 'rgba(255,255,255,0)')
  ctx.fillStyle = g; ctx.fillRect(pad, pad, size, size)

  // 3. the wet highlight hugging the top edge
  ctx.filter = `blur(${size * 0.010}px)`
  g = ctx.createLinearGradient(pad, pad, pad, pad + size * .13)
  g.addColorStop(0, 'rgba(255,255,255,.42)')
  g.addColorStop(1, 'rgba(255,255,255,0)')
  ctx.fillStyle = g; ctx.fillRect(pad, pad, size, size * .15)
  ctx.filter = 'none'

  // 4. inner shadow under the top edge — this is what gives it thickness
  ctx.save()
  ctx.filter = `blur(${size * 0.03}px)`
  ctx.strokeStyle = 'rgba(0,0,0,.5)'
  ctx.lineWidth = size * 0.055
  squircle(ctx, pad - size * 0.02, pad - size * 0.045, size * 1.04)
  ctx.stroke()
  ctx.restore()

  g = ctx.createRadialGradient(pad + size / 2, pad + size * .44, size * .24,
                               pad + size / 2, pad + size / 2, size * .80)
  g.addColorStop(0, 'rgba(0,0,0,0)')
  g.addColorStop(1, 'rgba(0,0,0,.5)')
  ctx.fillStyle = g; ctx.fillRect(pad, pad, size, size)
  ctx.restore()

  // 5. fresnel rim — bright where the glass turns away from you
  ctx.save()
  squircle(ctx, pad, pad, size)
  ctx.lineWidth = Math.max(1, size * 0.0085)
  g = ctx.createLinearGradient(pad, pad, pad + size, pad + size)
  g.addColorStop(0, 'rgba(255,255,255,.55)')
  g.addColorStop(0.38, 'rgba(255,255,255,.08)')
  g.addColorStop(0.72, 'rgba(255,170,80,.10)')
  g.addColorStop(1, 'rgba(255,190,110,.34)')
  ctx.strokeStyle = g
  ctx.stroke()
  ctx.restore()
}

/**
 * The whole icon: tile, your mark, glass.
 *
 *   paint(ctx, 1024, (c, pad, size) => { ...your mark... })
 *
 * The mark gets `pad` and `size` so it can position against the tile rather
 * than the canvas — that is what lets the same mark render at 16 and 1024.
 */
export function paint(ctx, S, mark, opts) {
  const t = tile(ctx, S, opts)
  mark(ctx, t.pad, t.size)
  glass(ctx, S, t.pad, t.size)
  return t
}
