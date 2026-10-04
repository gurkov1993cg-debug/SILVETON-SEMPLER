from pathlib import Path
import sys

root = Path(sys.argv[1])
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
if not view.is_file():
    raise SystemExit(f'missing view: {view}')
s = view.read_text(encoding='utf-8')

if 'import android.graphics.RadialGradient' not in s:
    s = s.replace('import android.graphics.Paint\n', 'import android.graphics.Paint\nimport android.graphics.RadialGradient\n')


def replace_method(text, start_sig, next_sig, body):
    a = text.find(start_sig)
    if a < 0:
        raise SystemExit(f'missing method: {start_sig}')
    b = text.find(next_sig, a)
    if b < 0:
        raise SystemExit(f'missing next method after: {start_sig}')
    return text[:a] + body.rstrip() + '\n\n' + text[b:]

bg = r'''    private fun drawSilvetonBackground(c: Canvas, w: Float, h: Float) {
        // Exact graphite/cool-light visual language from SILVETON LIVE MIDI.
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, 0f, 0f, h, C_BG_TOP, C_BG, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, h, paint)
        paint.shader = RadialGradient(
            w * 0.5f, -h * 0.25f, w * 0.55f,
            Color.argb(28, 120, 180, 255), Color.argb(0, 120, 180, 255),
            Shader.TileMode.CLAMP
        )
        c.drawRect(0f, 0f, w, h, paint)
        paint.shader = null
    }'''

card = r'''    private fun silvCard(c: Canvas, r: RectF, base: Int, radius: Float) {
        val shadow = RectF(r.left, r.top + max(2f, radius * 0.22f), r.right, r.bottom + max(2f, radius * 0.22f))
        paint.style = Paint.Style.FILL
        paint.shader = null
        paint.color = Color.argb(155, 0, 0, 0)
        c.drawRoundRect(shadow, radius, radius, paint)

        val top = if (base == C_PANEL_DARK) Color.rgb(24, 29, 36) else Color.rgb(29, 34, 41)
        val bottom = if (base == C_PANEL_DARK) Color.rgb(13, 17, 22) else Color.rgb(18, 22, 27)
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null

        // Machined edge: pale at the top, almost invisible on the sides, dark at the bottom.
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(1f, radius * 0.055f)
        paint.shader = LinearGradient(
            0f, r.top, 0f, r.bottom,
            intArrayOf(Color.argb(46,255,255,255), Color.argb(10,255,255,255), Color.argb(120,0,0,0)),
            floatArrayOf(0f, 0.46f, 1f), Shader.TileMode.CLAMP
        )
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null
        paint.style = Paint.Style.FILL
    }'''

key = r'''    private fun silvKey(c: Canvas, r: RectF, active: Boolean, red: Boolean, enabled: Boolean = true) {
        val radius = min(r.width(), r.height()) * 0.13f
        val shadow = RectF(r.left, r.top + max(1.5f, radius * 0.20f), r.right, r.bottom + max(1.5f, radius * 0.20f))
        paint.style = Paint.Style.FILL
        paint.shader = null
        paint.color = Color.argb(if (enabled) 150 else 90, 0, 0, 0)
        c.drawRoundRect(shadow, radius, radius, paint)

        val top = if (enabled) Color.rgb(44, 50, 59) else Color.rgb(28, 32, 38)
        val bottom = if (enabled) Color.rgb(27, 31, 38) else Color.rgb(20, 23, 28)
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null

        if (active) {
            val lit = if (red) C_PANIC else C_ACCENT
            paint.shader = LinearGradient(
                0f, r.top, 0f, r.bottom,
                colorWithAlpha(lit, 0), colorWithAlpha(lit, if (red) 30 else 42),
                Shader.TileMode.CLAMP
            )
            c.drawRoundRect(r, radius, radius, paint)
            paint.shader = null
        }

        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(1f, radius * 0.07f)
        paint.shader = LinearGradient(
            0f, r.top, 0f, r.bottom,
            intArrayOf(Color.argb(if(enabled)60 else 25,255,255,255), Color.argb(8,255,255,255), Color.argb(150,0,0,0)),
            floatArrayOf(0f, 0.50f, 1f), Shader.TileMode.CLAMP
        )
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null

        if (active) {
            paint.color = if (red) Color.argb(185,229,72,77) else Color.argb(185,47,127,214)
            paint.strokeWidth = max(1f, radius * 0.055f)
            c.drawRoundRect(r, radius, radius, paint)
        }
        paint.style = Paint.Style.FILL
    }'''

led = r'''    private fun silvLed(c: Canvas, x: Float, y: Float, on: Boolean, red: Boolean, radius: Float) {
        val color = if (red) C_PANIC else C_ACCENT
        paint.style = Paint.Style.FILL
        if (on) {
            paint.shader = RadialGradient(x, y, radius * 3.2f, colorWithAlpha(color, 90), colorWithAlpha(color, 0), Shader.TileMode.CLAMP)
            c.drawCircle(x, y, radius * 3.2f, paint)
            paint.shader = RadialGradient(x - radius * 0.30f, y - radius * 0.30f, radius * 1.20f, Color.WHITE, color, Shader.TileMode.CLAMP)
        } else {
            paint.shader = RadialGradient(x - radius * 0.30f, y - radius * 0.30f, radius * 1.20f,
                Color.rgb(78,86,96), Color.rgb(34,39,46), Shader.TileMode.CLAMP)
        }
        c.drawCircle(x, y, radius, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(1f, radius * 0.18f)
        paint.color = Color.argb(160, 0, 0, 0)
        c.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.FILL
    }'''

