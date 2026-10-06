package com.silveton.sampler

import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.SweepGradient
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.InputType
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.Window
import android.view.WindowManager
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.round

/**
 * Functional SILVETON SAMPLER touchscreen surface.
 *
 * Visual baseline: SILVETON LIVE MIDI native Canvas renderer; sampler labels/content remain unchanged.
 * SILVETON_GUI_V2 - approved "software synth" look: dark side navigation, amber LCDs,
 * seven light UPPER strips with real faders and knobs, dark editor boards.
 * Everything is drawn in fixed 1340x800 design units and scaled uniformly; touches are
 * mapped back into the same units, so every drawn control is a real hit with a native
 * state/DSP/action path. PROGRAM, SAMPLE and MIXER are not tablet top-level pages;
 * prepared sounds are selected directly into COMBI UPPER 1..7.
 */
class SilvetonSamplerView(
    context: Context,
    private val sampler: SamplerAppController
) : View(context) {

    private enum class Page { LIST, COMBI, EFFECTS, SETTINGS }
    private enum class UpperTab { TUNE, RANGE, CONTROL, TONE, FX }
    private enum class WriteTarget { LIST, BLEND }
    private enum class WriteMode { WRITE, COPY, MOVE }

    private data class Hit(
        val rect: RectF,
        val tap: (Float, Float) -> Unit,
        val reset: (() -> Unit)? = null,
        val drag: ((Float, Float, Boolean) -> Unit)? = null
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val hits = ArrayList<Hit>(192)
    private var page = Page.COMBI
    private var tab = UpperTab.TUNE
    private var selectedUpper = 0
    private var editingCombiSlot: Int? = null
    // LIST position the sound in the engine was loaded from (-1 = not from the LIST).
    private var loadedFromList = -1
    private var midiConnections = 0
    private var warning: String? = null
    private var listPage = 0
    private var listFollowedActive = -2

    private data class SmoothRangeHit(
        val rect: RectF,
        val minValue: Float,
        val maxValue: Float,
        val step: Float,
        var low: Float,
        var high: Float,
        val onChange: (Float, Float) -> Unit
    )
    private val smoothRangeHits = ArrayList<SmoothRangeHit>(4)
    private var activeSmoothRange: SmoothRangeHit? = null
    private var activeSmoothRangeHigh = false
    private var smoothPointerId = -1

    private data class ListLongHit(val rect: RectF, val action: (Float, Float) -> Unit)
    private val listLongHits = ArrayList<ListLongHit>(24)
    private var listLongPressX = 0f
    private var listLongPressY = 0f
    private var listLongPressArmed = false
    private var listLongPressFired = false
    private val listLongPressSlop by lazy { ViewConfiguration.get(context).scaledTouchSlop.toFloat() }
    private val listLongPressRunnable = Runnable {
        if (!listLongPressArmed) return@Runnable
        val target = listLongHitAt(listLongPressX, listLongPressY) ?: return@Runnable
        listLongPressArmed = false
        listLongPressFired = true
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        target.action.invoke(listLongPressX, listLongPressY)
        invalidate()
    }

    private val upperStates = arrayOfNulls<SamplerAppController.UpperState>(7)
    // The sound last chosen on each Upper: its list opens there again.
    private val lastPickedSound = arrayOfNulls<SamplerAppController.SoundItem>(7)
    private var fxState: SamplerAppController.CombiFxState? = null
    private var globalState: SamplerAppController.GlobalUiState? = null
    private var frameAudioStatus: SamplerAppController.AudioStatus = sampler.audioStatus()
    private var frameUpperMeters: List<Pair<Float, Float>> = List(7) { 0f to 0f }
    private var frameMidiStatus: SamplerAppController.MidiStatus = sampler.midiStatus()
    private var lastMidiSerial = frameMidiStatus.activitySerial
    private var midiFlashUntilMs = 0L
    private var lastTelemetryMs = 0L

    private var activeHit: Hit? = null
    private var activePointerId = -1
    private var downX = 0f
    private var downY = 0f
    private var downTimeMs = 0L
    private var dragMoved = false
    private var lastTapTimeMs = 0L
    private var lastTapRect: RectF? = null
    private val touchSlop = ViewConfiguration.get(context).scaledTouchSlop.toFloat()

    // Uniform design scale (1340x800 design units -> screen pixels).
    private var uiScale = 1f
    private var uiOffsetX = 0f
    private var uiOffsetY = 0f
    private val touchMatrix = Matrix()

    // Typeface.create() used to run for virtually every label on every frame.
    // Keep the faces for the lifetime of the view instead.
    private val faceRegular = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
    private val faceBold = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)
    private val faceMono = Typeface.MONOSPACE
    private val faceMonoBold = Typeface.create(Typeface.MONOSPACE, Typeface.BOLD)

    // LIST/COMBI identity data is not realtime telemetry. Cache it instead of
    // allocating arrays/strings through JNI every redraw.
    private var cachedCombiName = "COMBI"
    private var cachedListItems: List<SamplerAppController.ListItem> = emptyList()
    private var cachedActiveListEntry = -1
    private var listCacheLoaded = false

    // Korg-style WRITE screen (SILVETON_KORG_WRITE_V1).
    private var writeOpen = false
    private var writeMode = WriteMode.WRITE
    private var writeSource = -1
    private var writeTarget = WriteTarget.LIST
    private var writeName = ""
    private var writePage = 0
    private var writeSelected = -1
    private var writeList: List<SamplerAppController.ListItem> = emptyList()
    private var writeBlends: Map<Int, String> = emptyMap()

    private var flashText: String? = null
    private var flashUntilMs = 0L

    init {
        isFocusable = true
        refreshAllUppers()
        refreshFx()
        refreshGlobal()
        cachedCombiName = sampler.combiName()
    }

    fun setMidiConnections(count: Int) {
        midiConnections = max(0, count)
        invalidate()
    }

    fun setWarning(message: String?) {
        warning = message?.takeIf { it.isNotBlank() }
        invalidate()
    }

    // ------------------------------------------------------------------ touch

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val e = MotionEvent.obtain(event)
        touchMatrix.reset()
        touchMatrix.setTranslate(-uiOffsetX, -uiOffsetY)
        touchMatrix.postScale(1f / uiScale, 1f / uiScale)
        e.transform(touchMatrix)
        try {
            return handleTouch(e)
        } finally {
            e.recycle()
        }
    }

    private fun handleTouch(event: MotionEvent): Boolean {
        if (handleSmoothRangeTouch(event)) return true
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                listLongPressX = event.x
                listLongPressY = event.y
                listLongPressFired = false
                listLongPressArmed = listLongHitAt(event.x, event.y) != null
                removeCallbacks(listLongPressRunnable)
                if (listLongPressArmed) {
                    postDelayed(listLongPressRunnable, ViewConfiguration.getLongPressTimeout().toLong())
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (listLongPressArmed) {
                    val dx = event.x - listLongPressX
                    val dy = event.y - listLongPressY
                    if (dx * dx + dy * dy > listLongPressSlop * listLongPressSlop) {
                        listLongPressArmed = false
                        removeCallbacks(listLongPressRunnable)
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                listLongPressArmed = false
                removeCallbacks(listLongPressRunnable)
                if (listLongPressFired) {
                    val cancel = MotionEvent.obtain(event)
                    cancel.action = MotionEvent.ACTION_CANCEL
                    onTouchEventBase(cancel)
                    cancel.recycle()
                    listLongPressFired = false
                    return true
                }
            }
        }
        return onTouchEventBase(event)
    }

    private fun onTouchEventBase(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                activePointerId = event.getPointerId(0)
                downX = event.x
                downY = event.y
                downTimeMs = android.os.SystemClock.uptimeMillis()
                dragMoved = false
                activeHit = hitAt(downX, downY)
                parent?.requestDisallowInterceptTouchEvent(activeHit?.drag != null)
                postInvalidateOnAnimation()
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                val id = activePointerId
                if (id == -1) return true
                val index = event.findPointerIndex(id)
                if (index < 0) return true
                val x = event.getX(index)
                val y = event.getY(index)
                val hit = activeHit ?: return true
                if (hit.drag != null) {
                    val dx = x - downX
                    val dy = y - downY
                    if (!dragMoved && dx * dx + dy * dy >= touchSlop * touchSlop) dragMoved = true
                    if (dragMoved) {
                        hit.drag.invoke(x, y, false)
                        postInvalidateOnAnimation()
                    }
                }
                return true
            }
            MotionEvent.ACTION_UP -> {
                val hit = activeHit
                val x = event.x
                val y = event.y
                val now = android.os.SystemClock.uptimeMillis()
                if (hit != null) {
                    if (dragMoved && hit.drag != null) {
                        // One native commit at finger-up. MOVE stays local/vsync-fast.
                        hit.drag.invoke(x, y, true)
                    } else {
                        val previous = lastTapRect
                        val doubleTap = previous != null && previous.contains(x, y) &&
                            now - lastTapTimeMs <= 280L
                        val held = now - downTimeMs >= 480L
                        when {
                            doubleTap && hit.reset != null -> {
                                hit.reset.invoke()
                                lastTapTimeMs = 0L
                                lastTapRect = null
                            }
                            held && hit.drag != null -> {
                                // Long press keeps exact numeric entry available.
                                hit.tap.invoke(x, y)
                                lastTapTimeMs = 0L
                                lastTapRect = null
                            }
                            hit.drag != null -> {
                                hit.drag.invoke(x, y, true)
                                lastTapTimeMs = now
                                lastTapRect = RectF(hit.rect)
                            }
                            else -> {
                                hit.tap.invoke(x, y)
                                lastTapTimeMs = now
                                lastTapRect = RectF(hit.rect)
                            }
                        }
                    }
                }
                activeHit = null
                activePointerId = -1
                dragMoved = false
                parent?.requestDisallowInterceptTouchEvent(false)
                postInvalidateOnAnimation()
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                activeHit = null
                activePointerId = -1
                dragMoved = false
                parent?.requestDisallowInterceptTouchEvent(false)
                postInvalidateOnAnimation()
                return true
            }
        }
        return true
    }

    private fun handleSmoothRangeTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val range = smoothRangeHits.asReversed().firstOrNull { it.rect.contains(event.x, event.y) }
                if (range != null) {
                    activeSmoothRange = range
                    smoothPointerId = event.getPointerId(0)
                    val span = range.maxValue - range.minValue
                    val lowX = range.rect.left + range.rect.width() * ((range.low - range.minValue) / span).coerceIn(0f, 1f)
                    val highX = range.rect.left + range.rect.width() * ((range.high - range.minValue) / span).coerceIn(0f, 1f)
                    activeSmoothRangeHigh = abs(event.x - highX) < abs(event.x - lowX)
                    parent?.requestDisallowInterceptTouchEvent(true)
                    emitSmoothRange(range, event.x)
                    return true
                }
            }
            MotionEvent.ACTION_MOVE -> {
                val pointer = smoothPointerId
                if (pointer != -1) {
                    val index = event.findPointerIndex(pointer)
                    if (index >= 0) {
                        activeSmoothRange?.let { emitSmoothRange(it, event.getX(index)); return true }
                    }
                }
            }
            MotionEvent.ACTION_UP -> {
                activeSmoothRange?.let {
                    emitSmoothRange(it, event.x)
                    activeSmoothRange = null
                    smoothPointerId = -1
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (activeSmoothRange != null) {
                    activeSmoothRange = null
                    smoothPointerId = -1
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
        }
        return false
    }

    private fun registerSmoothRange(r: RectF, minValue: Float, maxValue: Float, step: Float, low: Float, high: Float, onChange: (Float, Float) -> Unit) {
        smoothRangeHits.add(SmoothRangeHit(RectF(r), minValue, maxValue, step, low, high, onChange))
    }

    private fun emitSmoothRange(hit: SmoothRangeHit, x: Float) {
        if (hit.rect.width() <= 1f) return
        val t = ((x - hit.rect.left) / hit.rect.width()).coerceIn(0f, 1f)
        val value = quantizeControl(hit.minValue + (hit.maxValue - hit.minValue) * t, hit.minValue, hit.maxValue, hit.step)
        if (activeSmoothRangeHigh) hit.high = max(hit.low, value) else hit.low = min(hit.high, value)
        hit.onChange(hit.low, hit.high)
        invalidate()
    }

    private fun quantizeControl(value: Float, minValue: Float, maxValue: Float, step: Float): Float {
        val clamped = value.coerceIn(minValue, maxValue)
        if (step <= 0f) return clamped
        val q = round((clamped - minValue) / step) * step + minValue
        return q.coerceIn(minValue, maxValue)
    }

    /** Relative knob: drag up or right to raise. Long press = exact number, double tap = default. */
    private fun knobHit(
        r: RectF, value: Float, minV: Float, maxV: Float, step: Float, travel: Float,
        onChange: (Float) -> Unit, exact: () -> Unit, reset: () -> Unit,
        onFinal: ((Float) -> Unit)? = null
    ) {
        val start = value
        var last = value
        hit(r, { _, _ -> exact() }, reset) { x, y, final ->
            if (!dragMoved) return@hit
            val d = (x - downX) - (y - downY)
            val moved = if (abs(d) <= touchSlop) 0f else d - Math.signum(d) * touchSlop
            val nv = quantizeControl(start + moved / travel * (maxV - minV), minV, maxV, step)
            if (nv != last) {
                last = nv
                onChange(nv)
                invalidate()
            }
            if (final) onFinal?.invoke(nv)
        }
    }

    /** Relative vertical fader in dB. Long press = exact number, double tap = 0 dB. */
    private fun faderHit(r: RectF, levelDb: Float, travel: Float, onChange: (Float) -> Unit, exact: () -> Unit, reset: () -> Unit) {
        val startT = faderT(levelDb)
        var last = levelDb
        hit(r, { _, _ -> exact() }, reset) { _, y, _ ->
            if (!dragMoved) return@hit
            val d = downY - y
            val moved = if (abs(d) <= touchSlop) 0f else d - Math.signum(d) * touchSlop
            val nv = quantizeControl(faderDb((startT + moved / travel).coerceIn(0f, 1f)), -60f, 12f, 0.1f)
            if (nv != last) {
                last = nv
                onChange(nv)
                invalidate()
            }
        }
    }

    // Fader law: -60..-24 dB, -24..0 dB and 0..+12 dB each take one third of the travel.
    private fun faderT(db: Float): Float = when {
        db <= -60f -> 0f
        db < -24f -> (db + 60f) / 108f
        db < 0f -> 1f / 3f + (db + 24f) / 72f
        else -> 2f / 3f + min(db, 12f) / 36f
    }

    private fun faderDb(t: Float): Float = when {
        t <= 1f / 3f -> -60f + t * 108f
        t <= 2f / 3f -> -24f + (t - 1f / 3f) * 72f
        else -> (t - 2f / 3f) * 36f
    }

    // ------------------------------------------------------------------ frame

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        hits.clear()
        smoothRangeHits.clear()
        listLongHits.clear()
        val telemetryNow = android.os.SystemClock.uptimeMillis()
        if (lastTelemetryMs == 0L || telemetryNow - lastTelemetryMs >= 50L) {
            frameAudioStatus = sampler.audioStatus()
            frameUpperMeters = sampler.upperMeters()
            frameMidiStatus = sampler.midiStatus()
            lastTelemetryMs = telemetryNow
        }
        if (frameMidiStatus.activitySerial != lastMidiSerial) {
            lastMidiSerial = frameMidiStatus.activitySerial
            midiFlashUntilMs = android.os.SystemClock.elapsedRealtime() + 220L
        }

        val w = width.toFloat()
        val h = height.toFloat()
        if (w <= 0f || h <= 0f) return
        val k = min(w / DESIGN_W, h / DESIGN_H)
        uiScale = k
        uiOffsetX = (w - DESIGN_W * k) * 0.5f
        uiOffsetY = (h - DESIGN_H * k) * 0.5f
        paint.shader = null
        paint.style = Paint.Style.FILL
        paint.color = C_SIDE_A
        canvas.drawRect(0f, 0f, w, h, paint)

        canvas.save()
        canvas.translate(uiOffsetX, uiOffsetY)
        canvas.scale(k, k)
        drawMetal(canvas)
        drawSideNav(canvas)
        drawHeader(canvas)
        when (page) {
            Page.LIST -> drawList(canvas)
            Page.COMBI -> drawCombi(canvas)
            Page.EFFECTS -> drawEffects(canvas)
            Page.SETTINGS -> drawSettings(canvas)
        }
        warning?.let { msg ->
            val box = RectF(112f, 744f, 1326f, 786f)
            shadow(canvas, box, 10f, 3f, 90)
            fill(canvas, box, 10f, C_WARN_BG)
            text(canvas, msg, box.left + 16f, box.centerY() + 5f, 14f, C_WARN_TEXT, face = faceBold, maxWidth = box.width() - 120f)
            text(canvas, "СКРИЙ ×", box.right - 16f, box.centerY() + 5f, 12f, C_WARN_TEXT, right = true, face = faceBold, spacing = 0.1f)
            hit(box, { _, _ -> warning = null })
        }
        if (writeOpen) {
            // Modal: only the WRITE screen answers touches while it is open.
            hits.clear()
            smoothRangeHits.clear()
            listLongHits.clear()
            drawWriteScreen(canvas)
        }
        canvas.restore()

        if (activeHit?.drag != null || activeSmoothRange != null) postInvalidateOnAnimation() else postInvalidateDelayed(100L)
    }

    private fun drawMetal(c: Canvas) {
        paint.style = Paint.Style.FILL
        paint.color = Color.BLACK
        paint.shader = LinearGradient(0f, 0f, 4f, 0f, intArrayOf(C_METAL_A, C_METAL_A, C_METAL_B, C_METAL_B),
            floatArrayOf(0f, 0.5f, 0.5f, 1f), Shader.TileMode.REPEAT)
        c.drawRect(0f, 0f, DESIGN_W, DESIGN_H, paint)
        paint.shader = null
    }

    private fun drawSideNav(c: Canvas) {
        paint.style = Paint.Style.FILL
        paint.color = Color.BLACK
        paint.shader = LinearGradient(0f, 0f, 96f, 0f, C_SIDE_A, C_SIDE_B, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, 96f, DESIGN_H, paint)
        paint.shader = LinearGradient(96f, 0f, 104f, 0f, Color.argb(80, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawRect(96f, 0f, 104f, DESIGN_H, paint)
        paint.shader = null

        c.save()
        c.rotate(-90f, 48f, 66f)
        text(c, "SILVETON", 48f, 70f, 13f, C_LOGO, center = true, face = faceBold, spacing = 0.23f)
        c.restore()

        val labels = listOf(
            Page.LIST to "ЛИСТ",
            Page.COMBI to "КОМБИ",
            Page.EFFECTS to "ЕФЕКТИ",
            Page.SETTINGS to "НАСТРОЙКИ"
        )
        labels.forEachIndexed { i, (p, label) ->
            val b = RectF(10f, 128f + i * 104f, 86f, 224f + i * 104f)
            navKey(c, b, page == p)
            led(c, b.centerX(), b.centerY() - 14f, page == p, 5f)
            text(c, label, b.centerX(), b.centerY() + 22f, 11f, if (page == p) Color.WHITE else C_NAV_TEXT,
                center = true, face = if (page == p) faceBold else faceRegular, spacing = 0.06f, maxWidth = 70f)
            hit(b, { _, _ ->
                page = p
                if (p == Page.LIST) refreshListCache()
                if (p == Page.COMBI) cachedCombiName = sampler.combiName()
                if (p == Page.EFFECTS) refreshFx()
                if (p == Page.SETTINGS) refreshGlobal()
            })
        }

        val gear = RectF(10f, 734f, 86f, 786f)
        navKey(c, gear, page == Page.SETTINGS)
        drawGear(c, gear.centerX(), gear.centerY(), 11f)
        hit(gear, { _, _ -> page = Page.SETTINGS; refreshGlobal() })
    }

    private fun drawHeader(c: Canvas) {
        val a = frameAudioStatus
        val m = frameMidiStatus
        val now = android.os.SystemClock.elapsedRealtime()
        val midiRx = now < midiFlashUntilMs
        val keyFlash = midiRx && (m.status and 0xF0) == 0x90 && m.data2 > 0
        val midiConnected = max(midiConnections, m.deviceCount) > 0
        val midiValue = when {
            keyFlash -> midiNoteName(m.data1)
            midiRx -> "RX"
            midiConnected -> "ON"
            m.lastError != 0 -> "E${-m.lastError}"
            else -> "OFF"
        }

        val running = sampler.audioRunning()
        val lastAudioError = sampler.audioLastError()
        val audioOk = running && a.sampleRate == 96000 && a.framesPerBurst == 96 && a.bufferSizeFrames == 192
        val audioSignal = a.activeVoices > 0 || frameUpperMeters.any { it.first > 0.0001f || it.second > 0.0001f }
        val audioValue = when {
            audioOk -> "96k 96x2"
            running -> "${a.sampleRate / 1000}k ${a.framesPerBurst}"
            lastAudioError != 0 -> "OFF E${-lastAudioError}"
            else -> "OFF"
        }

        // Big LCD: where you are and what is loaded.
        val big = RectF(112f, 14f, 672f, 96f)
        lcd(c, big)
        val slotText = editingCombiSlot?.let { "%03d".format(it) } ?: "---"
        val activeItem = realListItem(cachedActiveListEntry)
        val line1 = when (page) {
            Page.COMBI -> "КОМБИ $slotText" + if (loadedFromList >= 0) " · ЛИСТ %03d".format(loadedFromList + 1) else ""
            Page.LIST -> "ЛИСТ · СТРАНИЦА ${listPage + 1}"
            Page.EFFECTS -> "ЕФЕКТИ · КОМБИ $slotText"
            Page.SETTINGS -> "НАСТРОЙКИ · ГЛОБАЛНИ"
        }
        val line2 = if (page == Page.LIST && activeItem != null) "%03d %s".format(cachedActiveListEntry + 1, activeItem.name) else cachedCombiName
        val flash = flashText?.takeIf { now < flashUntilMs }
        text(c, flash ?: line1, big.left + 18f, big.top + 22f, 12f, if (flash != null) C_LCD_OK else C_LCD_DIM,
            face = faceMono, spacing = 0.25f, maxWidth = big.width() - 36f)
        text(c, line2, big.left + 18f, big.top + 60f, 32f, C_LCD, face = faceMono, maxWidth = big.width() - 36f)

        // Status LCD.
        val st = RectF(684f, 14f, 1014f, 96f)
        lcd(c, st)
        val colW = (st.width() - 32f) / 3f
        fun cell(col: Int, row: Int, label: String, value: String, valueColor: Int) {
            val x = st.left + 16f + col * colW
            val ly = st.top + 20f + row * 38f
            text(c, label, x, ly, 11f, C_LCD_DIM, face = faceMono, spacing = 0.15f, maxWidth = colW - 6f)
            text(c, value, x, ly + 19f, 17f, valueColor, face = faceMono, maxWidth = colW - 6f)
        }
        cell(0, 0, "AUDIO", audioValue, if (audioOk) C_LCD_OK else if (running) C_LCD_VALUE else C_LCD_BAD)
        cell(1, 0, "MIDI", midiValue, if (midiRx) C_LCD else if (midiConnected) C_LCD_OK else C_LCD_VALUE)
        cell(2, 0, "ГЛАСОВЕ", a.activeVoices.toString(), if (audioSignal) C_LCD else C_LCD_VALUE)
        cell(0, 1, "XRUN", a.xrunCount.toString(), if (a.xrunCount > 0) C_LCD_BAD else C_LCD_VALUE)
        cell(1, 1, "ЛАТЕНЦИЯ", if (a.estimatedLatencyMs >= 0.0) "%.1f ms".format(a.estimatedLatencyMs) else "—", C_LCD_VALUE)
        val porta = upperStates[selectedUpper]?.portamentoSec ?: 0f
        // SILVETON_PORTA_BALANCE_V2: PORTA BALANCE is back (3..40 ms, whole sampler):
        // how softly the pitch rounds into and out of every glide. Tap = change.
        val bal = sampler.portaBalance()
        cell(2, 1, "PORTA U${selectedUpper + 1}", (if (porta <= 0f) "OFF" else "${round(porta * 1000f).toInt()}ms") + " B$bal", C_LCD_VALUE)
        val balR = RectF(st.left + 16f + 2f * colW, st.top + 40f, st.right - 4f, st.bottom - 2f)
        hit(balR, { _, _ ->
            val old = sampler.portaBalance()
            showNumber("PORTA BALANCE (ms)", old.toFloat(), 3f, 40f, 0,
                restore = { sampler.setPortaBalance(old); invalidate() }) { sampler.setPortaBalance(round(it).toInt()); invalidate() }
        })

        val blendsR = RectF(1026f, 14f, 1206f, 50f)
        lightKey(c, blendsR, "BLENDS")
        hit(blendsR, { _, _ -> showBlendPicker() })
        val writeR = RectF(1026f, 60f, 1112f, 96f)
        lightKey(c, writeR, "WRITE")
        hit(writeR, { _, _ -> openWrite(WriteTarget.LIST, append = false) })
        val addR = RectF(1120f, 60f, 1206f, 96f)
        lightKey(c, addR, "+ LIST")
        hit(addR, { _, _ -> openWrite(WriteTarget.LIST, append = true) })

        val panic = RectF(1218f, 14f, 1326f, 96f)
        val down = isDown(panic)
        if (!down) shadow(c, panic, 12f, 4f, 100)
        fillV(c, panic, 12f, if (down) C_PANIC else Color.rgb(198, 52, 44), if (down) Color.rgb(140, 28, 22) else C_PANIC)
        line(c, panic.left + 12f, panic.top + 1.5f, panic.right - 12f, panic.top + 1.5f, C_PANIC_TOP, 2f)
        text(c, "PANIC", panic.centerX(), panic.centerY() + 6f, 15f, Color.WHITE, center = true, face = faceBold, spacing = 0.2f)
        hit(panic, { _, _ -> sampler.panic() })
    }

    // ------------------------------------------------------------------ COMBI

    private fun drawCombi(c: Canvas) {
        val meters = frameUpperMeters
        val stripW = (1214f - 6f * 8f) / 7f
        for (i in 0 until 7) {
            val state = upperStates[i] ?: sampler.upperState(i)?.also { upperStates[i] = it } ?: continue
            val left = 112f + i * (stripW + 8f)
            drawUpperStrip(c, RectF(left, 108f, left + stripW, 468f), i, state, meters.getOrElse(i) { 0f to 0f })
        }
        drawUpperEditor(c)
    }

    private fun drawUpperStrip(c: Canvas, r: RectF, i: Int, s: SamplerAppController.UpperState, meter: Pair<Float, Float>) {
        val selected = i == selectedUpper
        val empty = s.soundName == "EMPTY" || s.soundName.isBlank()
        val live = s.enabled && !empty
        val L = r.left
        val T = r.top
        val W = r.width()

        // Any free spot of the strip selects this UPPER (registered first = lowest priority).
        hit(r, { _, _ -> selectedUpper = i })

        if (selected) {
            fill(c, RectF(r.left - 3f, r.top - 3f, r.right + 3f, r.bottom + 3f), 15f, C_ORANGE)
        } else {
            shadow(c, r, 12f, 2f, 55)
        }
        fill(c, r, 12f, if (selected) C_STRIP_SEL else C_STRIP)
        line(c, L + 12f, T + 1f, r.right - 12f, T + 1f, Color.WHITE, 1f)

        text(c, "UPPER ${i + 1}", L + 12f, T + 21f, 10f, C_LABEL_GREY, spacing = 0.2f)
        led(c, r.right - 15f, T + 17f, live, 5f, light = true)
        text(c, if (empty) "EMPTY" else s.soundName, L + 12f, T + 42f, 15f, if (empty) C_EMPTY_TEXT else C_INK,
            face = faceBold, maxWidth = W - 24f)
        hit(RectF(L, T + 26f, r.right, T + 50f), { _, _ -> selectedUpper = i; showSoundPicker(i) })

        // ON / S / M
        val tgW = (W - 20f - 10f) / 3f
        for (k in 0 until 3) {
            val tr = RectF(L + 10f + k * (tgW + 5f), T + 52f, L + 10f + k * (tgW + 5f) + tgW, T + 80f)
            val on = when (k) { 0 -> s.enabled; 1 -> s.solo; else -> s.mute }
            val label = when (k) { 0 -> "ON"; 1 -> "S"; else -> "M" }
            val onColor = when (k) { 0 -> C_AMBER; 1 -> C_SOLO; else -> C_MUTE }
            stripToggle(c, tr, label, on, onColor)
            hit(tr, { _, _ ->
                val cur = upperStates[i] ?: s
                when (k) {
                    0 -> if (!empty) commitMain(i, cur.copy(enabled = !cur.enabled))
                    1 -> commitMain(i, cur.copy(solo = !cur.solo))
                    else -> commitMain(i, cur.copy(mute = !cur.mute))
                }
            })
        }

        // PAN knob: drag the whole row, long press = number, double tap = centre.
        drawKnob(c, L + 31f, T + 110f, 21f, (s.pan + 1f) * 0.5f, bipolar = true, ring = false)
        text(c, "PAN", L + 62f, T + 104f, 10f, C_LABEL_GREY, spacing = 0.2f)
        text(c, panText(s.pan), L + 62f, T + 123f, 14f, C_INK, face = faceMonoBold)
        knobHit(RectF(L, T + 84f, r.right, T + 136f), s.pan, -1f, 1f, 0.01f, 160f,
            onChange = { nv -> val cur = upperStates[i] ?: s; commitMain(i, cur.copy(pan = nv)) },
            exact = {
                showNumber("UPPER ${i + 1} PAN", s.pan, -1f, 1f, 2) { nv ->
                    val cur = upperStates[i] ?: s
                    commitMain(i, cur.copy(pan = nv))
                }
            },
            reset = { val cur = upperStates[i] ?: s; commitMain(i, cur.copy(pan = 0f)) })

        // LEVEL fader with L/R meters.
        val trackL = L + 58f
        val trackR = L + 70f
        val trackTop = T + 160f
        val trackBot = T + 282f
        val travel = trackBot - trackTop
        listOf("+12" to 1f, "0" to 2f / 3f, "-24" to 1f / 3f, "-60" to 0f).forEach { (lbl, t) ->
            text(c, lbl, L + 10f, trackBot - t * travel + 3.5f, 9f, C_SCALE, face = faceMono)
        }
        fill(c, RectF(trackL, trackTop, trackR, trackBot), 6f, C_TRACK_LIGHT)
        paint.color = Color.BLACK
        paint.shader = LinearGradient(trackL, 0f, trackR, 0f, Color.argb(70, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawRoundRect(RectF(trackL, trackTop, trackR, trackBot), 6f, 6f, paint)
        paint.shader = null
        stripMeter(c, RectF(L + 80f, trackTop, L + 90f, trackBot), meter.first)
        stripMeter(c, RectF(L + 100f, trackTop, L + 110f, trackBot), meter.second)
        faderCap(c, (trackL + trackR) * 0.5f, trackBot - faderT(s.levelDb) * travel)
        text(c, dbText(s.levelDb), r.centerX(), T + 308f, 14f, C_INK, center = true, face = faceMonoBold)
        val exactLevel = {
            showNumber("UPPER ${i + 1} LEVEL dB", s.levelDb, -60f, 12f, 1) { nv ->
                val cur = upperStates[i] ?: s
                commitMain(i, cur.copy(levelDb = nv))
            }
        }
        val resetLevel = { val cur = upperStates[i] ?: s; commitMain(i, cur.copy(levelDb = 0f)) }
        faderHit(RectF(L + 34f, T + 140f, r.right - 4f, T + 292f), s.levelDb, travel,
            onChange = { nv -> val cur = upperStates[i] ?: s; commitMain(i, cur.copy(levelDb = nv)) },
            exact = exactLevel, reset = resetLevel)
        hit(RectF(L, T + 292f, r.right, T + 316f), { _, _ -> exactLevel() }, resetLevel)

        // Voice mode + key range.
        val half = (W - 20f - 5f) / 2f
        val modeR = RectF(L + 10f, T + 322f, L + 10f + half, T + 350f)
        val rangeR = RectF(modeR.right + 5f, T + 322f, r.right - 10f, T + 350f)
        darkKey(c, modeR, voiceModeName(s.voiceMode))
        lightKey(c, rangeR, "${noteName(s.keyLow)}–${noteName(s.keyHigh)}", size = 11f, radius = 6f)
        hit(modeR, { _, _ -> val cur = upperStates[i] ?: s; commitMain(i, cur.copy(voiceMode = (cur.voiceMode + 1) % 3)) },
            { val cur = upperStates[i] ?: s; commitMain(i, cur.copy(voiceMode = 0)) })
        hit(rangeR, { _, _ -> selectedUpper = i; showKeyRange(i, upperStates[i] ?: s) })

        if (!live) {
            // Switched-off / empty UPPER: veil it, controls stay usable.
            fill(c, r, 12f, Color.argb(92, 220, 221, 225))
        }
    }

    private fun drawUpperEditor(c: Canvas) {
        val s = upperStates[selectedUpper] ?: return
        val upperLabel = "UPPER ${selectedUpper + 1}"
        text(c, upperLabel, 112f, 503f, 13f, C_INK_SOFT, face = faceBold, spacing = 0.15f)
        val nameX = 112f + textWidth(upperLabel, 13f, faceBold, 0.15f) + 8f
        text(c, s.soundName, nameX, 503f, 13f, C_ORANGE_DARK, face = faceBold, maxWidth = 284f - 10f - nameX)

        var x = 284f
        UpperTab.entries.forEach { t ->
            val tw = textWidth(t.name, 13f, faceBold, 0.15f) + 44f
            val rr = RectF(x, 478f, x + tw, 514f)
            val on = tab == t
            paint.shader = null
            paint.style = Paint.Style.FILL
            paint.color = if (on) C_BOARD_TOP else C_TAB
            c.drawRoundRect(rr, 8f, 8f, paint)
            c.drawRect(rr.left, rr.centerY(), rr.right, rr.bottom + 1f, paint)
            text(c, t.name, rr.centerX(), rr.top + 23f, 13f, if (on) C_AMBER else C_TAB_TEXT, center = true, face = faceBold, spacing = 0.15f)
            hit(rr, { _, _ -> tab = t })
            x += tw + 6f
        }

        board(c, RectF(112f, 514f, 1326f, 786f), squareTopLeft = true)
        val sec = RectF(128f, 530f, 1310f, 770f)
        when (tab) {
            UpperTab.TUNE -> drawTune(c, sec, s)
            UpperTab.RANGE -> drawRange(c, sec, s)
            UpperTab.CONTROL -> drawControl(c, sec, s)
            UpperTab.TONE -> drawTone(c, sec, s)
            UpperTab.FX -> drawUpperFx(c, sec, s)
        }
    }

    // PORTAMENTO TIME like the SILVETON keyboard processor: 0 = OFF, 1..100 ms (linear,
    // the same total time for every interval).
    private fun portamentoUiValue(seconds: Float): Float {
        if (seconds <= 0f) return 0f
        return (seconds * 1000f).coerceIn(1f, 100f)
    }

    private fun portamentoSecondsFromUi(value: Float): Float {
        val ms = round(value.coerceIn(0f, 100f))
        return if (ms <= 0f) 0f else ms / 1000f
    }

    private fun currentUpper(fallback: SamplerAppController.UpperState) = upperStates[selectedUpper] ?: fallback

    private fun drawTune(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        section(c, sec, "TUNE · ВИСОЧИНА И ПЛЪЗГАНЕ")
        val cy = 630f
        val cx = FloatArray(5) { sec.left + 115f + it * 230f }

        val porta = round(portamentoUiValue(s.portamentoSec))
        knobControl(c, cx[0], cy, 42f, porta, 0f, 100f, 1f, 250f, false,
            if (porta <= 0f) "OFF" else "${porta.toInt()} ms", "PORTAMENTO",
            onChange = { nv -> commitTune(currentUpper(s).copy(portamentoSec = portamentoSecondsFromUi(nv))) },
            exact = {
                showNumber("PORTAMENTO ms (0 = OFF)", porta, 0f, 100f, 0) { exact ->
                    commitTune(currentUpper(s).copy(portamentoSec = portamentoSecondsFromUi(round(exact))))
                }
            },
            reset = { commitTune(currentUpper(s).copy(portamentoSec = 0f)) })

        knobControl(c, cx[1], cy, 42f, s.octave.toFloat(), -4f, 4f, 1f, 200f, true, signed(s.octave), "ОКТАВА",
            onChange = { nv -> commitTune(currentUpper(s).copy(octave = nv.toInt())) },
            exact = { showNumber("ОКТАВА", s.octave.toFloat(), -4f, 4f, 0) { nv -> commitTune(currentUpper(s).copy(octave = nv.toInt())) } },
            reset = { commitTune(currentUpper(s).copy(octave = 0)) })

        knobControl(c, cx[2], cy, 42f, s.transpose.toFloat(), -48f, 48f, 1f, 480f, true, signed(s.transpose), "ТРАНСПОНИРАНЕ",
            onChange = { nv -> commitTune(currentUpper(s).copy(transpose = nv.toInt())) },
            exact = { showNumber("ТРАНСПОНИРАНЕ", s.transpose.toFloat(), -48f, 48f, 0) { nv -> commitTune(currentUpper(s).copy(transpose = nv.toInt())) } },
            reset = { commitTune(currentUpper(s).copy(transpose = 0)) })

        knobControl(c, cx[3], cy, 42f, s.fineCents, -100f, 100f, 0.1f, 400f, true, "%.1f ct".format(s.fineCents), "ФИНА НАСТРОЙКА",
            onChange = { nv -> commitTune(currentUpper(s).copy(fineCents = nv)) },
            exact = { showNumber("ФИНА НАСТРОЙКА ct", s.fineCents, -100f, 100f, 1) { nv -> commitTune(currentUpper(s).copy(fineCents = nv)) } },
            reset = { commitTune(currentUpper(s).copy(fineCents = 0f)) })

        knobControl(c, cx[4], cy, 42f, s.pitchBendRange.toFloat(), 0f, 24f, 1f, 240f, false, "${s.pitchBendRange} st", "PITCH BEND",
            onChange = { nv -> commitTune(currentUpper(s).copy(pitchBendRange = nv.toInt())) },
            exact = { showNumber("PITCH BEND st", s.pitchBendRange.toFloat(), 0f, 24f, 0) { nv -> commitTune(currentUpper(s).copy(pitchBendRange = nv.toInt())) } },
            reset = { commitTune(currentUpper(s).copy(pitchBendRange = 2)) })

        hintLine(c, sec)
    }

    private fun hintLine(c: Canvas, sec: RectF) {
        text(c, "Влачи копчето нагоре/надясно · Докосни стойността = въвеждане на число · Двоен допир = по подразбиране",
            sec.left + 16f, sec.bottom - 12f, 11f, C_HINT, spacing = 0.03f, maxWidth = sec.width() - 32f)
    }

    private fun drawRange(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        val vel = RectF(sec.left, sec.top, sec.left + 700f, sec.bottom)
        section(c, vel, "VELOCITY RANGE · СИЛА НА УДАРА")
        val bar = RectF(vel.left + 132f, 640f, vel.right - 132f, 652f)
        drawRangeBar(c, bar, s.velocityLow.toFloat(), s.velocityHigh.toFloat(), 1f, 127f)
        registerSmoothRange(RectF(bar.left, bar.top - 26f, bar.right, bar.bottom + 26f), 1f, 127f, 1f,
            s.velocityLow.toFloat(), s.velocityHigh.toFloat()) { lo, hi ->
            commitRange(currentUpper(s).copy(velocityLow = lo.toInt(), velocityHigh = hi.toInt()))
        }
        text(c, "МИН", vel.left + 66f, 616f, 10f, C_KNOB_LABEL, center = true, spacing = 0.2f)
        text(c, s.velocityLow.toString(), vel.left + 66f, 656f, 28f, C_AMBER, center = true, face = faceMono)
        text(c, "МАКС", vel.right - 66f, 616f, 10f, C_KNOB_LABEL, center = true, spacing = 0.2f)
        text(c, s.velocityHigh.toString(), vel.right - 66f, 656f, 28f, C_AMBER, center = true, face = faceMono)
        val resetVel = { commitRange(currentUpper(s).copy(velocityLow = 1, velocityHigh = 127)) }
        hit(RectF(vel.left, 596f, bar.left - 4f, 700f), { _, _ -> showVelocityRange(selectedUpper, currentUpper(s)) }, resetVel)
        hit(RectF(bar.right + 4f, 596f, vel.right, 700f), { _, _ -> showVelocityRange(selectedUpper, currentUpper(s)) }, resetVel)
        text(c, "Влачи краищата · Докосни числото = точна стойност · Двоен допир = 1–127",
            vel.left + 16f, vel.bottom - 12f, 11f, C_HINT, maxWidth = vel.width() - 32f)

        val key = RectF(vel.right + 12f, sec.top, sec.right, sec.bottom)
        section(c, key, "KEY RANGE · ОБХВАТ")
        text(c, "${noteName(s.keyLow)}  –  ${noteName(s.keyHigh)}", key.left + 20f, 614f, 30f, C_AMBER, face = faceMono, maxWidth = key.width() - 40f)
        drawMiniKeyboard(c, RectF(key.left + 20f, 636f, key.right - 20f, 700f), s.keyLow, s.keyHigh)
        text(c, "Докосни за точни граници", key.left + 16f, key.bottom - 12f, 11f, C_HINT, maxWidth = key.width() - 32f)
        hit(key, { _, _ -> showKeyRange(selectedUpper, currentUpper(s)) },
            { val cur = currentUpper(s); commitMain(selectedUpper, cur.copy(keyLow = 0, keyHigh = 127)) })
    }

    private fun drawControl(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        section(c, sec, "CONTROL · КОНТРОЛЕРИ НА UPPER")
        val colW = (sec.width() - 32f) / 5f
        val items = listOf(
            Triple("BREATH", "CC2", s.breath),
            Triple("EXPRESSION", "CC11", s.expression),
            Triple("AFTERTOUCH", "PRESSURE", s.aftertouch),
            Triple("DAMPER", "CC64", s.damper)
        )
        items.forEachIndexed { k, (name, cc, on) ->
            val cx = sec.left + 16f + colW * (k + 0.5f)
            val key = RectF(cx - 92f, 584f, cx + 92f, 694f)
            switchKey(c, key, name, cc, on)
            hit(key, { _, _ ->
                val cur = currentUpper(s)
                commitControl(when (k) {
                    0 -> cur.copy(breath = !cur.breath)
                    1 -> cur.copy(expression = !cur.expression)
                    2 -> cur.copy(aftertouch = !cur.aftertouch)
                    else -> cur.copy(damper = !cur.damper)
                })
            })
        }
        val cx = sec.left + 16f + colW * 4.5f
        knobControl(c, cx, 626f, 38f, (s.midiChannel + 1).toFloat(), 1f, 16f, 1f, 220f, false,
            "CH ${s.midiChannel + 1}", "MIDI КАНАЛ",
            onChange = { nv -> commitControl(currentUpper(s).copy(midiChannel = nv.toInt() - 1)) },
            exact = { showNumber("MIDI Channel", (s.midiChannel + 1).toFloat(), 1f, 16f, 0) { commitControl(currentUpper(s).copy(midiChannel = it.toInt() - 1)) } },
            reset = { commitControl(currentUpper(s).copy(midiChannel = 0)) })
        text(c, "Докосни бутона = ВКЛ/ИЗКЛ", sec.left + 16f, sec.bottom - 12f, 11f, C_HINT)
    }

    private enum class TonePar { ATTACK, DECAY, SUSTAIN, RELEASE, CUTOFF, RESONANCE }

    private fun toneValue(s: SamplerAppController.UpperState, p: TonePar): Float = when (p) {
        TonePar.ATTACK -> s.toneAttackMs
        TonePar.DECAY -> s.toneDecayMs
        TonePar.SUSTAIN -> s.toneSustain
        TonePar.RELEASE -> s.toneReleaseMs
        TonePar.CUTOFF -> s.toneCutoffHz
        TonePar.RESONANCE -> s.toneResonance
    }

    private fun toneToNorm(p: TonePar, v: Float): Float = when (p) {
        TonePar.ATTACK, TonePar.DECAY, TonePar.RELEASE -> kotlin.math.ln(v.coerceIn(0f, 20000f) + 1f) / kotlin.math.ln(20001f)
        TonePar.SUSTAIN, TonePar.RESONANCE -> v.coerceIn(0f, 1f)
        TonePar.CUTOFF -> kotlin.math.ln(v.coerceIn(20f, 20000f) / 20f) / kotlin.math.ln(1000f)
    }

    private fun toneFromNorm(p: TonePar, t: Float): Float {
        val n = t.coerceIn(0f, 1f)
        return when (p) {
            TonePar.ATTACK, TonePar.DECAY, TonePar.RELEASE ->
                round((kotlin.math.exp(n * kotlin.math.ln(20001f)) - 1f).coerceIn(0f, 20000f))
            TonePar.SUSTAIN, TonePar.RESONANCE -> round(n * 1000f) / 1000f
            TonePar.CUTOFF -> if (n >= 0.999f) 20000f else round(20f * Math.pow(1000.0, n.toDouble()).toFloat())
        }
    }

    private fun toneText(p: TonePar, v: Float): String = when (p) {
        TonePar.ATTACK, TonePar.DECAY, TonePar.RELEASE ->
            if (v < 1000f) "${round(v).toInt()} ms" else "%.2f s".format(v / 1000f)
        TonePar.SUSTAIN, TonePar.RESONANCE -> "${round(v * 100f).toInt()} %"
        TonePar.CUTOFF -> when {
            v >= 19900f -> "OPEN"
            v < 1000f -> "${round(v).toInt()} Hz"
            else -> "%.1f kHz".format(v / 1000f)
        }
    }

    private fun setTone(p: TonePar, value: Float) {
        val cur = upperStates[selectedUpper] ?: return
        sampler.setUpperTone(selectedUpper, p.ordinal, value)
        if (value < 0f) {
            upperStates[selectedUpper] = sampler.upperState(selectedUpper) ?: cur
            return
        }
        val bit = cur.toneSet or (1 shl p.ordinal)
        upperStates[selectedUpper] = when (p) {
            TonePar.ATTACK -> cur.copy(toneAttackMs = value, toneSet = bit)
            TonePar.DECAY -> cur.copy(toneDecayMs = value, toneSet = bit)
            TonePar.SUSTAIN -> cur.copy(toneSustain = value, toneSet = bit)
            TonePar.RELEASE -> cur.copy(toneReleaseMs = value, toneSet = bit)
            TonePar.CUTOFF -> cur.copy(toneCutoffHz = value, toneSet = bit)
            TonePar.RESONANCE -> cur.copy(toneResonance = value, toneSet = bit)
        }
    }

    private fun toneRange(p: TonePar): Triple<Float, Float, Int> = when (p) {
        TonePar.ATTACK, TonePar.DECAY, TonePar.RELEASE -> Triple(0f, 20000f, 0)
        TonePar.SUSTAIN, TonePar.RESONANCE -> Triple(0f, 100f, 0)
        TonePar.CUTOFF -> Triple(20f, 20000f, 0)
    }

    private fun drawTone(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        val gap = 10f
        val unit = (sec.width() - 3f * gap) / 11f
        val counts = intArrayOf(4, 2, 2, 3)
        val titles = arrayOf("AMP ENVELOPE", "FILTER", "LFO", "EQ")
        var x = sec.left
        val boxes = Array(4) { k ->
            val box = RectF(x, sec.top, x + unit * counts[k], sec.bottom)
            x = box.right + gap
            section(c, box, titles[k])
            box
        }
        val cy = 628f
        val radius = 28f
        fun cxOf(box: RectF, k: Int) = box.left + unit * (k + 0.5f)

        val env = listOf("ATTACK" to TonePar.ATTACK, "DECAY" to TonePar.DECAY, "SUSTAIN" to TonePar.SUSTAIN, "RELEASE" to TonePar.RELEASE)
        val filter = listOf("CUTOFF" to TonePar.CUTOFF, "RESONANCE" to TonePar.RESONANCE)
        listOf(boxes[0] to env, boxes[1] to filter).forEach { (box, items) ->
            items.forEachIndexed { k, (label, p) ->
                val value = toneValue(s, p)
                val range = toneRange(p)
                val pct = p == TonePar.SUSTAIN || p == TonePar.RESONANCE
                val shown = if (pct) value * 100f else value
                knobControl(c, cxOf(box, k), cy, radius, toneToNorm(p, value), 0f, 1f, 0.002f, 260f, false,
                    toneText(p, value), label, hitW = unit - 4f,
                    onChange = { nv -> setTone(p, toneFromNorm(p, nv)) },
                    exact = {
                        val wasSet = (s.toneSet and (1 shl p.ordinal)) != 0
                        showNumber(label, shown.coerceIn(range.first, range.second), range.first, range.second, range.third,
                            restore = { setTone(p, if (wasSet) value else -1f) }) { nv ->
                            setTone(p, if (pct) (nv / 100f).coerceIn(0f, 1f) else nv)
                        }
                    },
                    reset = { setTone(p, -1f) })
            }
        }

        val lfo = listOf("RATE" to s.lfoRateScale, "DEPTH" to s.lfoDepthScale)
        lfo.forEachIndexed { k, (label, value) ->
            val maxV = if (k == 0) 16f else 8f
            val apply = { nv: Float ->
                val cur = currentUpper(s)
                commitTone(if (k == 0) cur.copy(lfoRateScale = nv) else cur.copy(lfoDepthScale = nv))
            }
            knobControl(c, cxOf(boxes[2], k), cy, radius, value, 0f, maxV, 0.01f, 300f, false,
                "%.2f".format(value), label, hitW = unit - 4f,
                onChange = { nv -> apply(nv) },
                exact = { showNumber("LFO $label", value, 0f, maxV, 2) { apply(it) } },
                reset = { apply(1f) })
        }

        val eq = listOf("LOW" to s.eqLowDb, "MID" to s.eqMidDb, "HIGH" to s.eqHighDb)
        eq.forEachIndexed { k, (label, value) ->
            val apply = { nv: Float ->
                val cur = currentUpper(s)
                commitTone(when (k) { 0 -> cur.copy(eqLowDb = nv); 1 -> cur.copy(eqMidDb = nv); else -> cur.copy(eqHighDb = nv) })
            }
            knobControl(c, cxOf(boxes[3], k), cy, radius, value, -18f, 18f, 0.1f, 300f, true,
                "%.1f dB".format(value), label, hitW = unit - 4f,
                onChange = { nv -> apply(nv) },
                exact = { showNumber("EQ $label dB", value, -18f, 18f, 1) { apply(it) } },
                reset = { apply(0f) })
        }
    }

    private fun drawUpperFx(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        section(c, sec, "FX · ИЗПРАЩАНЕ КЪМ ЕФЕКТИТЕ")
        listOf(1, 2).forEach { n ->
            val value = if (n == 1) s.fxSend1 else s.fxSend2
            val apply = { nv: Float ->
                val cur = currentUpper(s)
                commitUpperFx(if (n == 1) cur.copy(fxSend1 = nv) else cur.copy(fxSend2 = nv))
            }
            knobControl(c, sec.left + 115f + (n - 1) * 230f, 630f, 42f, value, 0f, 1f, 0.01f, 250f, false,
                "${round(value * 100f).toInt()} %", "FX$n SEND",
                onChange = { nv -> apply(nv) },
                exact = { showNumber("FX$n SEND %", value * 100f, 0f, 100f, 0) { pct -> apply((pct / 100f).coerceIn(0f, 1f)) } },
                reset = { apply(0f) })
        }
        val fx = fxState
        val info = RectF(sec.left + 500f, sec.top + 46f, sec.right - 16f, sec.bottom - 40f)
        fill(c, info, 10f, Color.argb(40, 0, 0, 0))
        if (fx != null) {
            text(c, "FX1", info.left + 24f, info.top + 44f, 14f, C_KNOB_LABEL, face = faceBold, spacing = 0.2f)
            text(c, effectName(fx.fx1.type) + if (fx.fx1.type != 0) "  WET ${round(fx.fx1.wet * 100f).toInt()}%" else "", info.left + 80f, info.top + 46f, 24f, C_AMBER, face = faceMono, maxWidth = info.width() - 340f)
            text(c, "FX2", info.left + 24f, info.top + 94f, 14f, C_KNOB_LABEL, face = faceBold, spacing = 0.2f)
            text(c, effectName(fx.fx2.type) + if (fx.fx2.type != 0) "  WET ${round(fx.fx2.wet * 100f).toInt()}%" else "", info.left + 80f, info.top + 96f, 24f, C_AMBER, face = faceMono, maxWidth = info.width() - 340f)
            text(c, if (fx.routing == 0) "ПАРАЛЕЛНО" else "FX1 → FX2", info.left + 24f, info.top + 136f, 13f, C_KNOB_LABEL, spacing = 0.1f)
        }
        val open = RectF(info.right - 220f, info.centerY() - 22f, info.right - 24f, info.centerY() + 22f)
        lightKey(c, open, "ОТВОРИ ЕФЕКТИ")
        hit(open, { _, _ -> page = Page.EFFECTS; refreshFx() })
        hintLine(c, sec)
    }

    // ------------------------------------------------------------------ LIST

    private fun drawList(c: Canvas) {
        if (!listCacheLoaded) refreshListCache()
        val entries = cachedListItems
        val active = cachedActiveListEntry.takeIf { realListItem(it) != null } ?: -1
        if (active != listFollowedActive) {
            // Follow a newly loaded entry once; the page keys stay free afterwards.
            listFollowedActive = active
            if (active >= 0) listPage = (active / 10).coerceIn(0, 19)
        }
        listPage = listPage.coerceIn(0, 19)

        val keyW = (1214f - 19f * 6f) / 20f
        for (i in 0 until 20) {
            val pr = RectF(112f + i * (keyW + 6f), 108f, 112f + i * (keyW + 6f) + keyW, 156f)
            val hasData = (i * 10 until i * 10 + 10).any { realListItem(it) != null }
            pageKey(c, pr, (i + 1).toString(), i == listPage, hasData, active >= 0 && active / 10 == i)
            hit(pr, { _, _ -> listPage = i })
        }

        val gridTop = 168f
        val gridBottom = 730f
        val rowH = (gridBottom - gridTop - 40f) / 5f
        val colW = (1214f - 10f) / 2f
        for (j in 0 until 10) {
            val idx = listPage * 10 + j
            val col = j % 2
            val row = j / 2
            val rr = RectF(112f + col * (colW + 10f), gridTop + row * (rowH + 10f),
                112f + col * (colW + 10f) + colW, gridTop + row * (rowH + 10f) + rowH)
            val item = realListItem(idx)
            listCard(c, rr, item, idx, idx == active)
            if (item != null) {
                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0, 0, 0, idx)) {
                        cachedActiveListEntry = idx
                        listFollowedActive = idx
                        loadedFromList = idx
                        if (item.combiBank == 0 && item.combiPage == 0) editingCombiSlot = item.combiSlot
                        cachedCombiName = sampler.combiName()
                        refreshAllUppers()
                        refreshFx()
                    }
                })
                listLongHit(rr) { _, _ -> showListContextMenu(idx, item) }
            }
        }

        val activeItem = realListItem(active)
        val editR = RectF(112f, 742f, 300f, 786f)
        lightKey(c, editR, "EDIT COMBI", enabled = activeItem != null)
        if (activeItem != null) {
            hit(editR, { _, _ ->
                if (sampler.activateCombiAddress(activeItem.combiBank, activeItem.combiPage, activeItem.combiSlot)) {
                    editingCombiSlot = activeItem.combiSlot
                    loadedFromList = active
                    page = Page.COMBI
                    cachedCombiName = sampler.combiName()
                    refreshAllUppers()
                    refreshFx()
                }
            })
        }
        text(c, "Докосни = зареди  ·  Задръж = меню: EDIT · RENAME · COPY · MOVE · DELETE",
            316f, 769f, 12f, C_INK_SOFT, maxWidth = 830f)
        val used = entries.count { !sampler.isEmptyListItem(it) }
        text(c, "$used / 200", 1326f, 769f, 13f, C_INK_SOFT, right = true, face = faceMonoBold)
    }

    // SILVETON_LIST_POSITIONS_V1: a LIST position with a BLEND (null = empty position).
    private fun realListItem(index: Int): SamplerAppController.ListItem? =
        cachedListItems.getOrNull(index)?.takeUnless { sampler.isEmptyListItem(it) }

    private fun listCard(c: Canvas, r: RectF, item: SamplerAppController.ListItem?, idx: Int, selected: Boolean) {
        if (selected) fill(c, RectF(r.left - 3f, r.top - 3f, r.right + 3f, r.bottom + 3f), 15f, C_ORANGE)
        else if (item != null) shadow(c, r, 12f, 2f, 50)
        fill(c, r, 12f, when { item == null -> C_EMPTY_CARD; selected -> C_STRIP_SEL; else -> C_STRIP })
        if (isDown(r)) fill(c, r, 12f, Color.argb(28, 0, 0, 0))
        led(c, r.left + 22f, r.centerY(), selected, 5f, light = true)
        val box = RectF(r.left + 40f, r.centerY() - 22f, r.left + 124f, r.centerY() + 22f)
        lcd(c, box, 8f)
        text(c, "%03d".format(idx + 1), box.centerX(), box.centerY() + 8f, 22f,
            if (item != null) C_LCD else C_LCD_DIM, center = true, face = faceMono)
        if (item != null) {
            text(c, item.name, r.left + 142f, r.centerY() + 3f, 24f, C_INK, face = faceBold, maxWidth = r.width() - 160f)
            text(c, blendAddress(item), r.left + 142f, r.centerY() + 27f, 12f, C_LABEL_GREY, face = faceMono, spacing = 0.1f)
        } else {
            text(c, "—", r.left + 142f, r.centerY() + 9f, 24f, C_EMPTY_TEXT)
        }
    }

    private fun blendAddress(item: SamplerAppController.ListItem): String =
        if (item.combiBank == 0 && item.combiPage == 0) "BLEND %03d".format(item.combiSlot)
        else "COMBI B${item.combiBank + 1} P${item.combiPage + 1} S${item.combiSlot}"

    private fun editListCombi(item: SamplerAppController.ListItem, index: Int) {
        if (sampler.activateCombiAddress(item.combiBank, item.combiPage, item.combiSlot)) {
            editingCombiSlot = item.combiSlot
            loadedFromList = index
            page = Page.COMBI
            cachedCombiName = sampler.combiName()
            refreshAllUppers()
            refreshFx()
            invalidate()
        }
    }

    private fun showListContextMenu(index: Int, item: SamplerAppController.ListItem) {
        val actions = arrayOf("EDIT COMBI", "RENAME", "COPY", "MOVE", "DELETE")
        AlertDialog.Builder(context)
            .setTitle("%03d   %s".format(index + 1, item.name))
            .setItems(actions) { dialog, which ->
                when (actions[which]) {
                    "EDIT COMBI" -> editListCombi(item, index)
                    "RENAME" -> showTextPage("RENAME BLEND", item.name) { newName ->
                        if (sampler.renameLiveListEntry(index, newName)) { refreshListCache(); invalidate() }
                    }
                    "COPY" -> openPositionPicker(WriteMode.COPY, index)
                    "MOVE" -> openPositionPicker(WriteMode.MOVE, index)
                    "DELETE" -> AlertDialog.Builder(context)
                        .setTitle("DELETE  ЛИСТ %03d".format(index + 1))
                        .setMessage("„${item.name}“ излиза от ЛИСТА и мястото остава празно. Другите позиции не мърдат, самата бленда не се трие.")
                        .setPositiveButton("DELETE") { _, _ ->
                            if (sampler.deleteLiveListEntry(index)) {
                                if (loadedFromList == index) loadedFromList = -1
                                refreshListCache()
                                invalidate()
                            }
                        }
                        .setNegativeButton("CANCEL", null)
                        .show()
                }
                dialog.dismiss()
            }
            .setNegativeButton("CANCEL", null)
            .show()
    }

    // ------------------------------------------------------------------ WRITE / COPY / MOVE (Korg style)

    private fun openWrite(target: WriteTarget, append: Boolean) {
        writeMode = WriteMode.WRITE
        writeSource = -1
        writeList = sampler.listItems(0, 0, 0)
        writeBlends = sampler.combiCatalog().toMap()
        writeName = sampler.combiName()
        selectWriteTarget(target, append)
        writeOpen = true
        invalidate()
    }

    /** COPY / MOVE a LIST position to any position 1..200 chosen on the same screen. */
    private fun openPositionPicker(mode: WriteMode, source: Int) {
        writeMode = mode
        writeSource = source
        writeTarget = WriteTarget.LIST
        writeList = sampler.listItems(0, 0, 0)
        writeBlends = sampler.combiCatalog().toMap()
        writeName = writeListName(source) ?: ""
        writeSelected = firstFreeListPosition(after = source)
        writePage = if (writeSelected >= 0) writeSelected / 10 else source / 10
        writeOpen = true
        invalidate()
    }

    private fun writeListName(pos: Int): String? = writeList.getOrNull(pos)?.takeUnless { sampler.isEmptyListItem(it) }?.name

    private fun firstFreeListPosition(after: Int = -1): Int {
        for (k in 0 until 200) {
            val pos = (after + 1 + k) % 200
            if (pos != writeSource && writeListName(pos) == null) return pos
        }
        return -1
    }

    private fun selectWriteTarget(target: WriteTarget, append: Boolean) {
        writeTarget = target
        writeSelected = when (target) {
            WriteTarget.LIST ->
                if (!append && writeListName(loadedFromList) != null) loadedFromList
                else (0 until 200).firstOrNull { writeListName(it) == null } ?: -1
            WriteTarget.BLEND -> {
                val own = editingCombiSlot
                if (!append && own != null && own in 1..500) own - 1
                else ((1..500).firstOrNull { !writeBlends.containsKey(it) } ?: 0) - 1
            }
        }
        writePage = if (writeSelected >= 0) writeSelected / 10 else 0
    }

    private fun writeOccupiedName(pos: Int): String? = when (writeTarget) {
        WriteTarget.LIST -> writeListName(pos)
        WriteTarget.BLEND -> writeBlends[pos + 1]
    }

    private fun drawWriteScreen(c: Canvas) {
        fill(c, RectF(0f, 0f, DESIGN_W, DESIGN_H), 0f, Color.argb(170, 0, 0, 0))
        // Modal background: swallows touches outside the panel.
        hit(RectF(0f, 0f, DESIGN_W, DESIGN_H), { _, _ -> })
        val p = RectF(150f, 30f, 1290f, 770f)
        board(c, p)
        val picking = writeMode != WriteMode.WRITE
        val title = when (writeMode) { WriteMode.WRITE -> "WRITE"; WriteMode.COPY -> "COPY"; WriteMode.MOVE -> "MOVE" }
        text(c, title, p.left + 30f, p.top + 50f, 28f, C_AMBER, face = faceMonoBold, spacing = 0.1f)
        text(c, if (picking) "Избери къде да отиде. Заета позиция може да се презапише, другите не мърдат."
            else "Избери място. Заетите позиции могат да се презапишат.",
            p.left + 32f, p.top + 76f, 13f, C_KNOB_LABEL, maxWidth = p.width() - 360f)

        if (!picking) {
            val tl = RectF(p.right - 300f, p.top + 22f, p.right - 160f, p.top + 64f)
            val tb = RectF(p.right - 150f, p.top + 22f, p.right - 30f, p.top + 64f)
            segKey(c, tl, "ЛИСТ", writeTarget == WriteTarget.LIST)
            segKey(c, tb, "BLENDS", writeTarget == WriteTarget.BLEND)
            hit(tl, { _, _ -> if (writeTarget != WriteTarget.LIST) selectWriteTarget(WriteTarget.LIST, false) })
            hit(tb, { _, _ -> if (writeTarget != WriteTarget.BLEND) selectWriteTarget(WriteTarget.BLEND, false) })
        }

        text(c, if (picking) "ОТ" else "ИМЕ", p.left + 30f, p.top + 127f, 12f, C_SECTION_TITLE, spacing = 0.25f)
        val nameR = RectF(p.left + 90f, p.top + 92f, p.right - 180f, p.top + 148f)
        lcd(c, nameR)
        val nameText = if (picking) "%03d  %s".format(writeSource + 1, writeName) else writeName
        text(c, nameText, nameR.left + 16f, nameR.centerY() + 10f, 28f, C_LCD, face = faceMono, maxWidth = nameR.width() - 32f)
        if (!picking) {
            val renR = RectF(p.right - 166f, p.top + 98f, p.right - 30f, p.top + 142f)
            lightKey(c, renR, "RENAME")
            val rename = { showTextPage("RENAME", writeName) { writeName = it } }
            hit(nameR, { _, _ -> rename() })
            hit(renR, { _, _ -> rename() })
        }

        val listTarget = writeTarget == WriteTarget.LIST
        val pages = if (listTarget) 20 else 50
        val perRow = if (listTarget) 20 else 25
        val rows = (pages + perRow - 1) / perRow
        val keyH = if (rows == 1) 44f else 34f
        val keyGap = 5f
        val kx0 = p.left + 30f
        val kx1 = p.right - 30f
        val keysTop = p.top + 166f
        val keyW = (kx1 - kx0 - (perRow - 1) * keyGap) / perRow
        for (k in 0 until pages) {
            val row = k / perRow
            val col = k % perRow
            val kr = RectF(kx0 + col * (keyW + keyGap), keysTop + row * (keyH + keyGap),
                kx0 + col * (keyW + keyGap) + keyW, keysTop + row * (keyH + keyGap) + keyH)
            segKey(c, kr, (k + 1).toString(), k == writePage, size = 12f, withLed = writeSelected >= 0 && writeSelected / 10 == k)
            hit(kr, { _, _ -> writePage = k })
        }
        val keysBottom = keysTop + rows * keyH + (rows - 1) * keyGap

        val gridTop = keysBottom + 14f
        val gridBottom = p.bottom - 92f
        val gap = 8f
        val cellH = (gridBottom - gridTop - 4f * gap) / 5f
        val cellW = (kx1 - kx0 - gap) / 2f
        for (j in 0 until 10) {
            val pos = writePage * 10 + j
            val col = j % 2
            val row = j / 2
            val cell = RectF(kx0 + col * (cellW + gap), gridTop + row * (cellH + gap),
                kx0 + col * (cellW + gap) + cellW, gridTop + row * (cellH + gap) + cellH)
            val isSource = picking && pos == writeSource
            val usable = !isSource && (if (listTarget) pos < 200 else pos < 500)
            writeCell(c, cell, pos + 1, writeOccupiedName(pos), usable, pos == writeSelected, isSource)
            if (usable) hit(cell, { _, _ -> writeSelected = pos })
        }

        val label = if (listTarget) "ЛИСТ" else "BLEND"
        val selName = if (writeSelected >= 0) writeOccupiedName(writeSelected) else null
        val action = when (writeMode) { WriteMode.WRITE -> "ЗАПИС"; WriteMode.COPY -> "КОПИРАНЕ"; WriteMode.MOVE -> "МЕСТЕНЕ" }
        val info = when {
            writeSelected < 0 -> "Избери позиция"
            selName != null -> "$action, ПРЕЗАПИС:  $label %03d  „%s“".format(writeSelected + 1, selName)
            else -> "$action:  $label %03d  (свободна)".format(writeSelected + 1)
        }
        text(c, info, p.left + 30f, p.bottom - 38f, 17f, if (selName != null) C_AMBER else C_LCD_OK, face = faceBold, maxWidth = 760f)

        val cancel = RectF(p.right - 330f, p.bottom - 70f, p.right - 190f, p.bottom - 22f)
        val write = RectF(p.right - 178f, p.bottom - 70f, p.right - 30f, p.bottom - 22f)
        lightKey(c, cancel, "CANCEL", size = 14f)
        hit(cancel, { _, _ -> writeOpen = false })
        accentKey(c, write, title, writeSelected >= 0)
        if (writeSelected >= 0) hit(write, { _, _ -> confirmWrite() })
    }

    private fun writeCell(c: Canvas, r: RectF, number: Int, name: String?, usable: Boolean, selected: Boolean, source: Boolean = false) {
        if (selected) fill(c, RectF(r.left - 3f, r.top - 3f, r.right + 3f, r.bottom + 3f), 12f, C_ORANGE)
        fill(c, r, 10f, when { source -> Color.rgb(96, 84, 64); !usable -> Color.rgb(58, 61, 66); selected -> Color.WHITE; else -> C_STRIP })
        if (usable && isDown(r)) fill(c, r, 10f, Color.argb(30, 0, 0, 0))
        val box = RectF(r.left + 10f, r.centerY() - 18f, r.left + 80f, r.centerY() + 18f)
        lcd(c, box, 7f)
        text(c, "%03d".format(number), box.centerX(), box.centerY() + 7f, 19f, if (usable || source) C_LCD else C_LCD_DIM, center = true, face = faceMono)
        val textX = r.left + 94f
        when {
            source -> {
                text(c, name ?: "", textX, r.centerY() + 7f, 19f, Color.WHITE, face = faceBold, maxWidth = r.width() - 94f - 110f)
                text(c, "ИЗТОЧНИК", r.right - 14f, r.centerY() + 5f, 11f, C_AMBER, right = true, face = faceBold, spacing = 0.15f)
            }
            !usable -> text(c, "—", textX, r.centerY() + 7f, 18f, Color.rgb(120, 124, 130))
            name != null -> {
                text(c, name, textX, r.centerY() + 7f, 19f, C_INK, face = faceBold, maxWidth = r.width() - 94f - 96f)
                text(c, "ЗАЕТО", r.right - 14f, r.centerY() + 5f, 11f, C_ORANGE_DARK, right = true, face = faceBold, spacing = 0.15f)
            }
            else -> {
                text(c, "— свободна —", textX, r.centerY() + 7f, 17f, C_EMPTY_TEXT)
                text(c, "СВОБОДНО", r.right - 14f, r.centerY() + 5f, 11f, C_FREE, right = true, face = faceBold, spacing = 0.15f)
            }
        }
    }

    private fun confirmWrite() {
        val pos = writeSelected
        if (pos < 0) return
        val name = writeName.trim().ifBlank { sampler.combiName() }
        val runIt: () -> Unit = when (writeMode) {
            WriteMode.COPY -> { { doCopyOrMove(false, writeSource, pos) } }
            WriteMode.MOVE -> { { doCopyOrMove(true, writeSource, pos) } }
            WriteMode.WRITE -> if (writeTarget == WriteTarget.LIST) { { doWriteList(pos, name) } } else { { doWriteBlend(pos + 1, name) } }
        }
        val old = writeOccupiedName(pos)
        if (old == null) { runIt(); return }
        val incoming = if (writeMode == WriteMode.WRITE) name else writeName
        val label = if (writeTarget == WriteTarget.LIST) "ЛИСТ" else "BLEND"
        var extra = ""
        if (writeTarget == WriteTarget.BLEND) {
            val users = writeList.filter { it.combiBank == 0 && it.combiPage == 0 && it.combiSlot == pos + 1 }
                .map { "%03d".format(writeList.indexOf(it) + 1) }
            if (users.isNotEmpty()) extra = "\nИзползва се и в ЛИСТ: " + users.joinToString(", ")
        }
        AlertDialog.Builder(context)
            .setTitle("ПРЕЗАПИС  $label %03d".format(pos + 1))
            .setMessage("„$old“ ще бъде заменен с „$incoming“.$extra")
            .setPositiveButton("ПРЕЗАПИШИ") { _, _ -> runIt() }
            .setNegativeButton("CANCEL", null)
            .show()
    }

    private fun doWriteList(pos: Int, name: String) {
        val index = sampler.writeCombiToList(pos, name)
        if (index < 0) { invalidate(); return }
        refreshListCache()
        val item = realListItem(index)
        if (item != null && item.combiBank == 0 && item.combiPage == 0) editingCombiSlot = item.combiSlot
        loadedFromList = index
        listFollowedActive = cachedActiveListEntry
        listPage = (index / 10).coerceIn(0, 19)
        cachedCombiName = sampler.combiName()
        refreshAllUppers()
        refreshFx()
        writeOpen = false
        page = Page.LIST
        flash("ЗАПИСАНО: ЛИСТ %03d".format(index + 1))
    }

    private fun doWriteBlend(slot: Int, name: String) {
        if (!sampler.writeCombiSlot(slot, name)) { invalidate(); return }
        editingCombiSlot = slot
        loadedFromList = -1
        cachedCombiName = sampler.combiName()
        refreshAllUppers()
        refreshFx()
        refreshListCache()
        writeOpen = false
        flash("ЗАПИСАНО: BLEND %03d".format(slot))
    }

    private fun doCopyOrMove(move: Boolean, from: Int, to: Int) {
        val ok = if (move) sampler.moveListEntryTo(from, to) else sampler.copyListEntryTo(from, to)
        if (!ok) { invalidate(); return }
        if (move && loadedFromList == from) loadedFromList = to
        else if (loadedFromList == to) loadedFromList = -1
        refreshListCache()
        listFollowedActive = cachedActiveListEntry.takeIf { realListItem(it) != null } ?: -1
        listPage = (to / 10).coerceIn(0, 19)
        writeOpen = false
        page = Page.LIST
        flash((if (move) "ПРЕМЕСТЕНО: ЛИСТ %03d" else "КОПИРАНО: ЛИСТ %03d").format(to + 1))
    }

    private fun flash(message: String) {
        flashText = message
        flashUntilMs = android.os.SystemClock.elapsedRealtime() + 3500L
        invalidate()
    }

    // ------------------------------------------------------------------ EFFECTS

    private fun drawEffects(c: Canvas) {
        val fx = fxState ?: sampler.combiFxState()?.also { fxState = it } ?: return
        val half = (1214f - 10f) / 2f
        drawFxBoard(c, RectF(112f, 108f, 112f + half, 722f), 1, fx.fx1)
        drawFxBoard(c, RectF(122f + half, 108f, 1326f, 722f), 2, fx.fx2)
        val route = RectF(112f, 734f, 1326f, 786f)
        lightKey(c, route, if (fx.routing == 0) "ROUTING:   FX1 + FX2   ПАРАЛЕЛНО" else "ROUTING:   FX1 → FX2   ПОСЛЕДОВАТЕЛНО", size = 14f)
        hit(route, { _, _ -> val next = if (fx.routing == 0) 1 else 0; if (sampler.updateCombiFxRouting(next)) refreshFx() },
            { if (sampler.updateCombiFxRouting(0)) refreshFx() })
    }

    // SILVETON_FX_V2: every effect shows only the knobs its sound really uses.
    //   DELAY: WET, TIME, FEEDBACK, TONE   CHORUS: WET, RATE, DEPTH   REVERB: WET, DECAY, TONE
    private fun reverbSeconds(feedback: Float): Float = 0.6f * Math.pow(20.0, (feedback.coerceIn(0f, 0.95f) / 0.95f).toDouble()).toFloat()
    private fun reverbFeedbackFor(seconds: Float): Float =
        (0.95f * (kotlin.math.ln(seconds.coerceIn(0.6f, 12f) / 0.6f) / kotlin.math.ln(20f))).coerceIn(0f, 0.95f)

    private fun drawFxBoard(c: Canvas, r: RectF, slot: Int, fx: SamplerAppController.FxState) {
        board(c, r)
        text(c, "FX$slot", r.left + 22f, r.top + 45f, 26f, C_AMBER, face = faceMonoBold)
        val types = listOf(0 to "OFF", 1 to "DELAY", 2 to "CHORUS", 3 to "REVERB")
        val selL = r.left + 110f
        val selR = r.right - 20f
        val w = (selR - selL - 3f * 6f) / 4f
        types.forEachIndexed { k, (t, label) ->
            val b = RectF(selL + k * (w + 6f), r.top + 18f, selL + k * (w + 6f) + w, r.top + 58f)
            segKey(c, b, label, fx.type == t)
            hit(b, { _, _ -> if (sampler.updateCombiFx(slot, fx.copy(type = t))) refreshFx() })
        }
        val sec = RectF(r.left + 16f, r.top + 74f, r.right - 16f, r.bottom - 16f)
        section(c, sec, "ПАРАМЕТРИ · " + effectName(fx.type))

        // index into WET, TIME, FEEDBACK, RATE, DEPTH, TONE
        val shownParams = when (fx.type) {
            1 -> intArrayOf(0, 1, 2, 5)
            2 -> intArrayOf(0, 3, 4)
            3 -> intArrayOf(0, 2, 5)
            else -> intArrayOf()
        }
        if (shownParams.isEmpty()) {
            text(c, "FX$slot е изключен. Избери DELAY, CHORUS или REVERB отгоре.", sec.centerX(), sec.top + 160f, 16f,
                C_KNOB_LABEL, center = true, maxWidth = sec.width() - 40f)
        }
        val reverb = fx.type == 3
        val names = arrayOf("WET", "TIME", if (reverb) "DECAY" else "FEEDBACK", "RATE", "DEPTH", "TONE")
        val values = floatArrayOf(fx.wet, fx.timeMs, fx.feedback, fx.rateHz, fx.depth, fx.tone)
        val mins = floatArrayOf(0f, 1f, 0f, 0.05f, 0f, 0f)
        val maxs = floatArrayOf(1f, 2500f, 0.95f, 8f, 1f, 1f)
        val defs = floatArrayOf(0f, 320f, 0.25f, 0.8f, 0.35f, 0.5f)
        val steps = floatArrayOf(0.01f, 1f, 0.005f, 0.01f, 0.01f, 0.01f)
        val travels = floatArrayOf(250f, 500f, 300f, 300f, 250f, 250f)
        shownParams.forEachIndexed { k, i ->
            val cx = sec.left + sec.width() * (k + 0.5f) / shownParams.size
            val cy = sec.top + 150f
            val v = values[i]
            val shown = when {
                i == 1 -> "${v.toInt()} ms"
                i == 3 -> "%.2f Hz".format(v)
                i == 2 && reverb -> "%.1f s".format(reverbSeconds(v))
                else -> "${round(v * 100f).toInt()} %"
            }
            val apply = { nv: Float ->
                val nf = when (i) {
                    0 -> fx.copy(wet = nv); 1 -> fx.copy(timeMs = nv); 2 -> fx.copy(feedback = nv)
                    3 -> fx.copy(rateHz = nv); 4 -> fx.copy(depth = nv); else -> fx.copy(tone = nv)
                }
                if (sampler.updateCombiFx(slot, nf)) refreshFx()
            }
            knobControl(c, cx, cy, 40f, v, mins[i], maxs[i], steps[i], travels[i], false, shown, names[i],
                hitW = min(200f, sec.width() / shownParams.size - 10f),
                onChange = { nv -> apply(nv) },
                exact = {
                    when {
                        i == 2 && reverb -> showNumber("FX$slot DECAY s", reverbSeconds(v), 0.6f, 12f, 1,
                            restore = { apply(v) }) { apply(reverbFeedbackFor(it)) }
                        i == 1 -> showNumber("FX$slot TIME ms", v, mins[i], maxs[i], 0) { apply(it) }
                        i == 3 -> showNumber("FX$slot RATE Hz", v, mins[i], maxs[i], 2) { apply(it) }
                        else -> showNumber("FX$slot ${names[i]} %", v * 100f, mins[i] * 100f, maxs[i] * 100f, 0,
                            restore = { apply(v) }) { apply((it / 100f).coerceIn(mins[i], maxs[i])) }
                    }
                },
                reset = { apply(defs[i]) })
        }

        if (shownParams.isNotEmpty()) {
            // Which UPPERs send to this effect: an effect is only heard through the FX SEND of an UPPER.
            val sends = (0 until 7).mapNotNull { u ->
                val s = upperStates[u] ?: return@mapNotNull null
                val send = if (slot == 1) s.fxSend1 else s.fxSend2
                if (send > 0.004f && s.soundName != "EMPTY" && s.enabled) "U${u + 1} ${round(send * 100f).toInt()}%" else null
            }
            val y = sec.top + 300f
            text(c, "ПРАЩАТ КЪМ FX$slot", sec.left + 16f, y, 11f, C_SECTION_TITLE, spacing = 0.25f)
            if (sends.isEmpty()) {
                text(c, "Никой UPPER не праща тук – вдигни FX$slot SEND", sec.left + 16f, y + 30f, 15f, C_AMBER, face = faceBold, maxWidth = sec.width() - 32f)
                text(c, "(КОМБИ, долу FX, FX$slot SEND на всеки UPPER)", sec.left + 16f, y + 54f, 13f, C_KNOB_LABEL, maxWidth = sec.width() - 32f)
            } else {
                text(c, sends.joinToString("   "), sec.left + 16f, y + 30f, 15f, C_LCD_OK, face = faceMonoBold, maxWidth = sec.width() - 32f)
            }
            if (fx.wet <= 0.004f) {
                text(c, "WET е 0 % – вдигни WET, за да се чува.", sec.left + 16f, y + 90f, 15f, C_AMBER, face = faceBold, maxWidth = sec.width() - 32f)
            }
        }
    }

    // ------------------------------------------------------------------ SETTINGS

    private fun drawSettings(c: Canvas) {
        val g = globalState ?: sampler.globalUiState()?.also { globalState = it } ?: return
        val b = RectF(112f, 108f, 1326f, 786f)
        board(c, b)
        val labels = arrayOf("MASTER TUNE", "TRANSPOSE", "MASTER GAIN", "EQ LOW", "EQ MID", "EQ HIGH", "LIMITER", "CEILING",
            "VELOCITY CURVE", "BREATH CC", "EXPRESSION CC", "DAMPER CC", "PITCH BEND", "AFTERTOUCH")
        val cols = 5
        val gap = 10f
        val cw = (b.width() - 32f - gap * (cols - 1)) / cols
        val rows = (labels.size + cols - 1) / cols
        val ch = (b.height() - 32f - gap * (rows - 1)) / rows
        labels.indices.forEach { i ->
            val col = i % cols
            val row = i / cols
            val rr = RectF(b.left + 16f + col * (cw + gap), b.top + 16f + row * (ch + gap),
                b.left + 16f + col * (cw + gap) + cw, b.top + 16f + row * (ch + gap) + ch)
            section(c, rr, labels[i])
            val cx = rr.centerX()
            val cy = rr.top + 98f
            when (i) {
                6, 12, 13 -> {
                    val on = when (i) { 6 -> g.limiterEnabled; 12 -> g.pitchBendEnabled; else -> g.aftertouchEnabled }
                    val key = RectF(cx - 80f, rr.top + 56f, cx + 80f, rr.top + 146f)
                    switchKey(c, key, if (on) "ON" else "OFF", "", on)
                    hit(rr, { _, _ -> editGlobal(i, g) }, { resetGlobal(i, g) })
                }
                else -> {
                    val spec = globalSliderSpec(i, g)
                    val bipolar = i == 0 || i == 1 || i in 3..5
                    knobControl(c, cx, cy, 34f, spec.first, spec.second, spec.third, globalStep(i), globalTravel(i), bipolar,
                        globalValueText(i, g), "", hitW = cw - 10f,
                        onChange = { nv -> commitGlobal(globalWithSliderValue(i, globalState ?: g, nv)) },
                        exact = { editGlobal(i, globalState ?: g) },
                        reset = { resetGlobal(i, globalState ?: g) })
                }
            }
        }
    }

    private fun globalStep(i: Int): Float = when (i) { 1, 8, 9, 10, 11 -> 1f; else -> 0.1f }
    private fun globalTravel(i: Int): Float = when (i) {
        0 -> 400f; 1 -> 480f; 2 -> 390f; 3, 4, 5 -> 360f; 7 -> 240f; 8 -> 160f; else -> 508f
    }

    private fun editGlobal(i: Int, g: SamplerAppController.GlobalUiState) {
        when (i) {
            0 -> showNumber("Master Tune Hz", g.masterTuneHz, 400f, 480f, 1) { commitGlobal(g.copy(masterTuneHz = it)) }
            1 -> showNumber("Transpose", g.transpose.toFloat(), -48f, 48f, 0) { commitGlobal(g.copy(transpose = it.toInt())) }
            2 -> showNumber("Master Gain dB", g.masterGainDb, -60f, 18f, 1) { commitGlobal(g.copy(masterGainDb = it)) }
            3 -> showNumber("EQ Low dB", g.eqLowDb, -18f, 18f, 1) { commitGlobal(g.copy(eqLowDb = it)) }
            4 -> showNumber("EQ Mid dB", g.eqMidDb, -18f, 18f, 1) { commitGlobal(g.copy(eqMidDb = it)) }
            5 -> showNumber("EQ High dB", g.eqHighDb, -18f, 18f, 1) { commitGlobal(g.copy(eqHighDb = it)) }
            6 -> commitGlobal(g.copy(limiterEnabled = !g.limiterEnabled))
            7 -> showNumber("Limiter Ceiling", g.limiterCeilingDb, -12f, 0f, 1) { commitGlobal(g.copy(limiterCeilingDb = it)) }
            8 -> showNumber("Velocity Curve", g.velocityCurve.toFloat(), 0f, 4f, 0) { commitGlobal(g.copy(velocityCurve = it.toInt())) }
            9 -> showNumber("Breath CC", g.breathCc.toFloat(), 0f, 127f, 0) { commitGlobal(g.copy(breathCc = it.toInt())) }
            10 -> showNumber("Expression CC", g.expressionCc.toFloat(), 0f, 127f, 0) { commitGlobal(g.copy(expressionCc = it.toInt())) }
            11 -> showNumber("Damper CC", g.damperCc.toFloat(), 0f, 127f, 0) { commitGlobal(g.copy(damperCc = it.toInt())) }
            12 -> commitGlobal(g.copy(pitchBendEnabled = !g.pitchBendEnabled))
            13 -> commitGlobal(g.copy(aftertouchEnabled = !g.aftertouchEnabled))
        }
    }

    private fun resetGlobal(i: Int, g: SamplerAppController.GlobalUiState) {
        val n = when (i) {
            0 -> g.copy(masterTuneHz = 440f); 1 -> g.copy(transpose = 0); 2 -> g.copy(masterGainDb = 0f)
            3 -> g.copy(eqLowDb = 0f); 4 -> g.copy(eqMidDb = 0f); 5 -> g.copy(eqHighDb = 0f)
            6 -> g.copy(limiterEnabled = true); 7 -> g.copy(limiterCeilingDb = -0.3f); 8 -> g.copy(velocityCurve = 0)
            9 -> g.copy(breathCc = 2); 10 -> g.copy(expressionCc = 11); 11 -> g.copy(damperCc = 64)
            12 -> g.copy(pitchBendEnabled = true); else -> g.copy(aftertouchEnabled = true)
        }
        commitGlobal(n)
    }

    private fun globalSliderSpec(i: Int, g: SamplerAppController.GlobalUiState): Triple<Float, Float, Float> = when (i) {
        0 -> Triple(g.masterTuneHz, 400f, 480f); 1 -> Triple(g.transpose.toFloat(), -48f, 48f); 2 -> Triple(g.masterGainDb, -60f, 18f)
        3 -> Triple(g.eqLowDb, -18f, 18f); 4 -> Triple(g.eqMidDb, -18f, 18f); 5 -> Triple(g.eqHighDb, -18f, 18f)
        7 -> Triple(g.limiterCeilingDb, -12f, 0f); 8 -> Triple(g.velocityCurve.toFloat(), 0f, 4f)
        9 -> Triple(g.breathCc.toFloat(), 0f, 127f); 10 -> Triple(g.expressionCc.toFloat(), 0f, 127f); 11 -> Triple(g.damperCc.toFloat(), 0f, 127f)
        else -> Triple(0f, 0f, 1f)
    }

    private fun globalWithSliderValue(i: Int, g: SamplerAppController.GlobalUiState, v: Float): SamplerAppController.GlobalUiState = when (i) {
        0 -> g.copy(masterTuneHz = v); 1 -> g.copy(transpose = round(v).toInt()); 2 -> g.copy(masterGainDb = v)
        3 -> g.copy(eqLowDb = v); 4 -> g.copy(eqMidDb = v); 5 -> g.copy(eqHighDb = v); 7 -> g.copy(limiterCeilingDb = v)
        8 -> g.copy(velocityCurve = round(v).toInt()); 9 -> g.copy(breathCc = round(v).toInt())
        10 -> g.copy(expressionCc = round(v).toInt()); 11 -> g.copy(damperCc = round(v).toInt()); else -> g
    }

    private fun globalValueText(i: Int, g: SamplerAppController.GlobalUiState): String = when (i) {
        0 -> "%.1f Hz".format(g.masterTuneHz); 1 -> signed(g.transpose); 2 -> "%.1f dB".format(g.masterGainDb)
        3 -> "%.1f dB".format(g.eqLowDb); 4 -> "%.1f dB".format(g.eqMidDb); 5 -> "%.1f dB".format(g.eqHighDb)
        7 -> "%.1f dB".format(g.limiterCeilingDb); 8 -> g.velocityCurve.toString(); 9 -> "CC ${g.breathCc}"
        10 -> "CC ${g.expressionCc}"; 11 -> "CC ${g.damperCc}"; else -> ""
    }

    // ------------------------------------------------------------------ dialogs

    private fun showBlendPicker() {
        val blends = sampler.combiCatalog()
        if (blends.isEmpty()) {
            AlertDialog.Builder(context)
                .setTitle("BLENDS")
                .setMessage("Няма записани COMBI / BLEND позиции в заредената банка.")
                .setPositiveButton("OK", null)
                .show()
            return
        }
        val labels = blends.map { item ->
            "%03d   %s".format(item.first, item.second.ifBlank { "COMBI ${item.first}" })
        }.toTypedArray()
        // Open the list on the blend that is loaded now, not on position 1.
        val checkedBlend = editingCombiSlot?.let { slot -> blends.indexOfFirst { it.first == slot } } ?: -1
        AlertDialog.Builder(context)
            .setTitle("BLENDS  1–500")
            .setSingleChoiceItems(labels, checkedBlend) { d, index ->
                d.dismiss()
                val item = blends[index]
                if (sampler.activateCombiAddress(0, 0, item.first)) {
                    editingCombiSlot = item.first
                    loadedFromList = -1
                    page = Page.COMBI
                    cachedCombiName = sampler.combiName()
                    refreshAllUppers()
                    refreshFx()
                    invalidate()
                }
            }
            .setNegativeButton("CANCEL", null)
            .show()
    }

    private fun showSoundPicker(upper: Int) {
        val sounds = sampler.soundCatalog()
        if (sounds.isEmpty()) {
            val detail = sampler.bankLoadError().ifBlank {
                "SILVETON_BANK.sv не е заредена или няма SOUND позиции."
            }
            AlertDialog.Builder(context)
                .setTitle("BANK HAS NO SOUNDS")
                .setMessage(detail)
                .setPositiveButton("OK", null)
                .show()
            return
        }
        val labels = ArrayList<String>(sounds.size + 1)
        labels.add("EMPTY")
        sounds.forEach { item -> labels.add("%03d   %s".format(item.slot, item.name)) }
        // Open the list on this Upper's own sound (e.g. position 53), not on position 1.
        val picked = lastPickedSound.getOrNull(upper)
        val currentName = upperStates.getOrNull(upper)?.soundName ?: sampler.upperState(upper)?.soundName
        var checked = -1
        if (picked != null && picked.name == currentName) {
            val k = sounds.indexOfFirst { it.bank == picked.bank && it.page == picked.page && it.slot == picked.slot }
            if (k >= 0) checked = k + 1
        }
        if (checked < 0 && currentName != null) {
            checked = if (currentName == "EMPTY") 0 else sounds.indexOfFirst { it.name == currentName }.let { if (it >= 0) it + 1 else -1 }
        }
        AlertDialog.Builder(context).setTitle("UPPER ${upper + 1} — SOUND").setSingleChoiceItems(labels.toTypedArray(), checked) { d, which ->
            if (which == 0) { sampler.clearUpperSound(upper); lastPickedSound[upper] = null }
            else { sampler.assignUpperSound(upper, sounds[which - 1]); lastPickedSound[upper] = sounds[which - 1] }
            refreshUpper(upper); d.dismiss(); invalidate()
        }.setNegativeButton("CANCEL", null).show()
    }

    private fun showKeyRange(upper: Int, s: SamplerAppController.UpperState) {
        showTwoInts("KEY RANGE", "Low 0-127", s.keyLow, "High 0-127", s.keyHigh, 0, 127) { lo, hi -> if (lo <= hi) commitMain(upper, s.copy(keyLow = lo, keyHigh = hi)) }
    }

    private fun showVelocityRange(upper: Int, s: SamplerAppController.UpperState) {
        showTwoInts("VELOCITY RANGE", "Low 1-127", s.velocityLow, "High 1-127", s.velocityHigh, 1, 127) { lo, hi -> if (lo <= hi) commitRange(s.copy(velocityLow = lo, velocityHigh = hi)) }
    }

    private fun uiPx(dp: Float): Int = (dp * resources.displayMetrics.density + 0.5f).toInt()

    private fun editorLabel(textValue: String): TextView = TextView(context).apply {
        text = textValue
        setTextColor(Color.rgb(158, 170, 184))
        textSize = 12f
        setPadding(0, uiPx(10f), 0, uiPx(6f))
    }

    private fun editorField(value: String, numeric: Boolean = true): EditText = EditText(context).apply {
        inputType = if (numeric)
            InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
        else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
        setText(value)
        setSelection(text.length)
        setTextColor(Color.WHITE)
        setHintTextColor(Color.rgb(110, 120, 132))
        textSize = if (numeric) 30f else 22f
        gravity = Gravity.CENTER
        setPadding(uiPx(18f), uiPx(12f), uiPx(18f), uiPx(12f))
        background = GradientDrawable().apply {
            shape = GradientDrawable.RECTANGLE
            cornerRadius = uiPx(10f).toFloat()
            setColor(Color.rgb(31, 36, 43))
            setStroke(uiPx(1f), Color.rgb(72, 87, 104))
        }
    }

    private fun showEditorPage(title: String, subtitle: String, body: LinearLayout, applyLabel: String = "APPLY",
                               onCancel: (() -> Unit)? = null, onApply: () -> Unit) {
        val dialog = Dialog(context)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(uiPx(28f), uiPx(22f), uiPx(28f), uiPx(22f))
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = uiPx(16f).toFloat()
                setColor(Color.rgb(20, 24, 29))
                setStroke(uiPx(1f), Color.rgb(68, 84, 102))
            }
        }
        root.addView(TextView(context).apply {
            text = title; setTextColor(Color.WHITE); textSize = 25f; setTypeface(typeface, Typeface.BOLD)
        })
        root.addView(TextView(context).apply {
            text = subtitle; setTextColor(Color.rgb(135, 151, 168)); textSize = 13f
            setPadding(0, uiPx(4f), 0, uiPx(14f))
        })
        root.addView(body, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        val buttons = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.END; setPadding(0, uiPx(20f), 0, 0) }
        val cancel = Button(context).apply { text = "CANCEL"; isAllCaps = false }
        val apply = Button(context).apply { text = applyLabel; isAllCaps = false }
        buttons.addView(cancel); buttons.addView(apply); root.addView(buttons)
        dialog.setContentView(root)
        cancel.setOnClickListener { onCancel?.invoke(); dialog.dismiss() }
        dialog.setOnCancelListener { onCancel?.invoke() }
        apply.setOnClickListener { onApply(); dialog.dismiss() }
        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            setDimAmount(0.76f)
            addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setLayout((resources.displayMetrics.widthPixels * 0.78f).toInt(), WindowManager.LayoutParams.WRAP_CONTENT)
        }
    }

    private fun showTextPage(title: String, current: String, done: (String) -> Unit) {
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("NAME"))
        val edit = editorField(current, false); body.addView(edit)
        showEditorPage(title, "Edit the name and confirm.", body) {
            val value = edit.text.toString().trim(); if (value.isNotEmpty()) done(value); invalidate()
        }
    }

    private fun showTwoInts(title: String, l1: String, v1: Int, l2: String, v2: Int, minV: Int, maxV: Int, done: (Int, Int) -> Unit) {
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel(l1)); val a = editorField(v1.toString()); body.addView(a)
        body.addView(editorLabel(l2)); val b = editorField(v2.toString()); body.addView(b)
        showEditorPage(title, "Allowed range: $minV … $maxV", body) {
            val x = a.text.toString().toIntOrNull()?.coerceIn(minV, maxV) ?: v1
            val y = b.text.toString().toIntOrNull()?.coerceIn(minV, maxV) ?: v2
            done(x, y); invalidate()
        }
    }

    /**
     * Number entry. SILVETON_LIVE_EDIT_V1: with [live] the slider and − / + change the
     * sound at once (you hear it on the notes you hold); CANCEL / Back puts the old
     * value back ([restore], or the starting value). A typed number counts on APPLY.
     */
    private fun showNumber(title: String, current: Float, minV: Float, maxV: Float, decimals: Int,
                           live: Boolean = true, restore: (() -> Unit)? = null, done: (Float) -> Unit) {
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        var previewed = false
        var lastPreview = current
        fun preview(value: Float) {
            if (!live) return
            val v = value.coerceIn(minV, maxV)
            if (previewed && v == lastPreview) return
            lastPreview = v
            previewed = true
            done(v)
            invalidate()
        }
        fun formatEditorValue(value: Float): String = if (decimals == 0) round(value).toInt().toString() else "% .${decimals}f".format(value).trim()
        val scale = when { decimals <= 0 -> 1; decimals == 1 -> 10; else -> 100 }
        val sliderMax = max(1, round((maxV - minV) * scale).toInt())
        body.addView(editorLabel("VALUE"))
        val edit = editorField(formatEditorValue(current.coerceIn(minV, maxV)))
        body.addView(edit)
        body.addView(editorLabel("RANGE   ${formatEditorValue(minV)}  …  ${formatEditorValue(maxV)}"))
        var syncing = false
        val seek = SeekBar(context).apply {
            max = sliderMax
            progress = round((current.coerceIn(minV, maxV) - minV) * scale).toInt().coerceIn(0, sliderMax)
        }
        fun setEditorValue(value: Float) {
            val clamped = value.coerceIn(minV, maxV)
            syncing = true
            val newText = formatEditorValue(clamped)
            edit.setText(newText)
            edit.setSelection(newText.length)
            seek.progress = round((clamped - minV) * scale).toInt().coerceIn(0, sliderMax)
            syncing = false
        }
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (!fromUser || syncing) return
                val value = minV + progress.toFloat() / scale.toFloat()
                setEditorValue(value)
                preview(value)
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        body.addView(seek)
        val fine = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, uiPx(8f), 0, 0) }
        val step = 1f / scale.toFloat()
        fun currentTyped(): Float = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
        val minus = Button(context).apply { text = "−"; textSize = 20f; setOnClickListener { val v = (currentTyped() - step).coerceIn(minV, maxV); setEditorValue(v); preview(v) } }
        val plus = Button(context).apply { text = "+"; textSize = 20f; setOnClickListener { val v = (currentTyped() + step).coerceIn(minV, maxV); setEditorValue(v); preview(v) } }
        fine.addView(minus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        fine.addView(plus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        body.addView(fine)
        val subtitle = if (live) "Плъзгай или натискай − / + : чуваш веднага. Или напиши число и натисни APPLY."
            else "Drag smoothly or type the exact value."
        showEditorPage(title, subtitle, body, onCancel = {
            if (previewed) {
                if (restore != null) restore() else done(current.coerceIn(minV, maxV))
                invalidate()
            }
        }) {
            val value = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
            done(value)
            invalidate()
        }
    }

    // ------------------------------------------------------------------ commits

    private fun commitMain(i: Int, s: SamplerAppController.UpperState) {
        upperStates[i] = s
        if (!sampler.updateUpperMain(i, s)) refreshUpper(i)
    }
    private fun commitTune(s: SamplerAppController.UpperState) {
        upperStates[selectedUpper] = s
        if (!sampler.updateUpperTune(selectedUpper, s)) refreshUpper(selectedUpper)
    }
    private fun commitRange(s: SamplerAppController.UpperState) {
        upperStates[selectedUpper] = s
        if (!sampler.updateUpperRange(selectedUpper, s)) refreshUpper(selectedUpper)
    }
    private fun commitControl(s: SamplerAppController.UpperState) {
        upperStates[selectedUpper] = s
        if (!sampler.updateUpperControl(selectedUpper, s)) refreshUpper(selectedUpper)
    }
    private fun commitTone(s: SamplerAppController.UpperState) {
        upperStates[selectedUpper] = s
        if (!sampler.updateUpperTone(selectedUpper, s)) refreshUpper(selectedUpper)
    }
    private fun commitUpperFx(s: SamplerAppController.UpperState) {
        upperStates[selectedUpper] = s
        if (!sampler.updateUpperFx(selectedUpper, s)) refreshUpper(selectedUpper)
    }
    private fun commitGlobal(g: SamplerAppController.GlobalUiState) {
        globalState = g
        if (!sampler.updateGlobalUi(g)) refreshGlobal()
    }

    private fun refreshUpper(i: Int) { upperStates[i] = sampler.upperState(i) }
    private fun refreshAllUppers() { for (i in 0 until 7) refreshUpper(i) }
    private fun refreshFx() { fxState = sampler.combiFxState() }
    private fun refreshGlobal() { globalState = sampler.globalUiState() }
    /** MASTER changed from outside the screen (tablet volume keys). */
    fun globalChangedOutside() { refreshGlobal(); invalidate() }
    private fun refreshListCache() {
        cachedListItems = sampler.listItems(0, 0, 0)
        cachedActiveListEntry = sampler.activeListEntry(0, 0, 0)
        listCacheLoaded = true
    }

    private fun hit(r: RectF, tap: (Float, Float) -> Unit, reset: (() -> Unit)? = null, drag: ((Float, Float, Boolean) -> Unit)? = null) {
        hits.add(Hit(RectF(r), tap, reset, drag))
    }
    private fun hitAt(x: Float, y: Float): Hit? = hits.asReversed().firstOrNull { it.rect.contains(x, y) }

    private fun listLongHit(r: RectF, action: (Float, Float) -> Unit) {
        listLongHits.add(ListLongHit(RectF(r), action))
    }
    private fun listLongHitAt(x: Float, y: Float): ListLongHit? =
        listLongHits.asReversed().firstOrNull { it.rect.contains(x, y) }

    // ------------------------------------------------------------------ drawing kit

    private fun isDown(r: RectF): Boolean {
        val h = activeHit ?: return false
        if (dragMoved) return false
        return abs(h.rect.left - r.left) < 0.5f && abs(h.rect.top - r.top) < 0.5f &&
            abs(h.rect.right - r.right) < 0.5f && abs(h.rect.bottom - r.bottom) < 0.5f
    }

    private fun fill(c: Canvas, r: RectF, radius: Float, color: Int) {
        paint.shader = null
        paint.style = Paint.Style.FILL
        paint.color = color
        if (radius <= 0f) c.drawRect(r, paint) else c.drawRoundRect(r, radius, radius, paint)
    }

    private fun fillV(c: Canvas, r: RectF, radius: Float, top: Int, bottom: Int) {
        paint.style = Paint.Style.FILL
        paint.color = Color.BLACK
        paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, top, bottom, Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null
    }

    private fun shadow(c: Canvas, r: RectF, radius: Float, dy: Float, alpha: Int) {
        fill(c, RectF(r.left - 1f, r.top + dy - 0.5f, r.right + 1f, r.bottom + dy + 1.5f), radius + 1f, Color.argb(alpha / 2, 0, 0, 0))
        fill(c, RectF(r.left, r.top + dy, r.right, r.bottom + dy), radius, Color.argb(alpha, 0, 0, 0))
    }

    private fun line(c: Canvas, x1: Float, y1: Float, x2: Float, y2: Float, color: Int, w: Float) {
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = w
        paint.color = color
        c.drawLine(x1, y1, x2, y2, paint)
        paint.style = Paint.Style.FILL
    }

    private fun text(
        c: Canvas, s: String, x: Float, y: Float, size: Float, color: Int,
        center: Boolean = false, face: Typeface = faceRegular, spacing: Float = 0f,
        maxWidth: Float = Float.POSITIVE_INFINITY, right: Boolean = false
    ) {
        paint.shader = null
        paint.style = Paint.Style.FILL
        paint.color = color
        paint.textSize = size
        paint.typeface = face
        paint.letterSpacing = spacing
        paint.textAlign = when { center -> Paint.Align.CENTER; right -> Paint.Align.RIGHT; else -> Paint.Align.LEFT }
        var shown = s
        if (maxWidth != Float.POSITIVE_INFINITY && maxWidth > 1f) {
            val measured = paint.measureText(shown)
            if (measured > maxWidth) {
                paint.textSize = max(size * 0.72f, size * (maxWidth / measured))
                if (paint.measureText(shown) > maxWidth) {
                    val ell = "…"
                    val room = max(0f, maxWidth - paint.measureText(ell))
                    var count = shown.length
                    while (count > 0 && paint.measureText(shown, 0, count) > room) count--
                    shown = if (count <= 0) ell else shown.take(count) + ell
                }
            }
        }
        c.drawText(shown, x, y, paint)
        paint.textAlign = Paint.Align.LEFT
        paint.letterSpacing = 0f
    }

    private fun textWidth(s: String, size: Float, face: Typeface, spacing: Float = 0f): Float {
        paint.textSize = size
        paint.typeface = face
        paint.letterSpacing = spacing
        val w = paint.measureText(s)
        paint.letterSpacing = 0f
        return w
    }

    private fun led(c: Canvas, x: Float, y: Float, on: Boolean, radius: Float, light: Boolean = false) {
        paint.style = Paint.Style.FILL
        paint.color = Color.BLACK
        if (on) {
            paint.shader = RadialGradient(x, y, radius * 2.8f, colorWithAlpha(C_ORANGE, 120), colorWithAlpha(C_ORANGE, 0), Shader.TileMode.CLAMP)
            c.drawCircle(x, y, radius * 2.8f, paint)
            paint.shader = RadialGradient(x - radius * 0.3f, y - radius * 0.3f, radius * 1.2f, Color.rgb(255, 214, 170), C_ORANGE, Shader.TileMode.CLAMP)
        } else {
            val base = if (light) C_LED_OFF_LIGHT else C_LED_OFF
            paint.shader = RadialGradient(x - radius * 0.3f, y - radius * 0.3f, radius * 1.2f, lighten(base, 30), base, Shader.TileMode.CLAMP)
        }
        c.drawCircle(x, y, radius, paint)
        paint.shader = null
    }

    private fun navKey(c: Canvas, r: RectF, on: Boolean) {
        shadow(c, r, 12f, 2f, 100)
        fillV(c, r, 12f, if (on) Color.rgb(66, 69, 75) else Color.rgb(50, 53, 58), if (on) Color.rgb(54, 57, 63) else Color.rgb(41, 44, 49))
        line(c, r.left + 10f, r.top + 1f, r.right - 10f, r.top + 1f, C_NAV_EDGE, 1f)
        if (isDown(r)) fill(c, r, 12f, Color.argb(70, 0, 0, 0))
    }

    private fun lightKey(c: Canvas, r: RectF, label: String, enabled: Boolean = true, size: Float = 12f, radius: Float = 8f) {
        val down = enabled && isDown(r)
        if (!down) shadow(c, r, radius, 2f, 45)
        fill(c, r, radius, C_KEY_LIGHT_EDGE)
        val face = RectF(r.left, r.top + if (down) 1f else 0f, r.right, r.bottom - if (down) 0.5f else 2f)
        fill(c, face, radius, when { !enabled -> Color.rgb(214, 216, 220); down -> Color.rgb(212, 215, 220); else -> C_KEY_LIGHT })
        text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (enabled) C_KEY_TEXT else C_EMPTY_TEXT,
            center = true, face = faceBold, spacing = 0.1f, maxWidth = r.width() - 12f)
    }

    private fun darkKey(c: Canvas, r: RectF, label: String) {
        val down = isDown(r)
        fill(c, r, 6f, Color.BLACK)
        fill(c, RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f), 6f, if (down) Color.rgb(28, 30, 34) else C_DARK_KEY)
        text(c, label, r.centerX(), r.centerY() + 4f, 11f, C_AMBER, center = true, face = faceMonoBold, spacing = 0.06f, maxWidth = r.width() - 8f)
    }

    private fun stripToggle(c: Canvas, r: RectF, label: String, on: Boolean, onColor: Int) {
        val down = isDown(r)
        if (on) {
            fill(c, r, 6f, Color.BLACK)
            fill(c, RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f), 6f, C_DARK_KEY)
        } else {
            fill(c, r, 6f, C_KEY_LIGHT_EDGE)
            fill(c, RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f), 6f, if (down) Color.rgb(205, 208, 213) else C_TOGGLE)
        }
        text(c, label, r.centerX(), r.centerY() + 4f, 11f, if (on) onColor else C_TOGGLE_TEXT, center = true, face = faceBold)
    }

    private fun segKey(c: Canvas, r: RectF, label: String, on: Boolean, size: Float = 13f, withLed: Boolean = false) {
        val down = isDown(r)
        if (!down) shadow(c, r, 8f, 2f, 70)
        fillV(c, r, 8f, if (on) Color.rgb(30, 32, 36) else Color.rgb(74, 78, 85), if (on) Color.rgb(22, 24, 27) else Color.rgb(58, 61, 67))
        if (down) fill(c, r, 8f, Color.argb(60, 0, 0, 0))
        if (on) fill(c, RectF(r.left + 8f, r.bottom - 5f, r.right - 8f, r.bottom - 3f), 1f, C_ORANGE)
        if (withLed) led(c, r.right - 7f, r.top + 7f, true, 2.5f)
        text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (on) C_AMBER else Color.rgb(205, 209, 215),
            center = true, face = faceBold, spacing = 0.1f, maxWidth = r.width() - 8f)
    }

    private fun accentKey(c: Canvas, r: RectF, label: String, enabled: Boolean) {
        val down = enabled && isDown(r)
        if (!down) shadow(c, r, 10f, 3f, 90)
        fillV(c, r, 10f,
            if (!enabled) Color.rgb(96, 99, 105) else if (down) Color.rgb(214, 92, 10) else Color.rgb(255, 146, 62),
            if (!enabled) Color.rgb(80, 83, 89) else if (down) Color.rgb(180, 74, 6) else Color.rgb(214, 92, 10))
        text(c, label, r.centerX(), r.centerY() + 6f, 16f, if (enabled) Color.WHITE else Color.rgb(150, 153, 158),
            center = true, face = faceBold, spacing = 0.2f)
    }

    private fun switchKey(c: Canvas, r: RectF, title: String, sub: String, on: Boolean) {
        val down = isDown(r)
        if (!down) shadow(c, r, 12f, 3f, 90)
        fillV(c, r, 12f, if (on) Color.rgb(40, 42, 47) else Color.rgb(70, 74, 80), if (on) Color.rgb(28, 30, 34) else Color.rgb(56, 59, 65))
        if (down) fill(c, r, 12f, Color.argb(60, 0, 0, 0))
        led(c, r.left + 18f, r.top + 18f, on, 5f)
        text(c, title, r.centerX(), r.centerY() + if (sub.isEmpty()) 8f else 0f, if (sub.isEmpty()) 22f else 15f,
            if (on) C_AMBER else Color.rgb(205, 209, 215), center = true, face = faceBold, spacing = 0.08f, maxWidth = r.width() - 40f)
        if (sub.isNotEmpty()) text(c, sub, r.centerX(), r.centerY() + 20f, 11f, C_KNOB_LABEL, center = true, face = faceMono)
        if (sub.isNotEmpty() || title.length > 3) {
            text(c, if (on) "ON" else "OFF", r.right - 14f, r.top + 22f, 10f, if (on) C_AMBER else C_KNOB_LABEL, right = true, face = faceBold, spacing = 0.15f)
        }
    }

    private fun pageKey(c: Canvas, r: RectF, label: String, on: Boolean, hasData: Boolean, ledOn: Boolean) {
        if (on) {
            shadow(c, r, 8f, 2f, 60)
            fill(c, r, 8f, Color.BLACK)
            fill(c, RectF(r.left, r.top, r.right, r.bottom - 2f), 8f, C_DARK_KEY)
            text(c, label, r.centerX(), r.top + 24f, 16f, C_AMBER, center = true, face = faceMonoBold)
        } else {
            lightKey(c, r, "", enabled = true)
            text(c, label, r.centerX(), r.top + 24f, 16f, if (hasData) C_KEY_TEXT else C_EMPTY_TEXT, center = true, face = faceMonoBold)
        }
        led(c, r.centerX(), r.bottom - 11f, ledOn, 3f, light = !on)
    }

    private fun lcd(c: Canvas, r: RectF, radius: Float = 10f) {
        fill(c, RectF(r.left, r.top + 1f, r.right, r.bottom + 1f), radius, Color.argb(150, 255, 255, 255))
        fillV(c, r, radius, C_LCD_TOP, C_LCD_BOT)
        paint.color = Color.BLACK
        paint.shader = LinearGradient(0f, r.top, 0f, r.top + 9f, Color.argb(170, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawRoundRect(r, radius, radius, paint)
        paint.shader = null
    }

    private fun board(c: Canvas, r: RectF, squareTopLeft: Boolean = false) {
        shadow(c, r, 14f, 4f, 80)
        fillV(c, r, 14f, C_BOARD_TOP, C_BOARD_BOT)
        if (squareTopLeft) {
            paint.color = Color.BLACK
            paint.shader = LinearGradient(0f, r.top, 0f, r.bottom, C_BOARD_TOP, C_BOARD_BOT, Shader.TileMode.CLAMP)
            c.drawRect(r.left, r.top, r.left + 16f, r.top + 16f, paint)
            paint.shader = null
        }
        line(c, r.left + (if (squareTopLeft) 1f else 14f), r.top + 1f, r.right - 14f, r.top + 1f, C_BOARD_EDGE, 1f)
    }

    private fun section(c: Canvas, r: RectF, title: String?) {
        fill(c, r, 12f, C_SECTION)
        line(c, r.left + 12f, r.top + 1f, r.right - 12f, r.top + 1f, Color.argb(20, 255, 255, 255), 1f)
        if (title != null) {
            text(c, title, r.left + 16f, r.top + 24f, 11f, C_SECTION_TITLE, spacing = 0.25f, maxWidth = r.width() - 32f)
            fill(c, RectF(r.left + 16f, r.top + 30f, r.left + 46f, r.top + 32f), 1f, C_ORANGE)
        }
    }

    private fun knobControl(
        c: Canvas, cx: Float, cy: Float, radius: Float,
        value: Float, minV: Float, maxV: Float, step: Float, travel: Float,
        bipolar: Boolean, valueText: String, label: String,
        hitW: Float = radius * 2f + 120f,
        onChange: (Float) -> Unit, exact: () -> Unit, reset: () -> Unit,
        onFinal: ((Float) -> Unit)? = null
    ) {
        val t = if (maxV > minV) ((value - minV) / (maxV - minV)).coerceIn(0f, 1f) else 0f
        drawKnob(c, cx, cy, radius, t, bipolar, ring = true)
        val vy = cy + radius + 30f
        text(c, valueText, cx, vy, if (radius >= 40f) 19f else 15f, C_AMBER, center = true, face = faceMono, maxWidth = hitW - 6f)
        if (label.isNotEmpty()) text(c, label, cx, vy + 16f, if (radius >= 40f) 11f else 10f, C_KNOB_LABEL, center = true, spacing = 0.15f, maxWidth = hitW - 6f)
        knobHit(RectF(cx - hitW / 2f, cy - radius - 16f, cx + hitW / 2f, cy + radius + 10f), value, minV, maxV, step, travel,
            onChange, exact, reset, onFinal)
        hit(RectF(cx - hitW / 2f, cy + radius + 10f, cx + hitW / 2f, vy + 22f), { _, _ -> exact() }, reset)
    }

    private fun drawKnob(c: Canvas, cx: Float, cy: Float, r: Float, t: Float, bipolar: Boolean, ring: Boolean) {
        val tt = t.coerceIn(0f, 1f)
        paint.style = Paint.Style.FILL
        if (ring) {
            val rr = r + 8f
            val arc = RectF(cx - rr, cy - rr, cx + rr, cy + rr)
            paint.shader = null
            paint.style = Paint.Style.STROKE
            paint.strokeWidth = 4f
            paint.strokeCap = Paint.Cap.ROUND
            paint.color = Color.argb(110, 0, 0, 0)
            c.drawArc(arc, 135f, 270f, false, paint)
            paint.color = C_AMBER
            if (bipolar) {
                val sweep = (tt - 0.5f) * 270f
                if (abs(sweep) > 0.5f) c.drawArc(arc, 270f, sweep, false, paint)
            } else if (tt > 0.002f) {
                c.drawArc(arc, 135f, 270f * tt, false, paint)
            }
            paint.strokeCap = Paint.Cap.BUTT
            paint.style = Paint.Style.FILL
        }
        paint.color = Color.BLACK
        paint.shader = RadialGradient(cx, cy + r * 0.16f, r * 1.22f, Color.argb(150, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy + r * 0.12f, r * 1.22f, paint)
        paint.shader = SweepGradient(cx, cy, intArrayOf(C_KNOB_A, C_KNOB_B, C_KNOB_A, C_KNOB_C, C_KNOB_A), null)
        c.drawCircle(cx, cy, r, paint)
        paint.shader = RadialGradient(cx, cy - r * 0.6f, r * 0.95f, colorWithAlpha(Color.rgb(252,253,255), 60),
            colorWithAlpha(Color.rgb(252,253,255), 0), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy, r, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1.2f
        paint.color = Color.argb(150, 0, 0, 0)
        c.drawCircle(cx, cy, r, paint)
        val ang = Math.toRadians(135.0 + 270.0 * tt)
        val cos = kotlin.math.cos(ang).toFloat()
        val sin = kotlin.math.sin(ang).toFloat()
        paint.color = C_ORANGE
        paint.strokeWidth = max(3f, r * 0.09f)
        paint.strokeCap = Paint.Cap.ROUND
        c.drawLine(cx + cos * r * 0.40f, cy + sin * r * 0.40f, cx + cos * r * 0.84f, cy + sin * r * 0.84f, paint)
        paint.strokeCap = Paint.Cap.BUTT
        paint.style = Paint.Style.FILL
    }

    private fun stripMeter(c: Canvas, r: RectF, peak: Float) {
        fill(c, r, 3f, C_METER_BG)
        val db = if (peak <= 0.000001f) -90f else 20f * kotlin.math.log10(peak)
        val t = if (db <= -60f) 0f else faderT(min(db, 12f))
        if (t > 0.004f) {
            paint.color = Color.BLACK
            paint.shader = LinearGradient(0f, r.bottom, 0f, r.top,
                intArrayOf(C_GREEN, C_GREEN, C_YELLOW, C_RED, C_RED), floatArrayOf(0f, 0.5f, 0.6f, 0.67f, 1f), Shader.TileMode.CLAMP)
            c.drawRoundRect(RectF(r.left, r.bottom - r.height() * t, r.right, r.bottom), 3f, 3f, paint)
            paint.shader = null
        }
    }

    private fun faderCap(c: Canvas, cx: Float, cy: Float) {
        val r = RectF(cx - 20f, cy - 11f, cx + 20f, cy + 11f)
        shadow(c, r, 5f, 3f, 110)
        fillV(c, r, 5f, Color.rgb(91, 95, 102), Color.rgb(42, 44, 49))
        line(c, r.left + 4f, r.top + 1f, r.right - 4f, r.top + 1f, Color.rgb(154, 160, 168), 1f)
        fill(c, RectF(r.left + 4f, cy - 1f, r.right - 4f, cy + 1f), 1f, C_ORANGE)
    }

    private fun drawRangeBar(c: Canvas, r: RectF, low: Float, high: Float, minV: Float, maxV: Float) {
        fill(c, r, 6f, C_METER_BG)
        val a = ((low - minV) / (maxV - minV)).coerceIn(0f, 1f)
        val b = ((high - minV) / (maxV - minV)).coerceIn(0f, 1f)
        fill(c, RectF(r.left + r.width() * a, r.top, r.left + r.width() * b, r.bottom), 6f, C_AMBER)
        for (x in floatArrayOf(r.left + r.width() * a, r.left + r.width() * b)) {
            drawKnob(c, x, r.centerY(), 13f, 0.5f, bipolar = false, ring = false)
        }
    }

    private fun drawMiniKeyboard(c: Canvas, r: RectF, low: Int, high: Int) {
        val whites = (0..127).filter { it % 12 in intArrayOf(0, 2, 4, 5, 7, 9, 11) }
        val ww = r.width() / whites.size
        whites.forEachIndexed { k, note ->
            val kr = RectF(r.left + k * ww, r.top, r.left + (k + 1) * ww - 0.6f, r.bottom)
            fill(c, kr, 1f, if (note in low..high) C_AMBER else Color.rgb(226, 228, 232))
        }
        whites.forEachIndexed { k, note ->
            if (note % 12 in intArrayOf(0, 2, 5, 7, 9) && note + 1 <= 127) {
                val bx = r.left + (k + 1) * ww
                val br = RectF(bx - ww * 0.32f, r.top, bx + ww * 0.32f, r.top + r.height() * 0.6f)
                fill(c, br, 1f, if (note + 1 in low..high) C_ORANGE_DARK else Color.rgb(32, 34, 38))
            }
        }
        for (oct in 0..10) {
            val k = whites.indexOf(oct * 12)
            if (k >= 0 && oct % 2 == 0) text(c, "C${oct - 1}", r.left + k * ww + 1f, r.bottom + 13f, 9f, C_KNOB_LABEL, face = faceMono)
        }
    }

    private fun drawGear(c: Canvas, cx: Float, cy: Float, radius: Float) {
        paint.shader = null
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = max(2f, radius * 0.18f)
        paint.color = C_NAV_TEXT
        c.drawCircle(cx, cy, radius * 0.55f, paint)
        c.drawCircle(cx, cy, radius * 0.18f, paint)
        for (i in 0 until 8) {
            val a = Math.PI * 2.0 * i / 8.0
            val x1 = cx + kotlin.math.cos(a).toFloat() * radius * 0.72f
            val y1 = cy + kotlin.math.sin(a).toFloat() * radius * 0.72f
            val x2 = cx + kotlin.math.cos(a).toFloat() * radius
            val y2 = cy + kotlin.math.sin(a).toFloat() * radius
            c.drawLine(x1, y1, x2, y2, paint)
        }
        paint.style = Paint.Style.FILL
    }

    private fun midiNoteName(note: Int): String {
        val n = note.coerceIn(0, 127)
        val names = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        return names[n % 12] + (n / 12 - 1)
    }

    private fun colorWithAlpha(color: Int, alpha: Int): Int = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))

    private fun lighten(color: Int, amount: Int): Int = Color.rgb(
        min(255, Color.red(color) + amount), min(255, Color.green(color) + amount), min(255, Color.blue(color) + amount))

    private fun signed(v: Int): String = if (v > 0) "+$v" else v.toString()
    private fun dbText(db: Float): String = "%.1f dB".format(db)
    private fun voiceModeName(m: Int) = when (m) { 1 -> "MONO"; 2 -> "LEGATO"; else -> "POLY" }
    private fun effectName(t: Int) = when (t) { 1 -> "DELAY"; 2 -> "CHORUS"; 3 -> "REVERB"; else -> "OFF" }
    private fun panText(p: Float) = when { p < -0.02f -> "L${(-p * 100).toInt()}"; p > 0.02f -> "R${(p * 100).toInt()}"; else -> "C" }
    private fun noteName(key: Int): String {
        val names = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        val k = key.coerceIn(0, 127)
        return names[k % 12] + (k / 12 - 1)
    }

    companion object {
        private const val DESIGN_W = 1340f
        private const val DESIGN_H = 800f
        // brushed metal + side navigation
        private val C_METAL_A = Color.rgb(217, 218, 222); private val C_METAL_B = Color.rgb(222, 223, 227)
        private val C_SIDE_A = Color.rgb(38, 40, 44); private val C_SIDE_B = Color.rgb(51, 54, 59)
        private val C_NAV_EDGE = Color.rgb(72, 76, 82); private val C_NAV_TEXT = Color.rgb(158, 163, 171)
        private val C_LOGO = Color.rgb(233, 196, 106)
        // LCD
        private val C_LCD_TOP = Color.rgb(13, 15, 13); private val C_LCD_BOT = Color.rgb(23, 26, 22)
        private val C_LCD = Color.rgb(255, 179, 71); private val C_LCD_DIM = Color.rgb(184, 132, 60)
        private val C_LCD_VALUE = Color.rgb(255, 207, 138); private val C_LCD_OK = Color.rgb(123, 216, 143); private val C_LCD_BAD = Color.rgb(255, 107, 94)
        // light parts
        private val C_KEY_LIGHT = Color.rgb(236, 238, 241); private val C_KEY_LIGHT_EDGE = Color.rgb(185, 188, 194); private val C_KEY_TEXT = Color.rgb(48, 52, 58)
        private val C_STRIP = Color.rgb(236, 238, 241); private val C_STRIP_SEL = Color.WHITE; private val C_EMPTY_CARD = Color.rgb(226, 228, 232)
        private val C_INK = Color.rgb(35, 38, 43); private val C_INK_SOFT = Color.rgb(48, 52, 58); private val C_LABEL_GREY = Color.rgb(123, 128, 138)
        private val C_SCALE = Color.rgb(138, 143, 151); private val C_EMPTY_TEXT = Color.rgb(160, 165, 172)
        private val C_TOGGLE = Color.rgb(223, 226, 230); private val C_TOGGLE_TEXT = Color.rgb(91, 97, 107); private val C_DARK_KEY = Color.rgb(43, 46, 51)
        private val C_TRACK_LIGHT = Color.rgb(201, 204, 209); private val C_METER_BG = Color.rgb(27, 29, 32)
        private val C_ORANGE = Color.rgb(255, 122, 26); private val C_ORANGE_DARK = Color.rgb(194, 90, 15); private val C_AMBER = Color.rgb(255, 179, 71)
        private val C_SOLO = Color.rgb(255, 215, 106); private val C_MUTE = Color.rgb(255, 107, 94); private val C_FREE = Color.rgb(46, 140, 80)
        private val C_GREEN = Color.rgb(60, 157, 90); private val C_YELLOW = Color.rgb(233, 196, 106); private val C_RED = Color.rgb(255, 107, 61)
        // dark boards
        private val C_BOARD_TOP = Color.rgb(52, 55, 61); private val C_BOARD_BOT = Color.rgb(39, 41, 46); private val C_BOARD_EDGE = Color.rgb(85, 89, 95)
        private val C_SECTION = Color.argb(38, 0, 0, 0); private val C_SECTION_TITLE = Color.rgb(201, 204, 209)
        private val C_KNOB_LABEL = Color.rgb(170, 176, 184); private val C_HINT = Color.rgb(143, 148, 156)
        private val C_TAB = Color.rgb(201, 204, 209); private val C_TAB_TEXT = Color.rgb(87, 92, 101)
        private val C_KNOB_A = Color.rgb(75, 79, 86); private val C_KNOB_B = Color.rgb(42, 44, 49); private val C_KNOB_C = Color.rgb(32, 34, 38)
        private val C_PANIC = Color.rgb(179, 38, 30); private val C_PANIC_TOP = Color.rgb(224, 100, 92)
        private val C_WARN_BG = Color.rgb(179, 38, 30); private val C_WARN_TEXT = Color.WHITE
        private val C_LED_OFF = Color.rgb(85, 85, 85); private val C_LED_OFF_LIGHT = Color.rgb(187, 187, 187)
    }
}
