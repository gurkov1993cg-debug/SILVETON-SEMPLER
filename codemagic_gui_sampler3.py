from pathlib import Path
import sys

root = Path(sys.argv[1])
view = root / 'android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt'
if not view.is_file():
    raise SystemExit(f'missing view: {view}')
s = view.read_text(encoding='utf-8')

if 'import android.graphics.LinearGradient' not in s:
    s = s.replace('import android.graphics.Color\n', 'import android.graphics.Color\nimport android.graphics.LinearGradient\n')
if 'import android.graphics.Shader' not in s:
    s = s.replace('import android.graphics.RectF\n', 'import android.graphics.RectF\nimport android.graphics.Shader\n')


def replace_method(text, start_sig, next_sig, body):
    a = text.find(start_sig)
    if a < 0:
        raise SystemExit(f'missing method: {start_sig}')
    b = text.find(next_sig, a)
    if b < 0:
        raise SystemExit(f'missing next method after: {start_sig}')
    return text[:a] + body.rstrip() + '\n\n' + text[b:]

on_draw = r'''    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        hits.clear()
        frameAudioStatus = sampler.audioStatus()
        frameUpperMeters = sampler.upperMeters()

        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return

        val sx = w / DESIGN_W
        val sy = h / DESIGN_H
        drawSilvetonBackground(canvas, w, h)
        drawHeader(canvas, w, h)

        val marginX = 16f * sx
        val bodyTop = 90f * sy
        val bottom = h - 14f * sy
        val navW = 178f * sx
        val gap = 14f * sx
        val navLeft = w - marginX - navW
        val content = RectF(marginX, bodyTop, navLeft - gap, bottom)
        val nav = RectF(navLeft, bodyTop, w - marginX, bottom)
        drawRightNav(canvas, nav)

        when (page) {
            Page.LIST -> drawList(canvas, content)
            Page.COMBI -> drawCombi(canvas, content)
            Page.EFFECTS -> drawEffects(canvas, content)
            Page.SETTINGS -> drawSettings(canvas, content)
        }

        warning?.let { msg ->
            val s = min(sx, sy)
            val box = RectF(content.left, content.bottom - 38f * sy, content.right, content.bottom)
            silvCard(canvas, box, C_WARN_BG, 10f * s)
            text(canvas, msg, box.left + 12f * sx, box.centerY() + 5f * sy, 13f * s, C_WARN_TEXT)
        }

        postInvalidateDelayed(50L)
    }'''

header = r'''    private fun drawHeader(c: Canvas, w: Float, h: Float) {
        val sx = w / DESIGN_W
        val sy = h / DESIGN_H
        val ss = min(sx, sy)
        val hh = 76f * sy

        paint.shader = LinearGradient(0f, 0f, 0f, hh, C_BG_TOP, C_BG, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, hh, paint)
        paint.shader = null
        line(c, 16f * sx, hh - 1f * sy, w - 16f * sx, hh - 1f * sy, Color.argb(28,255,255,255), 1f * ss)

        val gear = RectF(16f*sx, 12f*sy, 84f*sx, 64f*sy)
        silvKey(c, gear, page == Page.SETTINGS, false)
        drawGear(c, gear.centerX(), gear.centerY(), 12f*ss)
        hit(gear, { _, _ -> page = Page.SETTINGS; refreshGlobal() })

        val panic = RectF(96f*sx, 12f*sy, 266f*sx, 64f*sy)
        silvKey(c, panic, false, true)
        silvLed(c, panic.left + 20f*sx, panic.centerY(), true, true, 5.5f*ss)
        text(c, "PANIC", panic.centerX()+8f*sx, panic.centerY()+6f*sy, 17f*ss, C_TEXT, alignCenter=true, bold=true)
        hit(panic, { _, _ -> sampler.panic() })

        drawBrand(c, w*0.5f, 38f*sy, sx, sy)

        val status = frameAudioStatus
        val chipY = 16f*sy
        val chipH = 44f*sy
        val midiW = 112f*sx
        val coresW = 132f*sx
        val soundW = 174f*sx
        val gap = 8f*sx
        var right = w - 16f*sx

        val midiR = RectF(right-midiW, chipY, right, chipY+chipH)
        silvStatus(c, midiR, "MIDI", if (midiConnections > 0) "ON" else "OFF", midiConnections > 0, sx, sy)
        right = midiR.left-gap
        val coresR = RectF(right-coresW, chipY, right, chipY+chipH)
        silvStatus(c, coresR, "ЯДРА", "6 / 7", true, sx, sy)
        right = coresR.left-gap
        val soundR = RectF(right-soundW, chipY, right, chipY+chipH)
        silvStatus(c, soundR, "ЗВУК", "${"%.1f".format(status.estimatedLatencyMs)} ms", true, sx, sy)
    }'''