brand = r'''    private fun drawBrand(c: Canvas, cx: Float, cy: Float, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        val waveLeft = cx - 250f * sx
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 2.4f * ss
        paint.shader = LinearGradient(waveLeft, cy - 14f*sy, waveLeft, cy + 14f*sy,
            Color.rgb(236,240,245), Color.rgb(140,150,162), Shader.TileMode.CLAMP)
        val path = android.graphics.Path()
        val points = 30
        for (i in 0..points) {
            val t = i.toFloat() / points
            val td = t.toDouble()
            val env = kotlin.math.sin(td * Math.PI).toFloat()
            val yy = cy - kotlin.math.sin(td * (Math.PI * 4.0)).toFloat() * 12f * sy * env
            val xx = waveLeft + 5f * sx + t * 44f * sx
            if (i == 0) path.moveTo(xx, yy) else path.lineTo(xx, yy)
        }
        c.drawPath(path, paint)
        paint.shader = null
        paint.style = Paint.Style.FILL

        // Metallic SILVETON wordmark copied from the LIVE MIDI visual renderer,
        // while keeping the sampler's own label/content.
        val brandX = cx - 175f * sx
        val base = cy + 10f * sy
        paint.textAlign = Paint.Align.LEFT
        paint.textSize = 34f * ss
        paint.typeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
        paint.color = Color.argb(220, 3, 5, 8)
        c.drawText("SILVETON", brandX + 1.2f*ss, base + 1.6f*ss, paint)
        paint.shader = LinearGradient(0f, base - 34f*ss, 0f, base,
            intArrayOf(Color.rgb(252,253,255), Color.rgb(200,207,215), Color.rgb(118,128,139), Color.rgb(176,184,193), Color.rgb(236,240,244)),
            null, Shader.TileMode.CLAMP)
        c.drawText("SILVETON", brandX, base, paint)
        paint.shader = null

        text(c, "sampler", cx + 36f*sx, cy + 8f*sy, 23f*ss, C_LABEL)
        val plate = RectF(cx + 132f*sx, cy - 13f*sy, cx + 224f*sx, cy + 13f*sy)
        paint.shader = LinearGradient(0f, plate.top, 0f, plate.bottom,
            Color.argb(28,255,255,255), Color.argb(8,255,255,255), Shader.TileMode.CLAMP)
        c.drawRoundRect(plate, 13f*ss, 13f*ss, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f * ss
        paint.color = Color.argb(55,255,255,255)
        c.drawRoundRect(plate, 13f*ss, 13f*ss, paint)
        paint.style = Paint.Style.FILL
        text(c, "version 1", plate.centerX(), plate.centerY() + 4f*sy, 12f*ss, C_LABEL, alignCenter=true)
    }'''

status = r'''    private fun silvStatus(c: Canvas, r: RectF, label: String, value: String, on: Boolean, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, Color.rgb(27,33,42), Color.rgb(18,23,30), Shader.TileMode.CLAMP)
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f*ss
        paint.color = Color.argb(38,255,255,255)
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.style = Paint.Style.FILL
        if (label == "MIDI") {
            silvLed(c, r.left+18f*sx, r.centerY(), on, false, 5.5f*ss)
            text(c, label, r.left+33f*sx, r.centerY()+4f*sy, 12f*ss, if(on) C_TEXT else C_TEXT_MUTED, bold=on)
        } else {
            text(c, label, r.left+13f*sx, r.centerY()-3f*sy, 10f*ss, C_TEXT_MUTED, bold=true)
            text(c, value, r.left+13f*sx, r.centerY()+14f*sy, 17f*ss, C_TEXT, bold=true)
        }
    }'''

s = replace_method(s, '    private fun drawSilvetonBackground(', '    private fun silvCard(', bg)
s = replace_method(s, '    private fun silvCard(', '    private fun silvKey(', card)
s = replace_method(s, '    private fun silvKey(', '    private fun silvLed(', key)
s = replace_method(s, '    private fun silvLed(', '    private fun drawGear(', led)
s = replace_method(s, '    private fun drawBrand(', '    private fun silvStatus(', brand)
s = replace_method(s, '    private fun silvStatus(', '    private fun lighten(', status)

# Helper used by the skin only; do not touch app content/state.
anchor = '    private fun lighten(color: Int, amount: Int): Int = Color.rgb('
if 'private fun colorWithAlpha(' not in s:
    i = s.find(anchor)
    if i < 0:
        raise SystemExit('lighten anchor missing')
    helper = '    private fun colorWithAlpha(color: Int, alpha: Int): Int = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))\n\n'
    s = s[:i] + helper + s[i:]

s = s.replace(
    ' * Visual baseline: SILVETON-sampler-3.zip / ui/list.html (machined graphite SILVETON skin).\n',
    ' * Visual baseline: SILVETON LIVE MIDI native Canvas renderer; sampler labels/content remain unchanged.\n'
)

required = [
    'RadialGradient(',
    'private fun colorWithAlpha(',
    'Color.rgb(252,253,255)',
    'Color.argb(28, 120, 180, 255)',
    'sampler labels/content remain unchanged',
]
for needle in required:
    if needle not in s:
        raise SystemExit(f'visual skin contract missing: {needle}')

view.write_text(s, encoding='utf-8')
print('PASS: SILVETON LIVE MIDI visual shell applied to Sampler without changing app content')
