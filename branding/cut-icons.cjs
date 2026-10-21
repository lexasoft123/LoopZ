/*
 * Cut a forge page into build/icon.icns, build/icon.ico and build/icon.png.
 *
 *   <app>/node_modules/.bin/electron cut-icons.cjs <forge.html> <out-dir>
 *
 * Electron, deliberately: it renders with the SAME Chromium the app ships, so
 * what the forge draws is exactly what the user sees. A different rasteriser
 * gets blur radii and gradient interpolation subtly different, and you find
 * out at 1024 px.
 *
 * (Note this uses canvas.toDataURL(), not capturePage(). The canvas is N x N
 * device pixels by construction, so display scale cannot get into it — which
 * is NOT true of capturePage, whose offscreen path returns the host's Retina
 * scale and ignores --force-device-scale-factor.)
 *
 * The forge page must expose:
 *   window.drawIcon(size, variant) -> dataURL     // variant: 'full' | 'small'
 */
const { app, BrowserWindow } = require('electron')
const { execFileSync } = require('node:child_process')
const { mkdirSync, rmSync, writeFileSync } = require('node:fs')
const path = require('node:path')

const [FORGE, OUT = 'build'] = process.argv.slice(2).filter((a) => !a.startsWith('--'))
if (!FORGE) {
  console.error('usage: electron cut-icons.cjs <forge.html> [out-dir]')
  process.exit(2)
}

/**
 * Below this, the full mark turns to mush and a simplified one reads better.
 * Apple ships two drawings of its own icons for the same reason. Keep them the
 * same family — same tile, same light, same silhouette idea — or it looks like
 * a different app in the Finder sidebar.
 */
const SMALL_AT = 32
const variantFor = (px) => (px <= SMALL_AT ? 'small' : 'full')

/** macOS wants @1x and @2x of each logical size. */
const ICNS = [
  ['icon_16x16', 16], ['icon_16x16@2x', 32],
  ['icon_32x32', 32], ['icon_32x32@2x', 64],
  ['icon_128x128', 128], ['icon_128x128@2x', 256],
  ['icon_256x256', 256], ['icon_256x256@2x', 512],
  ['icon_512x512', 512], ['icon_512x512@2x', 1024]
]
const ICO = [16, 24, 32, 48, 64, 128, 256]

/**
 * Write a multi-size .ico. The format is a 6-byte header, a 16-byte directory
 * entry per image, then the images — and since Vista an entry may hold a PNG
 * verbatim, so no BMP encoding is needed. 256 is stored as 0 in the size byte,
 * which is the one thing that catches people.
 */
function ico(pngs) {
  const head = Buffer.alloc(6)
  head.writeUInt16LE(0, 0)            // reserved
  head.writeUInt16LE(1, 2)            // 1 = icon
  head.writeUInt16LE(pngs.length, 4)
  const dir = Buffer.alloc(16 * pngs.length)
  let offset = head.length + dir.length
  pngs.forEach(({ size, data }, i) => {
    const o = i * 16
    dir.writeUInt8(size >= 256 ? 0 : size, o)      // width
    dir.writeUInt8(size >= 256 ? 0 : size, o + 1)  // height
    dir.writeUInt8(0, o + 2)                       // palette size
    dir.writeUInt8(0, o + 3)                       // reserved
    dir.writeUInt16LE(1, o + 4)                    // colour planes
    dir.writeUInt16LE(32, o + 6)                   // bits per pixel
    dir.writeUInt32LE(data.length, o + 8)
    dir.writeUInt32LE(offset, o + 12)
    offset += data.length
  })
  return Buffer.concat([head, dir, ...pngs.map((p) => p.data)])
}

app.disableHardwareAcceleration()
app.whenReady().then(async () => {
  const win = new BrowserWindow({ show: false, width: 64, height: 64, webPreferences: { offscreen: true } })
  await win.loadFile(path.resolve(FORGE))

  const png = async (px) => {
    const url = await win.webContents.executeJavaScript(
      `window.drawIcon(${px}, ${JSON.stringify(variantFor(px))})`)
    return Buffer.from(url.split(',')[1], 'base64')
  }

  const out = path.resolve(OUT)
  mkdirSync(out, { recursive: true })

  // ── macOS ──
  const set = path.join(out, '_iconset.iconset')
  rmSync(set, { recursive: true, force: true })
  mkdirSync(set, { recursive: true })
  for (const [name, px] of ICNS) writeFileSync(path.join(set, `${name}.png`), await png(px))
  execFileSync('iconutil', ['-c', 'icns', set, '-o', path.join(out, 'icon.icns')], { stdio: 'inherit' })
  rmSync(set, { recursive: true, force: true })
  console.log('wrote', path.join(OUT, 'icon.icns'))

  // ── Windows ──
  const pngs = []
  for (const px of ICO) pngs.push({ size: px, data: await png(px) })
  writeFileSync(path.join(out, 'icon.ico'), ico(pngs))
  console.log('wrote', path.join(OUT, 'icon.ico'), `(${ICO.join(', ')})`)

  // ── anything that just wants an image ──
  writeFileSync(path.join(out, 'icon.png'), await png(1024))
  console.log('wrote', path.join(OUT, 'icon.png'))

  win.destroy()
  app.quit()
})