nav = r'''    private fun drawRightNav(c: Canvas, r: RectF) {
        val sx = width.toFloat() / DESIGN_W
        val sy = height.toFloat() / DESIGN_H
        val ss = min(sx, sy)
        silvCard(c, r, C_PANEL_DARK, 16f*ss)
        val gap = 12f*sy
        val padX = 14f*sx
        val padY = 16f*sy
        val labels = listOf(
            Page.LIST to "ЛИСТ",
            Page.COMBI to "КОМБИ",
            Page.EFFECTS to "ЕФЕКТИ",
            Page.SETTINGS to "НАСТРОЙКИ"
        )
        val usable = r.height() - 2f*padY - gap*(labels.size-1)
        val bh = usable / labels.size
        labels.forEachIndexed { i, (p, label) ->
            val top = r.top + padY + i*(bh+gap)
            val b = RectF(r.left+padX, top, r.right-padX, top+bh)
            silvKey(c, b, page == p, false)
            silvLed(c, b.left+14f*sx, b.centerY(), page == p, false, 5f*ss)
            text(c, label, b.centerX()+8f*sx, b.centerY()+5f*sy, 14f*ss,
                if (page == p) C_TEXT else C_TEXT_DIM, alignCenter=true, bold=page==p)
            hit(b, { _, _ ->
                page = p
                if (p == Page.COMBI) refreshAllUppers()
                if (p == Page.EFFECTS) refreshFx()
                if (p == Page.SETTINGS) refreshGlobal()
            })
        }
    }'''

list_method = r'''    private fun drawList(c: Canvas, r: RectF) {
        val sx = width.toFloat() / DESIGN_W
        val sy = height.toFloat() / DESIGN_H
        val ss = min(sx, sy)
        silvCard(c, r, C_PANEL, 16f*ss)

        val entries = sampler.listItems(0, 0, 0)
        val active = sampler.activeListEntry(0, 0, 0)
        val maxPages = 20
        listPage = listPage.coerceIn(0, maxPages-1)
        if (active >= 0 && active < entries.size && active / 10 != listPage && !isPressed) {
            listPage = active / 10
        }

        val left = r.left + 20f*sx
        val right = r.right - 20f*sx
        val titleY = r.top + 26f*sy
        silvLed(c, left+5f*sx, titleY-5f*sy, true, false, 5f*ss)
        text(c, "Л И С Т", left+20f*sx, titleY, 15f*ss, C_LABEL, bold=true)
        line(c, left+100f*sx, titleY-5f*sy, right-130f*sx, titleY-5f*sy, Color.argb(24,255,255,255), 1f*ss)
        text(c, "СТРАНИЦА ${listPage+1}", right, titleY, 13f*ss, C_TEXT_DIM, alignCenter=false)

        val activeItem = entries.getOrNull(active)
        val nowTop = r.top + 40f*sy
        val nowBottom = r.top + 112f*sy
        val currentNumber = activeItem?.number ?: 0
        text(c, if (activeItem != null) "%03d".format(currentNumber) else "–", left, nowBottom-16f*sy, 46f*ss, C_ACCENT)
        text(c, activeItem?.name ?: "Силветон", left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)

        val voicesX = right-180f*sx
        text(c, "ГЛАСОВЕ", voicesX, nowTop+18f*sy, 11f*ss, C_TEXT_MUTED)
        text(c, frameAudioStatus.activeVoices.toString(), voicesX+12f*sx, nowTop+52f*sy, 24f*ss, C_TEXT)
        val xrunX = right-82f*sx
        text(c, "ПУКАНЕ", xrunX, nowTop+18f*sy, 11f*ss, C_TEXT_MUTED)
        text(c, frameAudioStatus.xrunCount.toString(), xrunX+10f*sx, nowTop+52f*sy, 24f*ss,
            if (frameAudioStatus.xrunCount > 0) C_PANIC else C_TEXT)

        val pagesTop = r.top + 118f*sy
        val pagesH = 50f*sy
        val pageGap = 7f*sx
        val pageW = (right-left-pageGap*19f)/20f
        for (i in 0 until 20) {
            val pr = RectF(left+i*(pageW+pageGap), pagesTop, left+i*(pageW+pageGap)+pageW, pagesTop+pagesH)
            val hasData = i*10 < entries.size
            silvKey(c, pr, i == listPage, false, enabled=hasData)
            text(c, (i+1).toString(), pr.centerX(), pr.top+23f*sy, 16f*ss,
                if (i == listPage) C_TEXT else if (hasData) C_TEXT_DIM else C_FAINT, alignCenter=true, bold=i==listPage)
            val activeHere = active >= 0 && active/10 == i
            silvLed(c, pr.centerX(), pr.bottom-10f*sy, activeHere, false, 3f*ss)
            if (hasData) hit(pr, { _, _ -> listPage = i })
        }

        val gridTop = pagesTop + pagesH + 14f*sy
        val gridBottom = r.bottom - 18f*sy
        val rowGap = 10f*sy
        val colGap = 10f*sx
        val rowH = (gridBottom-gridTop-rowGap*4f)/5f
        val colW = (right-left-colGap)/2f
        val start = listPage*10

        for (j in 0 until 10) {
            val idx = start+j
            val col = j%2
            val row = j/2
            val rr = RectF(left+col*(colW+colGap), gridTop+row*(rowH+rowGap),
                left+col*(colW+colGap)+colW, gridTop+row*(rowH+rowGap)+rowH)
            val item = entries.getOrNull(idx)
            val selected = idx == active
            silvKey(c, rr, selected, false, enabled=item != null)
            silvLed(c, rr.left+18f*sx, rr.centerY(), selected, false, 5.5f*ss)
            val number = item?.number ?: (idx+1)
            text(c, "%03d".format(number), rr.left+42f*sx, rr.centerY()-8f*sy, 17f*ss,
                if (selected) C_ACCENT else if (item != null) C_ACCENT_DEEP else C_FAINT, bold=selected)
            text(c, item?.name ?: "—", rr.left+98f*sx, rr.centerY()-5f*sy, 22f*ss,
                if (selected) C_TEXT else if (item != null) C_LABEL else C_FAINT)
            if (item != null) {
                val addr = "COMBI  B${item.combiBank+1}  P${item.combiPage+1}  S${item.combiSlot+1}"
                text(c, addr, rr.left+98f*sx, rr.centerY()+19f*sy, 12f*ss, C_TEXT_MUTED)
                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) refreshAllUppers()
                })
            }
        }
    }'''

s = replace_method(s, '    override fun onDraw(canvas: Canvas) {', '    private fun drawHeader(', on_draw)
s = replace_method(s, '    private fun drawHeader(', '    private fun drawRightNav(', header)
s = replace_method(s, '    private fun drawRightNav(', '    private fun drawCombi(', nav)
s = replace_method(s, '    private fun drawList(', '    private fun drawEffects(', list_method)

helper_anchor = '    private fun panel(c:Canvas,r:RectF,color:Int,rad:Float,border:Int=C_BORDER)'
idx = s.find(helper_anchor)
if idx < 0:
    raise SystemExit('helper insertion point missing')
helpers = r'''    private fun drawSilvetonBackground(c: Canvas, w: Float, h: Float) {
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, 0f, 0f, h, C_BG_TOP, C_BG, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w, h, paint)
        paint.shader = null
    }

    private fun silvCard(c: Canvas, r: RectF, base: Int, radius: Float) {
        val top = lighten(base, 10)
        val bottom = darken(base, 8)
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        paint.shader = null
        paint.color = Color.argb(90,0,0,0)
        val shadow = RectF(r.left, r.top+5f, r.right, r.bottom+5f)
        c.drawRoundRect(shadow, radius, radius, paint)
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f
        paint.color = Color.argb(32,255,255,255)
        c.drawRoundRect(r, radius, radius, paint)
        paint.style = Paint.Style.FILL
    }

    private fun silvKey(c: Canvas, r: RectF, active: Boolean, red: Boolean, enabled: Boolean = true) {
        val radius = min(r.width(), r.height()) * 0.13f
        val top = if (enabled) Color.rgb(44,50,59) else Color.rgb(28,32,38)
        val bottom = if (enabled) Color.rgb(27,31,38) else Color.rgb(20,23,28)
        paint.style = Paint.Style.FILL
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f
        paint.color = if (active) (if (red) C_PANIC else C_ACCENT_DEEP) else Color.argb(if(enabled)38 else 18,255,255,255)
        c.drawRoundRect(r, radius, radius, paint)
        paint.style = Paint.Style.FILL
        if (active) {
            paint.color = if (red) Color.argb(44,229,72,77) else Color.argb(28,108,198,255)
            c.drawRoundRect(r, radius, radius, paint)
        }
    }

    private fun silvLed(c: Canvas, x: Float, y: Float, on: Boolean, red: Boolean, radius: Float) {
        paint.style = Paint.Style.FILL
        paint.color = if (on) (if (red) C_PANIC else C_ACCENT) else Color.rgb(55,62,70)
        if (on) {
            paint.color = if (red) Color.argb(60,229,72,77) else Color.argb(60,108,198,255)
            c.drawCircle(x, y, radius*2.0f, paint)
            paint.color = if (red) C_PANIC else C_ACCENT
        }
        c.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(1f, radius*0.18f)
        paint.color = Color.argb(150,0,0,0)
        c.drawCircle(x, y, radius, paint)
        paint.style = Paint.Style.FILL
    }

    private fun drawGear(c: Canvas, cx: Float, cy: Float, radius: Float) {
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(2f, radius*0.18f)
        paint.color = C_LABEL
        c.drawCircle(cx, cy, radius*0.55f, paint)
        c.drawCircle(cx, cy, radius*0.18f, paint)
        for (i in 0 until 8) {
            val a = Math.PI*2.0*i/8.0
            val x1 = cx + kotlin.math.cos(a).toFloat()*radius*0.72f
            val y1 = cy + kotlin.math.sin(a).toFloat()*radius*0.72f
            val x2 = cx + kotlin.math.cos(a).toFloat()*radius
            val y2 = cy + kotlin.math.sin(a).toFloat()*radius
            c.drawLine(x1,y1,x2,y2,paint)
        }
        paint.style = Paint.Style.FILL
    }

    private fun drawBrand(c: Canvas, cx: Float, cy: Float, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        val waveLeft = cx - 250f*sx
        val waveRight = waveLeft + 54f*sx
        line(c, waveLeft, cy, waveRight, cy, C_ACCENT, 2f*ss)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 2.4f*ss
        paint.color = C_LABEL
        val p = android.graphics.Path()
        val points = 30
        for (i in 0..points) {
            val t = i.toFloat()/points
            val td = t.toDouble()
            val env = kotlin.math.sin(td*Math.PI).toFloat()
            val y = cy - kotlin.math.sin(td*(Math.PI*4.0)).toFloat()*12f*sy*env
            val x = waveLeft + 5f*sx + t*44f*sx
            if (i==0) p.moveTo(x,y) else p.lineTo(x,y)
        }
        c.drawPath(p, paint)
        paint.style = Paint.Style.FILL
        text(c, "SILVETON", cx-175f*sx, cy+10f*sy, 34f*ss, C_TEXT, bold=true)
        text(c, "sampler", cx+36f*sx, cy+8f*sy, 23f*ss, C_LABEL)
        val plate = RectF(cx+132f*sx, cy-13f*sy, cx+224f*sx, cy+13f*sy)
        paint.color = Color.argb(20,255,255,255)
        c.drawRoundRect(plate, 13f*ss,13f*ss,paint)
        paint.style=Paint.Style.STROKE
        paint.strokeWidth=1f*ss
        paint.color=Color.argb(40,255,255,255)
        c.drawRoundRect(plate,13f*ss,13f*ss,paint)
        paint.style=Paint.Style.FILL
        text(c,"version 1",plate.centerX(),plate.centerY()+4f*sy,12f*ss,C_LABEL,alignCenter=true)
    }

    private fun silvStatus(c: Canvas, r: RectF, label: String, value: String, on: Boolean, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        paint.color = C_RAISED
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f*ss
        paint.color = Color.argb(26,255,255,255)
        c.drawRoundRect(r, r.height()*0.5f, r.height()*0.5f, paint)
        paint.style = Paint.Style.FILL
        if (label == "MIDI") {
            silvLed(c, r.left+18f*sx, r.centerY(), on, false, 5.5f*ss)
            text(c,label,r.left+33f*sx,r.centerY()+4f*sy,12f*ss,if(on)C_TEXT else C_TEXT_MUTED,bold=on)
        } else {
            text(c,label,r.left+13f*sx,r.centerY()-3f*sy,10f*ss,C_TEXT_MUTED,bold=true)
            text(c,value,r.left+13f*sx,r.centerY()+14f*sy,17f*ss,C_TEXT,bold=true)
        }
    }

    private fun lighten(color: Int, amount: Int): Int = Color.rgb(
        min(255, Color.red(color)+amount), min(255, Color.green(color)+amount), min(255, Color.blue(color)+amount))
    private fun darken(color: Int, amount: Int): Int = Color.rgb(
        max(0, Color.red(color)-amount), max(0, Color.green(color)-amount), max(0, Color.blue(color)-amount))

'''
s = s[:idx] + helpers + s[idx:]

old_button = '    private fun button(c:Canvas,r:RectF,label:String,active:Boolean,accent:Int=C_ACCENT){panel(c,r,if(active)C_BUTTON_ACTIVE else C_BUTTON,8f.dp,if(active)accent else C_BORDER);text(c,label,r.centerX(),r.centerY()+5f.dp,12f.dp,if(active)C_TEXT else C_TEXT_DIM,alignCenter=true,bold=active)}'
new_button = '    private fun button(c:Canvas,r:RectF,label:String,active:Boolean,accent:Int=C_ACCENT){silvKey(c,r,active,accent==C_PANIC);text(c,label,r.centerX(),r.centerY()+5f.dp,12f.dp,if(active)C_TEXT else C_TEXT_DIM,alignCenter=true,bold=active)}'
if old_button in s:
    s = s.replace(old_button, new_button)

comp_start = s.find('    companion object{')
if comp_start < 0:
    raise SystemExit('companion object missing')
comp_end = s.find('\n    }\n}', comp_start)
if comp_end < 0:
    raise SystemExit('companion end missing')
new_comp = r'''    companion object{
        private const val DESIGN_W = 1340f
        private const val DESIGN_H = 800f
        private val C_BG_TOP=Color.rgb(12,15,20); private val C_BG=Color.rgb(6,8,11); private val C_RAISED=Color.rgb(25,31,40)
        private val C_PANEL=Color.rgb(29,34,41); private val C_PANEL_DARK=Color.rgb(18,22,27); private val C_SUBPANEL=Color.rgb(25,31,40)
        private val C_ROW=Color.rgb(27,31,38); private val C_ROW_SELECTED=Color.rgb(31,45,57); private val C_NAV_BG=Color.rgb(18,22,27)
        private val C_BUTTON=Color.rgb(27,31,38); private val C_BUTTON_ACTIVE=Color.rgb(34,47,59); private val C_CHIP=Color.rgb(25,31,40)
        private val C_BORDER=Color.rgb(48,55,64); private val C_ACCENT=Color.rgb(108,198,255); private val C_ACCENT_DEEP=Color.rgb(47,127,214)
        private val C_ACCENT_DARK=Color.rgb(47,89,120); private val C_TEXT=Color.rgb(232,236,241); private val C_LABEL=Color.rgb(206,213,221)
        private val C_TEXT_DIM=Color.rgb(138,150,163); private val C_TEXT_MUTED=Color.rgb(92,102,114); private val C_FAINT=Color.rgb(60,68,78)
        private val C_TRACK=Color.rgb(13,18,24); private val C_KNOB=Color.rgb(32,40,50); private val C_KNOB_LIGHT=Color.rgb(140,170,192)
        private val C_PANIC=Color.rgb(229,72,77); private val C_METER_GREEN=Color.rgb(61,220,151); private val C_METER_YELLOW=Color.rgb(226,188,77)
        private val C_METER_RED=Color.rgb(229,72,77); private val C_WARN_BG=Color.rgb(61,29,31); private val C_WARN_TEXT=Color.rgb(255,187,191)
    }'''
s = s[:comp_start] + new_comp + '\n}'

s = s.replace(' * Visual baseline: docs/APPROVED_COMBI_UI.png + docs/APPROVED_LIST_UI.png.\n',
              ' * Visual baseline: SILVETON-sampler-3.zip / ui/list.html (machined graphite SILVETON skin).\n')

view.write_text(s, encoding='utf-8')
print('PASS: SILVETON-sampler-3 GUI ported to native Android Canvas view')
