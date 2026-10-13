package com.silveton.sampler

import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
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

    // SILVETON_LOOK_V2: relief keys everywhere (shadow, gradient face, light edge), glowing EQ curve.
    private enum class Page { LIST, COMBI, EFFECTS, SETTINGS }
    // SILVETON_GUI_V4: the Upper editor edits one sound (basic / L1..L4) or the common Upper settings.
    private enum class UpperTab { TUNE, RANGE, CONTROL, TONE, BOW, FX, PORTA, EQ }   // SILVETON_SYNTH_MENU_OFF_V1: SYNTH and MATRIX pages removed; SILVETON_BOW_V1: BOW = ЛЪК
    private enum class WriteTarget { LIST, BLEND }
    private enum class WriteMode { WRITE, COPY, MOVE }

    private data class Hit(
        val rect: RectF,
        val tap: (Float, Float) -> Unit,
        val reset: (() -> Unit)? = null,
        val drag: ((Float, Float, Boolean) -> Unit)? = null
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val hits = ArrayList<Hit>(256)
    // ---------------------------------------------------------------- SILVETON_GUI_V4 state
    private var editSound = 0                    // 0 basic sound, 1..4 layer, 5 = ОБЩО UPPER
    // SILVETON_UPPER_MENU_V1: which sound every strip shows (0 = ОСНОВЕН, 1..4 = L1..L4) - chosen
    // from the drop-down key above the strip; the strip and the editor below follow it.
    private val stripSound = IntArray(7)
    private var dropUpper = -1                   // strip whose drop-down list is open
    private val upperParamsCache = arrayOfNulls<FloatArray>(7)
    private val layerParamsCache = Array(7) { arrayOfNulls<FloatArray>(4) }
    private val layerNameCache = Array(7) { arrayOfNulls<String>(4) }
    private var fxUnitFocus = 0                  // EFFECTS page: 0..3 send slot, 4..10 INSERT of Upper 1..7
    private val fxParamsCache = arrayOfNulls<FloatArray>(11)
    private var eqBand = 2                       // EQ page: 0 HP, 1..5 band, 6 LP
    private var analyzerOn = true
    private var analyzerAfter = true
    private var analyzerPeak = true
    private var picker: PickerState? = null
    private var analyzerCurve: FloatArray? = null
    private var analyzerHold: FloatArray? = null
    private var analyzerLastMs = 0L

    private class PickerState(val upper: Int, val layer: Int, val previousSlot: Int, val previousName: String) {
        var page = 0
        var search = ""
        var favOnly = false
        var recentOnly = false
        var chosenSlot = previousSlot
    }

    private fun uParams(u: Int): FloatArray = upperParamsCache[u] ?: sampler.upperParams(u).also { upperParamsCache[u] = it }
    private fun lParams(u: Int, l: Int): FloatArray = layerParamsCache[u][l] ?: sampler.layerParams(u, l).also { layerParamsCache[u][l] = it }
    private fun lName(u: Int, l: Int): String = layerNameCache[u][l] ?: sampler.layerName(u, l).also { layerNameCache[u][l] = it }
    private fun fxP(unit: Int): FloatArray = fxParamsCache[unit] ?: sampler.fxParams(unit).also { fxParamsCache[unit] = it }
    private fun dropParamCaches(u: Int) { upperParamsCache[u] = null; for (l in 0 until 4) { layerParamsCache[u][l] = null; layerNameCache[u][l] = null }; for (k in 0 until 5) bowCache[u][k] = null }
    private fun setUParam(u: Int, id: Int, v: Float) {
        val a = uParams(u).copyOf(); if (id < a.size) a[id] = v; upperParamsCache[u] = a
        if (!sampler.setUpperParam(u, id, v)) upperParamsCache[u] = null
        if (id in 16..35) refreshUpper(u)   // LOW / MID / HIGH of the old TONE page mirror bands 1 / 3 / 5
    }
    private fun setLParam(u: Int, l: Int, id: Int, v: Float) {
        val a = lParams(u, l).copyOf(); if (id < a.size) a[id] = v; layerParamsCache[u][l] = a
        if (!sampler.setLayerParam(u, l, id, v)) layerParamsCache[u][l] = null
    }
    private fun setFxP(unit: Int, id: Int, v: Float) {
        val a = fxP(unit).copyOf(); if (id < a.size) a[id] = v; fxParamsCache[unit] = a
        if (!sampler.setFxParam(unit, id, v)) fxParamsCache[unit] = null
        if (unit < 4) refreshFx()
    }
    private fun layerOn(u: Int, l: Int) = lParams(u, l).getOrElse(0) { 0f } >= 0.5f
    private fun layerSlot(u: Int, l: Int) = lParams(u, l).getOrElse(31) { 0f }.toInt()
    // TONE extra of a sound (0 = basic, 1..4 = layer): ids 0..14 (see the bridge)
    private fun toneX(u: Int, sound: Int, id: Int): Float = if (sound == 0) uParams(u).getOrElse(id) { 0f } else lParams(u, sound - 1).getOrElse(16 + id) { 0f }
    private fun setToneX(u: Int, sound: Int, id: Int, v: Float) { if (sound == 0) setUParam(u, id, v) else setLParam(u, sound - 1, 16 + id, v) }
    private fun soundLabel(sound: Int) = when (sound) { 0 -> "ОСНОВЕН"; 5 -> "ОБЩО UPPER"; else -> "LAYER $sound" }
    // SILVETON_BOW_V1: slot 9101..9104 = a bowed instrument (physical model), basic sound or layer
    private fun soundSlotOf(u: Int, sound: Int): Int = if (sound == 0) uParams(u).getOrElse(49) { 0f }.toInt() else layerSlot(u, sound - 1)
    private fun isBowSound(u: Int, sound: Int): Boolean = sound in 0..4 && soundSlotOf(u, sound) in 9101..9104
    private val bowCache = Array(7) { arrayOfNulls<FloatArray>(5) }
    private fun bowParams(u: Int, sound: Int): FloatArray = bowCache[u][sound] ?: sampler.soundSynthParams(u, sound).also { bowCache[u][sound] = it }
    private fun bowParam(u: Int, sound: Int, k: Int): Float = bowParams(u, sound).getOrElse(52 + k) { 0f }
    private fun setBowParam(u: Int, sound: Int, k: Int, v: Float) {
        val a = bowParams(u, sound).copyOf(); if (52 + k < a.size) a[52 + k] = v; bowCache[u][sound] = a
        if (!sampler.setSoundSynthParam(u, sound, 52 + k, v)) bowCache[u][sound] = null
        invalidate()
    }
    private fun tabsFor(sound: Int): List<UpperTab> =
        if (sound == 5) listOf(UpperTab.PORTA, UpperTab.EQ, UpperTab.CONTROL, UpperTab.FX)
        else if (isBowSound(selectedUpper, sound)) listOf(UpperTab.TUNE, UpperTab.RANGE, UpperTab.BOW)   // SILVETON_BOW_V1: a bowed instrument has the ЛЪК page instead of TONE
        else listOf(UpperTab.TUNE, UpperTab.RANGE, UpperTab.TONE)   // SILVETON_SYNTH_MENU_OFF_V1

    // SILVETON_GUI_V3: a "-" / "+" pair; both pressed together = reset.
    private class PairHit(val a: RectF, val b: RectF, val reset: () -> Unit)
    private val pairHits = ArrayList<PairHit>(8)
    private var pairConsumed = false
    private var settingsTab = 0          // 0 ОБЩИ, 1 MIDI, 2 INFO
    private var bankInfoCache: SamplerAppController.BankInfo? = null
    private var scanMode = 0             // 0 none, 1 TERCA scan, 2 SCALE scan
    private var copiedUpper: SamplerAppController.UpperState? = null
    private var copiedFrom = -1
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
    // SILVETON_LIST_GRID_V1: LIST position chosen for STORE / EDIT (-1 = none).
    private var listSelected = -1
    // SILVETON_LIST_V2: favourites (★), search by name, swipe for pages.
    private val listFavs: MutableSet<Int> = (sampler.listFavourites()).toMutableSet()
    private var listFavOnly = false
    private var listSearch = ""

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
        applyTheme(sampler.themeDark())   // SILVETON_THEME_V1
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
        if (event.actionMasked == MotionEvent.ACTION_POINTER_DOWN) {
            // SILVETON_GUI_V3: second finger on the partner of a - / + pair = reset.
            val ix = event.actionIndex
            val x2 = event.getX(ix)
            val y2 = event.getY(ix)
            val pair = pairHits.firstOrNull {
                (it.a.contains(downX, downY) && it.b.contains(x2, y2)) || (it.b.contains(downX, downY) && it.a.contains(x2, y2))
            }
            if (pair != null) {
                pair.reset()
                pairConsumed = true
                activeHit = null
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                invalidate()
            }
            return true
        }
        if (pairConsumed && (event.actionMasked == MotionEvent.ACTION_UP || event.actionMasked == MotionEvent.ACTION_CANCEL)) {
            pairConsumed = false
            activeHit = null
            activePointerId = -1
            invalidate()
            return true
        }
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
                // SILVETON_LIST_V2: swipe left / right over the BLENDS = next / previous page
                val pk = picker
                if (page == Page.COMBI && pk != null && (hit == null || hit.drag == null) &&
                    downY in 204f..786f && abs(x - downX) > 140f && abs(y - downY) < 90f) {
                    val pages = max(1, (pickerItems(pk).size + 39) / 40)
                    pk.page = (pk.page + if (x < downX) 1 else -1).coerceIn(0, pages - 1)
                    activeHit = null; activePointerId = -1; dragMoved = false
                    parent?.requestDisallowInterceptTouchEvent(false)
                    postInvalidateOnAnimation()
                    return true
                }
                if (page == Page.LIST && !writeOpen && (hit == null || hit.drag == null) &&
                    downY in 156f..640f && abs(x - downX) > 140f && abs(y - downY) < 90f) {
                    val pages = max(1, (listVisible().size + LIST_PER_PAGE - 1) / LIST_PER_PAGE)
                    listPage = (listPage + if (x < downX) 1 else -1).coerceIn(0, pages - 1)
                    activeHit = null; activePointerId = -1; dragMoved = false
                    parent?.requestDisallowInterceptTouchEvent(false)
                    postInvalidateOnAnimation()
                    return true
                }
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
        // SILVETON_FINE_KNOBS_V1: ten times finer steps than the display grid (whole-number
        // controls with a small range keep their steps), and a slow finger moves 4x finer -
        // the hundredths are reached by hand; the number dialog sets them exactly.
        val fine = if (step >= 1f && maxV - minV <= 48f) step else step / 10f
        var acc = 0f; var prevD = 0f
        hit(r, { _, _ -> exact() }, reset) { x, y, final ->
            if (!dragMoved) return@hit
            val d = (x - downX) - (y - downY)
            val moved = if (abs(d) <= touchSlop) 0f else d - Math.signum(d) * touchSlop
            val delta = moved - prevD; prevD = moved
            acc += delta * (if (abs(delta) <= 2f) 0.25f else 1f)
            val nv = quantizeControl(start + acc / (travel * DRAG_SLOW) * (maxV - minV), minV, maxV, fine)
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
            val nv = quantizeControl(faderDb((startT + moved / (travel * DRAG_SLOW)).coerceIn(0f, 1f)), -60f, 12f, 0.1f)
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
        pairHits.clear()
        smoothRangeHits.clear()
        listLongHits.clear()
        val telemetryNow = android.os.SystemClock.uptimeMillis()
        if (lastTelemetryMs == 0L || telemetryNow - lastTelemetryMs >= 50L) {
            val keyScanWas = sampler.keyScanUpper()
            scanMode = sampler.pollScan()
            if (keyScanWas >= 0 && sampler.keyScanUpper() < 0) refreshUpper(keyScanWas)
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
        if (darkTheme) {   // SILVETON_THEME_V1: deep panel, lighter towards the middle
            paint.shader = RadialGradient(DESIGN_W * 0.55f, DESIGN_H * 0.4f, DESIGN_W * 0.9f, Color.rgb(42, 44, 50), C_METAL_A, Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, DESIGN_W, DESIGN_H, paint)
            paint.shader = null
            return
        }
        paint.shader = LinearGradient(0f, 0f, 4f, 0f, intArrayOf(C_METAL_A, C_METAL_A, C_METAL_B, C_METAL_B),
            floatArrayOf(0f, 0.5f, 0.5f, 1f), Shader.TileMode.REPEAT)
        c.drawRect(0f, 0f, DESIGN_W, DESIGN_H, paint)
        paint.shader = null
    }

    // SILVETON_THEME_V1: the wooden side of the dark theme (walnut), drawn once into a bitmap.
    private var woodBitmap: Bitmap? = null
    private fun wood(): Bitmap {
        woodBitmap?.let { return it }
        val bw = 96; val bh = 800
        val bmp = Bitmap.createBitmap(bw, bh, Bitmap.Config.ARGB_8888)
        val cv = Canvas(bmp)
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        p.shader = LinearGradient(0f, 0f, bw.toFloat(), 0f, intArrayOf(Color.rgb(78, 48, 28), Color.rgb(118, 74, 42), Color.rgb(96, 58, 34), Color.rgb(70, 42, 24)),
            floatArrayOf(0f, 0.35f, 0.7f, 1f), Shader.TileMode.CLAMP)
        cv.drawRect(0f, 0f, bw.toFloat(), bh.toFloat(), p)
        p.shader = null
        p.style = Paint.Style.STROKE
        val rnd = java.util.Random(7)
        for (k in 0 until 70) {   // grain: wavy vertical lines, darker and lighter
            val x0 = rnd.nextFloat() * bw
            val amp = 2f + rnd.nextFloat() * 6f
            val per = 120f + rnd.nextFloat() * 260f
            val ph = rnd.nextFloat() * 6.28f
            val dark = rnd.nextBoolean()
            p.color = if (dark) Color.argb(28 + rnd.nextInt(30), 20, 10, 4) else Color.argb(18 + rnd.nextInt(20), 220, 170, 110)
            p.strokeWidth = 0.6f + rnd.nextFloat() * 1.6f
            val path = Path()
            path.moveTo(x0 + amp * kotlin.math.sin(ph), 0f)
            var y = 0f
            while (y < bh) { y += 8f; path.lineTo(x0 + amp * kotlin.math.sin(ph + y / per * 6.28f), y) }
            cv.drawPath(path, p)
        }
        woodBitmap = bmp
        return bmp
    }

    private fun drawSideNav(c: Canvas) {
        paint.style = Paint.Style.FILL
        paint.color = Color.BLACK
        if (darkTheme) {   // SILVETON_THEME_V1: walnut side like a real synthesizer
            paint.shader = null
            c.drawBitmap(wood(), null, RectF(0f, 0f, 96f, DESIGN_H), paint)
            line(c, 96f, 0f, 96f, DESIGN_H, Color.rgb(10, 8, 6), 3f)
            line(c, 98f, 0f, 98f, DESIGN_H, Color.rgb(120, 124, 134), 1f)
        } else {
            paint.shader = LinearGradient(0f, 0f, 96f, 0f, C_SIDE_A, C_SIDE_B, Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, 96f, DESIGN_H, paint)
        }
        paint.shader = LinearGradient(96f, 0f, 104f, 0f, Color.argb(80, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawRect(96f, 0f, 104f, DESIGN_H, paint)
        paint.shader = null

        c.save()
        c.rotate(90f, 48f, 66f)   // SILVETON_GUI_V3: reads top to bottom
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
            text(c, label, b.centerX(), b.centerY() + 22f, 11f, if (page == p) C_NAV_ON_TEXT else C_NAV_TEXT,
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
        val isList = page == Page.LIST
        val big = if (isList) RectF(112f, 14f, 560f, 96f) else RectF(112f, 14f, 672f, 96f)
        lcd(c, big)
        val slotText = editingCombiSlot?.let { "%03d".format(it) } ?: "---"
        val activeItem = realListItem(cachedActiveListEntry)
        val line1 = when (page) {
            Page.COMBI -> "КОМБИ $slotText" + if (loadedFromList >= 0) " · ЛИСТ %03d".format(loadedFromList + 1) else ""
            Page.LIST -> when {
                listSearch.isNotEmpty() -> "ЛИСТ · ТЪРСЕНЕ: ${listSearch.uppercase()}"
                listFavOnly -> "ЛИСТ · ЛЮБИМИ · ${listFavs.size}"
                else -> "ЛИСТ · СТРАНИЦА ${listPage + 1} · ${cachedListItems.count { !sampler.isEmptyListItem(it) }} / $LIST_SIZE"
            }
            Page.EFFECTS -> "ЕФЕКТИ · КОМБИ $slotText"
            Page.SETTINGS -> "НАСТРОЙКИ · ГЛОБАЛНИ"
        }
        val line2 = if (page == Page.LIST && activeItem != null) "%03d %s".format(cachedActiveListEntry + 1, activeItem.name) else cachedCombiName
        val flash = flashText?.takeIf { now < flashUntilMs }
        text(c, flash ?: line1, big.left + 18f, big.top + 22f, 12f, if (flash != null) C_LCD_OK else C_LCD_DIM,
            face = faceMono, spacing = 0.25f, maxWidth = big.width() - 36f)
        text(c, line2, big.left + 18f, big.top + 60f, 32f, C_LCD, face = faceMono, maxWidth = big.width() - 36f)

        // Status LCD.
        val st = if (isList) RectF(572f, 14f, 1000f, 96f) else RectF(684f, 14f, 1206f, 96f)
        if (isList) {
            // SILVETON_LIST_V2: SAVE AS / LOAD / ТЪРСИ / ★ live up here on the LIST page.
            val saveR = RectF(1012f, 14f, 1108f, 52f); lightKey(c, saveR, "SAVE AS", size = 11f); hit(saveR, { _, _ -> saveSetAs() })
            val loadR = RectF(1012f, 58f, 1108f, 96f); lightKey(c, loadR, "LOAD", size = 11f); hit(loadR, { _, _ -> loadSet() })
            val findR = RectF(1120f, 14f, 1206f, 52f)
            stripToggle(c, findR, "ТЪРСИ", listSearch.isNotEmpty(), C_AMBER, size = 11f)
            hit(findR, { _, _ ->
                if (listSearch.isNotEmpty()) { listSearch = ""; listPage = 0; invalidate() }
                else showTextPage("ТЪРСИ БЛЕНДА ПО ИМЕ", "") { q -> listSearch = q.trim(); listFavOnly = false; listPage = 0; invalidate() }
            })
            val favR = RectF(1120f, 58f, 1206f, 96f)
            stripToggle(c, favR, "★", listFavOnly, C_ORANGE, size = 18f)
            hit(favR, { _, _ -> listFavOnly = !listFavOnly; listSearch = ""; listPage = 0; invalidate() })
        }
        if (isList) {
            lcd(c, st)
            val colW = (st.width() - 32f) / 4f
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
            cell(1, 1, "ЛАТЕНЦИЯ", if (a.estimatedLatencyMs >= 0.0) "%.2f ms".format(a.estimatedLatencyMs) else "—", C_LCD_VALUE)
            cell(2, 1, "CPU", "${round(a.cpuLoadPercent).toInt()} %", if (a.cpuLoadPercent > 95f) C_LCD_BAD else if (a.cpuLoadPercent > 80f) C_LCD else C_LCD_VALUE)
        } else {
            // SILVETON_STATUS_V2: ЗВУК · MASTER · PORTA boxes
            val zv = RectF(684f, 14f, 926f, 96f)
            val ms = RectF(934f, 14f, 1066f, 96f)
            val po = RectF(1074f, 14f, 1206f, 96f)
            lcd(c, zv); lcd(c, ms); lcd(c, po)
            led(c, zv.left + 18f, zv.top + 18f, audioOk, 6f)
            if (!audioOk && running) led(c, zv.left + 18f, zv.top + 18f, true, 6f)
            text(c, if (audioOk) "ЗВУК OK" else if (running) "ЗВУК $audioValue" else "ЗВУК $audioValue", zv.left + 32f, zv.top + 23f, 11f, if (audioOk) C_LCD_OK else C_LCD_BAD, face = faceMono, spacing = 0.2f, maxWidth = 120f)
            text(c, if (a.estimatedLatencyMs >= 0.0) "%.2f ms".format(a.estimatedLatencyMs) else "—", zv.left + 14f, zv.top + 66f, 30f, C_LCD, face = faceMono)
            val cpu = a.cpuLoadPercent
            val cpuCol = if (cpu > 95f) C_LCD_BAD else if (cpu > 80f) C_LCD else C_LCD_OK
            text(c, "XRUN ${a.xrunCount}", zv.left + 150f, zv.top + 23f, 11f, if (a.xrunCount > 0) C_LCD_BAD else C_LCD_DIM, face = faceMono)
            text(c, "MIDI $midiValue", zv.left + 150f, zv.top + 41f, 11f, if (midiRx) C_LCD else C_LCD_DIM, face = faceMono, maxWidth = 86f)
            text(c, "CPU", zv.left + 150f, zv.top + 61f, 11f, C_LCD_DIM, face = faceMono)
            text(c, "${round(cpu).toInt()} %", zv.left + 186f, zv.top + 61f, 13f, cpuCol, face = faceMonoBold)
            // voices bar 0..128
            val vb = RectF(zv.left + 150f, zv.top + 68f, zv.right - 10f, zv.top + 74f)
            fill(c, vb, 3f, Color.rgb(40, 42, 40))
            fill(c, RectF(vb.left, vb.top, vb.left + vb.width() * (a.activeVoices / 128f).coerceIn(0f, 1f), vb.bottom), 3f, if (a.activeVoices > 110) C_LCD_BAD else C_LCD_OK)
            text(c, "${a.activeVoices} ГЛ.", zv.left + 150f, zv.bottom - 3f, 9f, C_LCD_DIM, face = faceMono)
            hit(zv, { _, _ -> page = Page.SETTINGS; settingsTab = 0 })
            // MASTER
            val g = globalState ?: sampler.globalUiState()?.also { globalState = it }
            val master = g?.masterGainDb ?: -6f
            text(c, "MASTER", ms.left + 12f, ms.top + 22f, 11f, C_LCD_DIM, face = faceMono, spacing = 0.2f)
            text(c, "%.2f dB".format(master), ms.left + 12f, ms.top + 56f, 24f, if (master > 0f) C_LCD_BAD else C_LCD, face = faceMono)
            val pk = max(frameUpperMeters.maxOfOrNull { max(it.first, it.second) } ?: 0f, 0f)
            val mb = RectF(ms.left + 12f, ms.bottom - 16f, ms.right - 12f, ms.bottom - 10f)
            fill(c, mb, 3f, Color.rgb(40, 42, 40))
            val mt = ((20f * kotlin.math.log10(max(pk, 1e-5f)) + 60f) / 60f).coerceIn(0f, 1f)
            fill(c, RectF(mb.left, mb.top, mb.left + mb.width() * mt, mb.bottom), 3f, if (mt > 0.9f) C_LCD_BAD else C_LCD_OK)
            hit(ms, { _, _ -> if (g != null) showNumber("MASTER dB", g.masterGainDb, -60f, 18f, 1) { commitGlobal(g.copy(masterGainDb = it)) } },
                { if (g != null) commitGlobal(g.copy(masterGainDb = -6f)) })
            // PORTA of the selected Upper
            val up = uParams(selectedUpper)
            val kronos = up.getOrElse(42) { 0f } >= 0.5f
            val porta = upperStates[selectedUpper]?.portamentoSec ?: 0f
            text(c, "PORTA U${selectedUpper + 1} · ${if (kronos) "KRONOS" else "SILVETON"}", po.left + 10f, po.top + 22f, 10f, C_LCD_DIM, face = faceMono, maxWidth = po.width() - 16f)
            val tv = if (kronos) { val t = up.getOrElse(45) { 0f }.toInt(); if (t <= 0) "OFF" else "TIME $t" } else if (porta <= 0f) "OFF" else "${round(porta * 1000f).toInt()} ms"
            text(c, tv, po.left + 10f, po.top + 52f, 20f, C_LCD, face = faceMono, maxWidth = po.width() - 16f)
            text(c, if (kronos) (if (up.getOrElse(43) { 0f } >= 0.5f) "FINGERED" else "ВИНАГИ") + " · " + (if (up.getOrElse(44) { 0f } >= 0.5f) "RATE" else "TIME")
                     else "СЪСЕДНИ ${sampler.portaNear()} · BAL ${sampler.portaBalance()}", po.left + 10f, po.top + 74f, 10f, C_LCD_VALUE, face = faceMono, maxWidth = po.width() - 16f)
            hit(po, { _, _ -> page = Page.COMBI; editSound = 5; tab = UpperTab.PORTA })
        }

        // SILVETON_LIST_GRID_V1: BLENDS / WRITE / + LIST are gone - a COMBI is stored
        // from the LIST page (STORE), and changed with EDIT.

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
        val pk = picker
        if (pk != null) { drawPicker(c, pk); return }
        if (editSound == 5 && tab == UpperTab.EQ) { drawEqPage(c); return }
        val meters = frameUpperMeters
        val stripW = (1214f - 6f * 8f) / 7f
        for (i in 0 until 7) {
            val state = upperStates[i] ?: sampler.upperState(i)?.also { upperStates[i] = it } ?: continue
            val left = 112f + i * (stripW + 8f)
            drawUpperStrip(c, RectF(left, 108f, left + stripW, 468f), i, state, meters.getOrElse(i) { 0f to 0f })
        }
        drawUpperEditor(c)
        if (dropUpper in 0..6) drawUpperMenu(c, dropUpper, 112f + dropUpper * (stripW + 8f), stripW)
    }

    // SILVETON_UPPER_MENU_V1: the drop-down list of one Upper - ОСНОВЕН and L1..L4 with their sounds.
    private fun drawUpperMenu(c: Canvas, i: Int, left: Float, w: Float) {
        hit(RectF(0f, 0f, 1340f, 800f), { _, _ -> dropUpper = -1; invalidate() })   // anywhere else closes
        val r = RectF(left + 4f, 140f, left + w - 4f, 140f + 6f + 5 * 38f + 6f)
        shadow(c, r, 10f, 3f, 90)
        fill(c, r, 10f, Color.WHITE)
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 2f; paint.color = Color.rgb(90, 94, 100); paint.shader = null
        c.drawRoundRect(r, 10f, 10f, paint); paint.style = Paint.Style.FILL
        val s = upperStates[i]
        for (k in 0..4) {
            val row = RectF(r.left + 4f, r.top + 6f + k * 38f, r.right - 4f, r.top + 6f + k * 38f + 34f)
            val on = stripSound[i] == k
            val label = if (k == 0) "ОСНОВЕН" else "L$k"
            val has = if (k == 0) !(s == null || s.soundName == "EMPTY" || s.soundName.isBlank()) else layerSlot(i, k - 1) > 0
            val name = when {
                k == 0 -> if (has) s!!.soundName else "— празен —"
                has -> sampler.layerName(i, k - 1).ifBlank { "L$k" }
                else -> "— празен —"
            }
            if (on) fill(c, row, 6f, C_BOARD_TOP)
            text(c, label, row.left + 8f, row.top + 22f, 11f, if (on) C_AMBER else Color.rgb(60, 66, 74), face = faceBold)
            text(c, name, row.right - 6f, row.top + 22f, 9f, if (on) C_LCD_DIM else (if (has) Color.rgb(90, 95, 105) else Color.rgb(150, 155, 162)),
                right = true, maxWidth = row.width() - 70f)
            hit(row, { _, _ ->
                stripSound[i] = k
                selectedUpper = i
                if (editSound != 5) { editSound = k; if (tab !in tabsFor(k)) tab = tabsFor(k).first() }
                dropUpper = -1
                if (!has) openPicker(i, k - 1)   // an empty place: choose its sound straight away
                invalidate()
            })
        }
    }

    private fun drawUpperStrip(c: Canvas, r: RectF, i: Int, s: SamplerAppController.UpperState, meter: Pair<Float, Float>) {
        val selected = i == selectedUpper
        // SILVETON_UPPER_MENU_V1: the strip shows ОСНОВЕН or the layer chosen from its drop-down key
        val ls = stripSound[i].coerceIn(0, 4)
        val layerView = ls > 0
        val lHas = layerView && layerSlot(i, ls - 1) > 0
        val lp = if (layerView) lParams(i, ls - 1) else FloatArray(0)
        val empty = if (layerView) !lHas else (s.soundName == "EMPTY" || s.soundName.isBlank())
        val live = if (layerView) (lHas && layerOn(i, ls - 1) && s.enabled) else (s.enabled && !empty)
        val shownName = if (layerView) (if (lHas) sampler.layerName(i, ls - 1).ifBlank { "L$ls" } else "EMPTY") else s.soundName
        val shownPan = if (layerView) lp.getOrElse(2) { 0f } else s.pan
        val shownLevel = if (layerView) lp.getOrElse(1) { 0f } else s.levelDb
        val shownLow = if (layerView) lp.getOrElse(6) { 0f }.toInt() else s.keyLow
        val shownHigh = if (layerView) lp.getOrElse(7) { 127f }.toInt() else s.keyHigh
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
        line(c, L + 12f, T + 1f, r.right - 12f, T + 1f, if (darkTheme) Color.rgb(72, 76, 86) else Color.WHITE, 1f)

        // SILVETON_UPPER_MENU_V1: drop-down key "UPPER n ▾" / "UPPER n · L2 ▾"
        val menuR = RectF(L + 6f, T + 5f, r.right - 24f, T + 29f)
        stripToggle(c, menuR, if (layerView) "UPPER ${i + 1} · L$ls ▾" else "UPPER ${i + 1} ▾", layerView, C_AMBER, size = 10f)
        hit(menuR, { _, _ -> selectedUpper = i; dropUpper = if (dropUpper == i) -1 else i; invalidate() })
        led(c, r.right - 13f, T + 17f, live, 5f, light = true)
        text(c, if (empty) "EMPTY" else shownName, L + 12f, T + 46f, 15f, if (empty) C_EMPTY_TEXT else C_INK,
            face = faceBold, maxWidth = W - 24f)
        hit(RectF(L, T + 30f, r.right, T + 50f), { _, _ -> selectedUpper = i; openPicker(i, ls - 1) })

        // ON / SOLO / MUTE - big keys (SILVETON_GUI_V3)
        val tgW = (W - 16f - 8f) / 3f
        for (k in 0 until 3) {
            val tr = RectF(L + 8f + k * (tgW + 4f), T + 52f, L + 8f + k * (tgW + 4f) + tgW, T + 90f)
            val on = when (k) { 0 -> if (layerView) (lHas && layerOn(i, ls - 1)) else s.enabled; 1 -> s.solo; else -> s.mute }
            val label = when (k) { 0 -> "ON"; 1 -> "SOLO"; else -> "MUTE" }
            val onColor = when (k) { 0 -> C_AMBER; 1 -> C_SOLO; else -> C_MUTE }
            stripToggle(c, tr, label, on, onColor, size = if (k == 0) 15f else 11f)
            hit(tr, { _, _ ->
                val cur = upperStates[i] ?: s
                when (k) {
                    0 -> if (layerView) { if (lHas) setLParam(i, ls - 1, 0, if (layerOn(i, ls - 1)) 0f else 1f) else openPicker(i, ls - 1) }
                         else if (!empty) commitMain(i, cur.copy(enabled = !cur.enabled))
                    1 -> commitMain(i, cur.copy(solo = !cur.solo))
                    else -> commitMain(i, cur.copy(mute = !cur.mute))
                }
                invalidate()
            })
        }

        // SILVETON_LAYERS_V1: L1..L4 keys. Tap = layer on / off (an empty layer opens the
        // sound picker), double tap = sound picker for that layer.
        val lW = (W - 16f - 12f) / 4f
        for (k in 0 until 4) {
            val lr = RectF(L + 8f + k * (lW + 4f), T + 94f, L + 8f + k * (lW + 4f) + lW, T + 122f)
            val has = layerSlot(i, k) > 0
            val on = has && layerOn(i, k)
            stripToggle(c, lr, "L${k + 1}", on, C_AMBER, size = 11f)
            if (has && !on) led(c, lr.right - 7f, lr.top + 7f, false, 2.5f)
            hit(lr, { _, _ ->
                selectedUpper = i
                if (has) setLParam(i, k, 0, if (on) 0f else 1f) else openPicker(i, k)
                invalidate()
            }, { selectedUpper = i; openPicker(i, k) })
        }

        // PAN knob: drag the whole row, long press = number, double tap = centre.
        drawKnob(c, L + 29f, T + 147f, 19f, (shownPan + 1f) * 0.5f, bipolar = true, ring = false)
        text(c, "PAN", L + 58f, T + 142f, 10f, C_LABEL_GREY, spacing = 0.2f)
        text(c, panText(shownPan), L + 58f, T + 160f, 14f, C_INK, face = faceMonoBold)
        val setPan: (Float) -> Unit = { nv -> if (layerView) setLParam(i, ls - 1, 2, nv) else { val cur = upperStates[i] ?: s; commitMain(i, cur.copy(pan = nv)) } }
        knobHit(RectF(L, T + 126f, r.right, T + 170f), shownPan, -1f, 1f, 0.01f, 160f,
            onChange = setPan,
            exact = { showNumber(if (layerView) "UPPER ${i + 1} L$ls PAN" else "UPPER ${i + 1} PAN", shownPan, -1f, 1f, 2) { nv -> setPan(nv) } },
            reset = { setPan(0f) })

        // LEVEL fader with L/R meters.
        val trackL = L + 52f
        val trackR = L + 68f
        val trackTop = T + 178f
        val trackBot = T + 248f
        val travel = trackBot - trackTop
        listOf("+12" to 1f, "0" to 2f / 3f, "-60" to 0f).forEach { (lbl, t) ->
            text(c, lbl, L + 10f, trackBot - t * travel + 3.5f, 9f, C_SCALE, face = faceMono)
        }
        fill(c, RectF(trackL, trackTop, trackR, trackBot), 6f, C_TRACK_LIGHT)
        paint.color = Color.BLACK
        paint.shader = LinearGradient(trackL, 0f, trackR, 0f, Color.argb(70, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawRoundRect(RectF(trackL, trackTop, trackR, trackBot), 6f, 6f, paint)
        paint.shader = null
        stripMeter(c, RectF(L + 92f, trackTop, L + 103f, trackBot), meter.first)
        stripMeter(c, RectF(L + 109f, trackTop, L + 120f, trackBot), meter.second)
        faderCap(c, (trackL + trackR) * 0.5f, trackBot - faderT(shownLevel) * travel, big = true)
        text(c, dbText(shownLevel), r.centerX(), T + 272f, 14f, C_INK, center = true, face = faceMonoBold)
        val setLevel: (Float) -> Unit = { nv -> if (layerView) setLParam(i, ls - 1, 1, nv) else { val cur = upperStates[i] ?: s; commitMain(i, cur.copy(levelDb = nv)) } }
        val exactLevel = {
            showNumber(if (layerView) "UPPER ${i + 1} L$ls LEVEL dB" else "UPPER ${i + 1} LEVEL dB", shownLevel, -60f, 12f, 1) { nv -> setLevel(nv) }
        }
        val resetLevel = { setLevel(0f) }
        faderHit(RectF(L + 26f, T + 172f, r.right - 4f, T + 258f), shownLevel, travel,
            onChange = setLevel, exact = exactLevel, reset = resetLevel)
        hit(RectF(L, T + 258f, r.right, T + 278f), { _, _ -> exactLevel() }, resetLevel)

        // POLY / MONO / LEGATO - three own keys (SILVETON_GUI_V3) + key range.
        val mW = (W - 16f - 8f) / 3f
        for (k in 0 until 3) {
            val mr = RectF(L + 8f + k * (mW + 4f), T + 282f, L + 8f + k * (mW + 4f) + mW, T + 312f)
            stripToggle(c, mr, when (k) { 0 -> "POLY"; 1 -> "MONO"; else -> "LEG" }, s.voiceMode == k, C_AMBER, size = 11f)
            hit(mr, { _, _ -> val cur = upperStates[i] ?: s; commitMain(i, cur.copy(voiceMode = k)) })
        }
        // T = TERCA, Q = quarter tone (SILVETON_TERCA_V1 / SILVETON_SCALE_V1), then the key range.
        val tqW = 40f
        val tR = RectF(L + 8f, T + 316f, L + 8f + tqW, T + 354f)
        val qR = RectF(tR.right + 4f, T + 316f, tR.right + 4f + tqW, T + 354f)
        // SILVETON_TQ_LIST_V1: lit = on; dim = chosen here but switched off on LIST.
        stripToggle(c, tR, "T", sampler.tercaOn(i) || sampler.tercaChosen(i), if (sampler.tercaOn(i)) C_AMBER else C_LCD_DIM, size = 18f)
        stripToggle(c, qR, "Q", sampler.quarterOn(i) || sampler.quarterChosen(i), if (sampler.quarterOn(i)) C_AMBER else C_LCD_DIM, size = 18f)
        hit(tR, { _, _ -> sampler.toggleTerca(i); invalidate() })
        hit(qR, { _, _ -> sampler.toggleQuarter(i); invalidate() })
        val rangeR = RectF(qR.right + 4f, T + 316f, r.right - 8f, T + 354f)
        // SILVETON_KEY_SCAN_V1: tap = SCAN, then press a key: this Upper plays up to it.
        // Tap again = cancel, double tap = whole keyboard.
        if (!layerView && sampler.keyScanUpper() == i) {
            val blink = (android.os.SystemClock.uptimeMillis() / 300L) % 2L == 0L
            stripToggle(c, rangeR, "НАТИСНИ…", true, if (blink) C_ORANGE else C_AMBER, size = 10f)
            postInvalidateDelayed(150L)
        } else {
            lightKey(c, rangeR, "${noteName(shownLow)}–${noteName(shownHigh)}", size = 10f, radius = 6f)
        }
        hit(rangeR, { _, _ ->
            selectedUpper = i
            if (layerView) { editSound = ls; tab = UpperTab.RANGE }   // the layer range is set on its RANGE page
            else if (sampler.keyScanUpper() == i) sampler.cancelKeyScan() else sampler.startKeyScan(i)
            invalidate()
        }, {
            if (layerView) { setLParam(i, ls - 1, 6, 0f); setLParam(i, ls - 1, 7, 127f) }
            else { sampler.cancelKeyScan(); val cur = upperStates[i] ?: s; commitMain(i, cur.copy(keyLow = 0, keyHigh = 127)) }
        })

        if (!live) {
            // Switched-off / empty UPPER: veil it, controls stay usable.
            fill(c, r, 12f, if (darkTheme) Color.argb(120, 30, 32, 37) else Color.argb(92, 220, 221, 225))
        }
    }

    private fun drawUpperEditor(c: Canvas) {
        val s = upperStates[selectedUpper] ?: return
        val u = selectedUpper
        // SILVETON_UPPER_MENU_V1: the edited sound is the one the strip shows (its drop-down key);
        // the first key names it and opens that list, the second is the common settings of the Upper.
        val ss = stripSound[u].coerceIn(0, 4)
        if (editSound != 5 && editSound != ss) { editSound = ss; if (tab !in tabsFor(ss)) tab = tabsFor(ss).first() }
        var x = 112f
        run {
            val rr = RectF(x, 478f, x + 150f, 514f)
            stripToggle(c, rr, if (ss == 0) "UPPER ${u + 1} · ОСНОВЕН" else "UPPER ${u + 1} · L$ss", editSound != 5, C_AMBER, size = 11f)
            hit(rr, { _, _ -> if (editSound == 5) { editSound = ss; if (tab !in tabsFor(ss)) tab = tabsFor(ss).first() } else dropUpper = u; invalidate() })
            x += 158f
            val gr = RectF(x, 478f, x + 118f, 514f)
            stripToggle(c, gr, "ОБЩО U${u + 1}", editSound == 5, C_AMBER, size = 11f)
            hit(gr, { _, _ -> editSound = 5; if (tab !in tabsFor(5)) tab = tabsFor(5).first() })
            x += 126f
        }
        tabsFor(editSound).forEach { t ->
            val name = when (t) { UpperTab.CONTROL -> "CTRL"; UpperTab.BOW -> "ЛЪК"; else -> t.name }
            val tw = textWidth(name, 13f, faceBold, 0.15f) + 40f
            val rr = RectF(x, 478f, x + tw, 514f)
            val on = tab == t
            paint.shader = null
            paint.style = Paint.Style.FILL
            paint.color = if (on) C_BOARD_TOP else C_TAB
            c.drawRoundRect(rr, 8f, 8f, paint)
            c.drawRect(rr.left, rr.centerY(), rr.right, rr.bottom + 1f, paint)
            text(c, name, rr.centerX(), rr.top + 23f, 13f, if (on) C_AMBER else C_TAB_TEXT, center = true, face = faceBold, spacing = 0.15f)
            hit(rr, { _, _ -> tab = t })
            x += tw + 6f
        }
        if (tab !in tabsFor(editSound)) tab = tabsFor(editSound).first()

        // COPY / PASTE all settings of an Upper (portamento .. FX).
        val copyR = RectF(1066f, 478f, 1190f, 512f)
        val pasteR = RectF(1196f, 478f, 1326f, 512f)
        lightKey(c, copyR, "COPY U${selectedUpper + 1}", size = 12f, radius = 7f)
        lightKey(c, pasteR, if (copiedUpper != null) "PASTE U${copiedFrom + 1}" else "PASTE", enabled = copiedUpper != null, size = 12f, radius = 7f)
        hit(copyR, { _, _ -> copiedUpper = upperStates[selectedUpper]; copiedFrom = selectedUpper; flash("КОПИРАН UPPER ${selectedUpper + 1}") })
        hit(pasteR, { _, _ -> pasteUpper(selectedUpper) })

        board(c, RectF(112f, 514f, 1326f, 786f), squareTopLeft = true)
        val sec = RectF(128f, 530f, 1310f, 770f)
        when (tab) {
            UpperTab.TUNE -> if (editSound == 0) drawTune(c, sec, s) else drawLayerTune(c, sec, u, editSound - 1)
            UpperTab.RANGE -> if (editSound == 0) drawRange(c, sec, s) else drawLayerRange(c, sec, u, editSound - 1)
            UpperTab.CONTROL -> drawControl(c, sec, s)
            UpperTab.TONE -> drawToneV2(c, sec, u, editSound)
            UpperTab.BOW -> drawBow(c, sec, u, editSound)   // SILVETON_BOW_V1
            UpperTab.FX -> drawUpperFx(c, sec, s)
            UpperTab.PORTA -> drawPorta(c, sec, s)
            UpperTab.EQ -> {}   // drawn full screen by drawCombi
        }
    }

    // ---------------------------------------------------------------- SILVETON_LAYERS_V1 pages
    private fun drawLayerTune(c: Canvas, sec: RectF, u: Int, l: Int) {
        section(c, sec, "LAYER ${l + 1} · ЗВУК, НИВО, ВИСОЧИНА")
        val p = lParams(u, l)
        val slot = layerSlot(u, l)
        val name = if (slot > 0) "%03d %s".format(slot, lName(u, l)) else "— НЯМА ЗВУК —"
        val nameR = RectF(sec.left + 16f, sec.top + 40f, sec.left + 420f, sec.top + 84f)
        lcd(c, nameR, 8f)
        text(c, name, nameR.left + 14f, nameR.top + 30f, 18f, if (slot > 0) C_LCD else C_LCD_DIM, face = faceMono, maxWidth = nameR.width() - 28f)
        hit(nameR, { _, _ -> openPicker(u, l) })
        val onR = RectF(sec.left + 432f, sec.top + 40f, sec.left + 540f, sec.top + 84f)
        stripToggle(c, onR, "LAYER ON", slot > 0 && layerOn(u, l), C_LCD_OK, size = 12f)
        hit(onR, { _, _ -> if (slot > 0) setLParam(u, l, 0, if (layerOn(u, l)) 0f else 1f) else openPicker(u, l) })
        val offR = RectF(sec.left + 552f, sec.top + 40f, sec.left + 660f, sec.top + 84f)
        lightKey(c, offR, "МАХНИ", enabled = slot > 0, size = 12f)
        hit(offR, { _, _ -> if (slot > 0) { sampler.setLayerSound(u, l, 0); dropParamCaches(u); invalidate() } })
        val cy = 672f
        val items = listOf(
            Triple("LEVEL", 1, Triple(-60f, 12f, 0.1f)), Triple("PAN", 2, Triple(-1f, 1f, 0.01f)),
            Triple("OCTAVE", 4, Triple(-3f, 3f, 1f)), Triple("TRANSPOSE", 3, Triple(-12f, 12f, 1f)), Triple("FINE", 5, Triple(-100f, 100f, 1f)))
        items.forEachIndexed { k, (label, id, rng) ->
            val v = p.getOrElse(id) { 0f }
            val cx = sec.left + 115f + k * 230f
            val shown = when (id) { 1 -> dbText(v); 2 -> panText(v); 5 -> "${round(v).toInt()} c"; else -> signed(round(v).toInt()) }
            knobControl(c, cx, cy, 30f, v, rng.first, rng.second, rng.third, 260f, id != 1, shown, label, hitW = 200f,
                onChange = { nv -> setLParam(u, l, id, if (rng.third >= 1f) round(nv) else nv) },
                exact = { showNumber("LAYER ${l + 1} $label", v, rng.first, rng.second, if (rng.third >= 1f) 0 else 2) { setLParam(u, l, id, it) } },
                reset = { setLParam(u, l, id, 0f) })
        }
    }

    private fun drawLayerRange(c: Canvas, sec: RectF, u: Int, l: Int) {
        val p = lParams(u, l)
        val vel = RectF(sec.left, sec.top, sec.left + 700f, sec.bottom)
        section(c, vel, "LAYER ${l + 1} · VELOCITY RANGE")
        val lo = p.getOrElse(8) { 1f }; val hi = p.getOrElse(9) { 127f }
        val bar = RectF(vel.left + 132f, 640f, vel.right - 132f, 652f)
        drawRangeBar(c, bar, lo, hi, 1f, 127f)
        registerSmoothRange(RectF(bar.left, bar.top - 26f, bar.right, bar.bottom + 26f), 1f, 127f, 1f, lo, hi) { a, b ->
            setLParam(u, l, 8, round(a)); setLParam(u, l, 9, round(b))
        }
        text(c, "МИН", vel.left + 66f, 616f, 10f, C_KNOB_LABEL, center = true, spacing = 0.2f)
        text(c, lo.toInt().toString(), vel.left + 66f, 656f, 28f, C_AMBER, center = true, face = faceMono)
        text(c, "МАКС", vel.right - 66f, 616f, 10f, C_KNOB_LABEL, center = true, spacing = 0.2f)
        text(c, hi.toInt().toString(), vel.right - 66f, 656f, 28f, C_AMBER, center = true, face = faceMono)
        hit(RectF(vel.left, 596f, bar.left - 4f, 700f), { _, _ -> showNumber("LAYER ${l + 1} VEL МИН", lo, 1f, 127f, 0) { setLParam(u, l, 8, round(it)) } }, { setLParam(u, l, 8, 1f); setLParam(u, l, 9, 127f) })
        hit(RectF(bar.right + 4f, 596f, vel.right, 700f), { _, _ -> showNumber("LAYER ${l + 1} VEL МАКС", hi, 1f, 127f, 0) { setLParam(u, l, 9, round(it)) } }, { setLParam(u, l, 8, 1f); setLParam(u, l, 9, 127f) })
        val key = RectF(vel.right + 12f, sec.top, sec.right, sec.bottom)
        section(c, key, "KEY RANGE · ОБХВАТ")
        val kl = p.getOrElse(6) { 0f }.toInt(); val kh = p.getOrElse(7) { 127f }.toInt()
        text(c, "${noteName(kl)}  –  ${noteName(kh)}", key.left + 20f, 614f, 30f, C_AMBER, face = faceMono, maxWidth = key.width() - 40f)
        drawMiniKeyboard(c, RectF(key.left + 20f, 636f, key.right - 20f, 700f), kl, kh)
        text(c, "Горе ляво = от, горе дясно = до · двоен допир = цялата", key.left + 16f, key.bottom - 12f, 11f, C_HINT, maxWidth = key.width() - 32f)
        hit(key, { _, _ -> showNumber("LAYER ${l + 1} КЛАВИШ ОТ", kl.toFloat(), 0f, 127f, 0) { setLParam(u, l, 6, round(it)) } },
            { setLParam(u, l, 6, 0f); setLParam(u, l, 7, 127f) },
            null)
        hit(RectF(key.left, key.top, key.centerX(), key.top + 40f), { _, _ -> showNumber("LAYER ${l + 1} КЛАВИШ ОТ", kl.toFloat(), 0f, 127f, 0) { setLParam(u, l, 6, round(it)) } })
        hit(RectF(key.centerX(), key.top, key.right, key.top + 40f), { _, _ -> showNumber("LAYER ${l + 1} КЛАВИШ ДО", kh.toFloat(), 0f, 127f, 0) { setLParam(u, l, 7, round(it)) } })
    }

    // ---------------------------------------------------------------- SILVETON_TONE_V2 page
    private val kStartQuick = floatArrayOf(0f, 10f, 25f, 60f, 120f)

    // ---------------------------------------------------------------- SILVETON_BOW_V1: the ЛЪК page of a bowed instrument
    private fun drawBow(c: Canvas, sec: RectF, u: Int, sound: Int) {
        val unit = sec.width() / 8f
        fun cx(k: Int) = sec.left + unit * (k + 0.5f)
        val r1 = 628f
        val rad = 26f
        val hw = unit - 8f
        secTitle(c, sec, "ЛЪК · ${soundLabel(sound)} · ${if (sound == 0) (upperStates[u]?.soundName ?: "") else lName(u, sound - 1)}")
        // k: 0 pressure 1 position 2 brightness 3 vibrato depth 4 vibrato speed 5 vibrato delay 6 attack 7 release
        data class K(val label: String, val id: Int, val minV: Float, val maxV: Float, val step: Float, val dec: Int, val def: Float, val text: (Float) -> String)
        val knobs = listOf(
            K("НАТИСК", 0, 0f, 100f, 1f, 0, 50f) { "%.0f %%".format(it) },
            K("ПОЗИЦИЯ", 1, 5f, 18f, 0.1f, 1, 12f) { v -> when { v <= 7f -> "ПОНТИЧЕЛО"; v >= 16f -> "ТАСТО"; else -> "%.1f".format(v) } },
            K("ЯРКОСТ", 2, 0f, 100f, 1f, 0, 50f) { "%.0f %%".format(it) },
            K("ВИБРАТО", 3, 0f, 100f, 1f, 0, 20f) { if (it <= 0f) "OFF" else "%.0f c".format(it) },
            K("ВИБ. СКОРОСТ", 4, 3f, 9f, 0.1f, 1, 5.5f) { "%.1f Hz".format(it) },
            K("ВИБ. ЗАКЪСН.", 5, 0f, 2000f, 10f, 0, 250f) { if (it < 1000f) "%.0f ms".format(it) else "%.2f s".format(it / 1000f) },
            K("АТАКА", 6, 5f, 200f, 1f, 0, 20f) { "%.0f ms".format(it) },
            K("РЕЛИЙЗ", 7, 10f, 500f, 1f, 0, 60f) { "%.0f ms".format(it) })
        knobs.forEachIndexed { k, kn ->
            // pressure, brightness: stored 0..1, shown in %; position stored 0.05..0.18, shown x100
            val raw = bowParam(u, sound, kn.id)
            val shown = when (kn.id) { 0, 2 -> raw * 100f; 1 -> raw * 100f; else -> raw }
            fun store(v: Float) = when (kn.id) { 0, 2, 1 -> v / 100f; else -> v }
            knobControl(c, cx(k), r1, rad, shown, kn.minV, kn.maxV, kn.step, 260f, false, kn.text(shown), kn.label, hitW = hw,
                onChange = { nv -> setBowParam(u, sound, kn.id, store(nv)) },
                exact = { showNumber(kn.label, shown, kn.minV, kn.maxV, kn.dec) { setBowParam(u, sound, kn.id, store(it)) } },
                reset = { setBowParam(u, sound, kn.id, store(kn.def)) })
        }
        text(c, "експресия (педал CC11) = сила и тембър на лъка · modulation = още вибрато · легато = плъзгане без нова атака (PORTA решава времето)", sec.left + 6f, sec.bottom - 6f, 10f, C_HINT, maxWidth = sec.width() - 12f)
    }

    private fun drawToneV2(c: Canvas, sec: RectF, u: Int, sound: Int) {
        val s = upperStates[u] ?: return
        val unit = sec.width() / 12f
        fun cx(k: Int) = sec.left + unit * (k + 0.5f)
        val r1 = 572f
        val r2 = 684f
        val rad = 20f
        val hw = unit - 4f
        // the six legacy values: basic sound = the Upper's TONE, a layer = its own
        fun legacy(k: Int): Float = if (sound == 0) toneValue(s, TonePar.entries[k]) else {
            val v = lParams(u, sound - 1).getOrElse(10 + k) { -1f }
            if (v >= 0f) v else when (k) { 0 -> 5f; 1 -> 80f; 2 -> 1f; 3 -> 120f; 4 -> 20000f; else -> 0f }
        }
        fun setLegacy(k: Int, v: Float) { if (sound == 0) setTone(TonePar.entries[k], v) else setLParam(u, sound - 1, 10 + k, v) }
        secTitle(c, sec, "TONE · ${soundLabel(sound)}")
        // row 1: ATTACK DECAY SUSTAIN RELEASE | START END | CUTOFF RESON F.ENV F.TIME | HI-PASS | MOD WHEEL
        val envNames = arrayOf("ATTACK", "DECAY", "SUSTAIN", "RELEASE", "CUTOFF", "RESON")
        val envCols = intArrayOf(0, 1, 2, 3, 6, 7)
        for (k in 0 until 6) {
            val p = TonePar.entries[k]
            val v = legacy(k)
            val range = toneRange(p)
            val pct = k == 2 || k == 5
            knobControl(c, cx(envCols[k]), r1, rad, toneToNorm(p, v), 0f, 1f, 0.002f, 260f, false, toneText(p, v), envNames[k], hitW = hw,
                onChange = { nv -> setLegacy(k, toneFromNorm(p, nv)) },
                exact = { showNumber(envNames[k], if (pct) v * 100f else v, range.first, range.second, range.third, restore = { setLegacy(k, v) }) { nv -> setLegacy(k, if (pct) nv / 100f else nv) } },
                reset = { setLegacy(k, -1f) })
        }
        // START / END (ms into the sample)
        fun msText(v: Float) = if (v <= 0f) "OFF" else if (v < 1000f) "%.2f ms".format(v) else "%.2f s".format(v / 1000f)
        listOf(0 to "START", 1 to "END").forEachIndexed { k, (id, label) ->
            val v = toneX(u, sound, id)
            val maxV = if (id == 0) 2000f else 60000f
            val norm = kotlin.math.ln(v.coerceIn(0f, maxV) + 1f) / kotlin.math.ln(maxV + 1f)
            knobControl(c, cx(4 + k), r1, rad, norm, 0f, 1f, 0.002f, 300f, false, msText(v), label, hitW = hw,
                onChange = { nv -> setToneX(u, sound, id, round(kotlin.math.exp(nv.coerceIn(0f, 1f) * kotlin.math.ln(maxV + 1f)) - 1f)) },
                exact = { showNumber("SAMPLE $label ms", v, 0f, maxV, 0) { setToneX(u, sound, id, it) } },
                reset = { setToneX(u, sound, id, 0f) })
        }
        // FILTER ENV amount (octaves) + time
        run {
            val v = toneX(u, sound, 2)
            knobControl(c, cx(8), r1, rad, v, -8f, 8f, 0.05f, 280f, true, "%.2f oct".format(v), "F.ENV", hitW = hw,
                onChange = { nv -> setToneX(u, sound, 2, nv) }, exact = { showNumber("FILTER ENV (октави)", v, -8f, 8f, 2) { setToneX(u, sound, 2, it) } }, reset = { setToneX(u, sound, 2, 0f) })
            val t = toneX(u, sound, 3)
            val norm = kotlin.math.ln(t.coerceIn(1f, 20000f)) / kotlin.math.ln(20000f)
            knobControl(c, cx(9), r1, rad, norm, 0f, 1f, 0.002f, 260f, false, msText(t), "F.TIME", hitW = hw,
                onChange = { nv -> setToneX(u, sound, 3, round(kotlin.math.exp(nv.coerceIn(0f, 1f) * kotlin.math.ln(20000f)).coerceIn(1f, 20000f))) },
                exact = { showNumber("FILTER ENV TIME ms", t, 1f, 20000f, 0) { setToneX(u, sound, 3, it) } }, reset = { setToneX(u, sound, 3, 120f) })
            val hp = toneX(u, sound, 6)
            val hn = if (hp < 10f) 0f else kotlin.math.ln(hp / 10f) / kotlin.math.ln(400f)
            knobControl(c, cx(10), r1, rad, hn, 0f, 1f, 0.002f, 260f, false, if (hp < 10f) "OFF" else "${"%.2f".format(hp)} Hz", "HI-PASS", hitW = hw,
                onChange = { nv -> setToneX(u, sound, 6, if (nv <= 0.002f) 0f else round(10f * Math.pow(400.0, nv.toDouble()).toFloat())) },
                exact = { showNumber("HIGH-PASS Hz (0 = OFF)", hp, 0f, 4000f, 0) { setToneX(u, sound, 6, it) } }, reset = { setToneX(u, sound, 6, 0f) })
            val wheel = toneX(u, sound, 11) >= 0.5f
            val wr = RectF(cx(11) - hw / 2f + 2f, r1 - 24f, cx(11) + hw / 2f - 2f, r1 + 24f)
            stripToggle(c, wr, "MOD\nWHEEL", wheel, C_AMBER, size = 10f)
            text(c, "ВИБРАТО ОТ", cx(11), r1 + 56f, 9f, C_KNOB_LABEL, center = true, spacing = 0.15f)
            hit(wr, { _, _ -> setToneX(u, sound, 11, if (wheel) 0f else 1f) })
        }
        // row 2: VEL->LEV VEL->CUT | quick START keys | LFO RATE DEPTH DELAY FADE | EQ LOW MID HIGH
        run {
            val vl = toneX(u, sound, 4)
            knobControl(c, cx(0), r2, rad, vl, 0f, 1f, 0.01f, 250f, false, "${"%.2f".format(vl * 100f)} %", "VEL→LEVEL", hitW = hw,
                onChange = { nv -> setToneX(u, sound, 4, nv) }, exact = { showNumber("VEL→LEVEL %", vl * 100f, 0f, 100f, 0) { setToneX(u, sound, 4, it / 100f) } }, reset = { setToneX(u, sound, 4, 1f) })
            val vc = toneX(u, sound, 5)
            knobControl(c, cx(1), r2, rad, vc, 0f, 8f, 0.05f, 250f, false, "%.2f oct".format(vc), "VEL→CUTOFF", hitW = hw,
                onChange = { nv -> setToneX(u, sound, 5, nv) }, exact = { showNumber("VEL→CUTOFF (октави)", vc, 0f, 8f, 2) { setToneX(u, sound, 5, it) } }, reset = { setToneX(u, sound, 5, 0f) })
            // quick SAMPLE START like Korg M3 OSC Start Offset: OFF 1 2 3 4
            val st = toneX(u, sound, 0)
            val qx0 = cx(2) - hw / 2f + 2f
            val qw = (2f * unit - 8f) / 5f
            for (k in 0 until 5) {
                val qr = RectF(qx0 + k * qw, r2 - 22f, qx0 + (k + 1) * qw - 3f, r2 + 22f)
                stripToggle(c, qr, if (k == 0) "OFF" else "$k", abs(st - kStartQuick[k]) < 0.5f, C_AMBER, size = 11f)
                hit(qr, { _, _ -> setToneX(u, sound, 0, kStartQuick[k]) })
            }
            text(c, "START: OFF · 10 · 25 · 60 · 120 ms", cx(3), r2 + 56f, 9f, C_KNOB_LABEL, center = true, spacing = 0.15f)
            val lfo = listOf(Triple("RATE", 7, Triple(0.1f, 20f, 0.05f)), Triple("DEPTH", 8, Triple(0f, 200f, 1f)), Triple("DELAY", 9, Triple(0f, 5000f, 10f)), Triple("FADE", 10, Triple(0f, 5000f, 10f)))
            lfo.forEachIndexed { k, (label, id, rng) ->
                val v = toneX(u, sound, id)
                val shown = when (id) { 7 -> "%.2f Hz".format(v); 8 -> "${round(v).toInt()} c"; else -> msText(v) }
                knobControl(c, cx(4 + k), r2, rad, v, rng.first, rng.second, rng.third, 260f, false, shown, "LFO $label", hitW = hw,
                    onChange = { nv -> setToneX(u, sound, id, nv) }, exact = { showNumber("ВИБРАТО $label", v, rng.first, rng.second, if (id == 7) 1 else 0) { setToneX(u, sound, id, it) } },
                    reset = { setToneX(u, sound, id, when (id) { 7 -> 5.5f; 10 -> 300f; else -> 0f }) })
            }
            listOf("LOW" to 12, "MID" to 13, "HIGH" to 14).forEachIndexed { k, (label, id) ->
                val v = toneX(u, sound, id)
                knobControl(c, cx(8 + k), r2, rad, v, -18f, 18f, 0.1f, 300f, true, "%.2f dB".format(v), "EQ $label", hitW = hw,
                    onChange = { nv -> setToneX(u, sound, id, nv) }, exact = { showNumber("EQ $label dB", v, -18f, 18f, 1) { setToneX(u, sound, id, it) } }, reset = { setToneX(u, sound, id, 0f) })
            }
            text(c, "допир на числото = точно", cx(11), r2 + 4f, 9f, C_HINT, center = true)
            text(c, "двоен допир = нула", cx(11), r2 + 18f, 9f, C_HINT, center = true)
        }
    }

    // ---------------------------------------------------------------- SILVETON_PORTA_MODES_V1 page
    private fun drawPorta(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        val u = selectedUpper
        val p = uParams(u)
        val kronos = p.getOrElse(42) { 0f } >= 0.5f
        section(c, sec, "PORTAMENTO · ПЛЪЗГАНЕ МЕЖДУ ТОНОВЕТЕ")
        val mA = RectF(sec.left + 16f, sec.top + 40f, sec.left + 176f, sec.top + 84f)
        val mB = RectF(sec.left + 184f, sec.top + 40f, sec.left + 344f, sec.top + 84f)
        stripToggle(c, mA, "SILVETON", !kronos, C_AMBER, size = 13f)
        stripToggle(c, mB, "KRONOS", kronos, C_AMBER, size = 13f)
        hit(mA, { _, _ -> setUParam(u, 42, 0f) }); hit(mB, { _, _ -> setUParam(u, 42, 1f) })
        val cy = 672f
        if (!kronos) {
            // SILVETON_PORTA_UI_ROUTE_V2: TIME is a TUNE parameter, not a TONE parameter.
            val porta = portamentoUiValue(s.portamentoSec)
            knobControl(c, sec.left + 115f, cy, 30f, porta, 0f, 100f, 1f, 300f, false, if (porta <= 0f) "OFF" else "${"%.2f".format(porta)} ms", "TIME", hitW = 200f,
                onChange = { nv -> commitTune(currentUpper(s).copy(portamentoSec = portamentoSecondsFromUi(nv))) },
                exact = { showNumber("PORTAMENTO TIME ms (0 = OFF)", porta, 0f, 100f, 0) { commitTune(currentUpper(s).copy(portamentoSec = portamentoSecondsFromUi(it))) } },
                reset = { commitTune(currentUpper(s).copy(portamentoSec = 0f)) })
            val bal = sampler.portaBalance().toFloat()
            knobControl(c, sec.left + 345f, cy, 30f, bal, 3f, 40f, 1f, 300f, false, "${bal.toInt()}", "BALANCE", hitW = 200f,
                onChange = { nv -> sampler.setPortaBalance(round(nv).toInt()) }, exact = { showNumber("PORTA BALANCE (мекота)", bal, 3f, 40f, 0) { sampler.setPortaBalance(round(it).toInt()) } }, reset = { sampler.setPortaBalance(11) })
            val near = sampler.portaNear().toFloat()
            knobControl(c, sec.left + 575f, cy, 30f, near, 0f, 400f, 1f, 300f, false, "${"%.2f".format(near)} ms", "TIME СЪСЕДНИ", hitW = 200f,
                onChange = { nv -> sampler.setPortaNear(round(nv).toInt()) }, exact = { showNumber("TIME СЪСЕДНИ ТОНОВЕ ms", near, 0f, 400f, 0) { sampler.setPortaNear(round(it).toInt()) } }, reset = { sampler.setPortaNear(90) })
            text(c, "Меко влизане · чисто достигане точно в TIME · BALANCE задава мекотата", sec.left + 16f, sec.bottom - 12f, 11f, C_HINT, maxWidth = 700f)
        } else {
            val t = p.getOrElse(45) { 40f }
            knobControl(c, sec.left + 115f, cy, 30f, t, 0f, 127f, 1f, 300f, false, if (t <= 0f) "OFF" else "${t.toInt()}", "TIME 0–127", hitW = 200f,
                onChange = { nv -> setUParam(u, 45, round(nv)) }, exact = { showNumber("KRONOS PORTA TIME (0 = OFF)", t, 0f, 127f, 0) { setUParam(u, 45, round(it)) } }, reset = { setUParam(u, 45, 0f) })
            val fing = p.getOrElse(43) { 0f } >= 0.5f
            val fr = RectF(sec.left + 250f, cy - 24f, sec.left + 430f, cy + 24f)
            stripToggle(c, fr, "FINGERED", fing, C_AMBER, size = 13f); hit(fr, { _, _ -> setUParam(u, 43, if (fing) 0f else 1f) })
            text(c, "само докато държиш клавиш", sec.left + 340f, cy + 48f, 10f, C_KNOB_LABEL, center = true)
            val rate = p.getOrElse(44) { 0f } >= 0.5f
            val rA = RectF(sec.left + 450f, cy - 24f, sec.left + 550f, cy + 24f)
            val rB = RectF(sec.left + 556f, cy - 24f, sec.left + 656f, cy + 24f)
            stripToggle(c, rA, "TIME", !rate, C_AMBER, size = 13f); stripToggle(c, rB, "RATE", rate, C_AMBER, size = 13f)
            hit(rA, { _, _ -> setUParam(u, 44, 0f) }); hit(rB, { _, _ -> setUParam(u, 44, 1f) })
            text(c, if (rate) "RATE: времето е за една октава" else "TIME: едно и също време за всеки интервал", sec.left + 553f, cy + 48f, 10f, C_KNOB_LABEL, center = true)
            text(c, "Меко, стегнато преливане · TIME / RATE · POLY / MONO", sec.left + 16f, sec.bottom - 12f, 11f, C_HINT, maxWidth = 700f)
        }
        // BEND RANGE of this Upper
        val bend = p.getOrElse(48) { s.pitchBendRange.toFloat() }
        knobControl(c, sec.left + 880f, cy, 30f, bend, 0f, 24f, 1f, 300f, false, "${bend.toInt()} st", "BEND RANGE", hitW = 160f,
            onChange = { nv -> setUParam(u, 48, round(nv)); refreshUpper(u) }, exact = { showNumber("PITCH BEND RANGE (полутонове)", bend, 0f, 24f, 0) { setUParam(u, 48, round(it)); refreshUpper(u) } }, reset = { setUParam(u, 48, 2f); refreshUpper(u) })
        listOf(2, 7, 12).forEachIndexed { k, v ->
            val br = RectF(sec.left + 980f + k * 72f, cy - 22f, sec.left + 1044f + k * 72f, cy + 22f)
            stripToggle(c, br, "$v", bend.toInt() == v, C_AMBER, size = 14f)
            hit(br, { _, _ -> setUParam(u, 48, v.toFloat()); refreshUpper(u) })
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

        // SILVETON_GUI_V3: OCTAVE and TRANSPOSE on - / + keys; both keys together = 0.
        stepper(c, RectF(cx[1] - 100f, 560f, cx[1] + 100f, 726f), "ОКТАВА", signed(s.octave),
            onMinus = { val cur = currentUpper(s); commitTune(cur.copy(octave = (cur.octave - 1).coerceAtLeast(-4))) },
            onPlus = { val cur = currentUpper(s); commitTune(cur.copy(octave = (cur.octave + 1).coerceAtMost(4))) },
            onReset = { commitTune(currentUpper(s).copy(octave = 0)) })
        stepper(c, RectF(cx[2] - 100f, 560f, cx[2] + 100f, 726f), "ТРАНСПОНИРАНЕ", signed(s.transpose),
            onMinus = { val cur = currentUpper(s); commitTune(cur.copy(transpose = (cur.transpose - 1).coerceAtLeast(-12))) },
            onPlus = { val cur = currentUpper(s); commitTune(cur.copy(transpose = (cur.transpose + 1).coerceAtMost(12))) },
            onReset = { commitTune(currentUpper(s).copy(transpose = 0)) })

        // SILVETON_TUNE_RANGES_V1: -1200..+1200 cents (one octave each way).
        knobControl(c, cx[3], cy, 42f, s.fineCents, -1200f, 1200f, 1f, 600f, true, "%.2f ct".format(s.fineCents), "ФИНА НАСТРОЙКА",
            onChange = { nv -> commitTune(currentUpper(s).copy(fineCents = nv)) },
            exact = { showNumber("ФИНА НАСТРОЙКА ct", s.fineCents, -1200f, 1200f, 0) { nv -> commitTune(currentUpper(s).copy(fineCents = nv)) } },
            reset = { commitTune(currentUpper(s).copy(fineCents = 0f)) })

        knobControl(c, cx[4], cy, 42f, s.pitchBendRange.toFloat(), 0f, 24f, 1f, 240f, false, "${s.pitchBendRange} st", "PITCH BEND",
            onChange = { nv -> commitTune(currentUpper(s).copy(pitchBendRange = nv.toInt())) },
            exact = { showNumber("PITCH BEND st", s.pitchBendRange.toFloat(), 0f, 24f, 0) { nv -> commitTune(currentUpper(s).copy(pitchBendRange = nv.toInt())) } },
            reset = { commitTune(currentUpper(s).copy(pitchBendRange = 1)) })

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
        TonePar.SUSTAIN, TonePar.RESONANCE -> "${"%.2f".format(v * 100f)} %"
        TonePar.CUTOFF -> when {
            v >= 19900f -> "OPEN"
            v < 1000f -> "${round(v).toInt()} Hz"
            else -> "%.2f kHz".format(v / 1000f)
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
                "%.2f dB".format(value), label, hitW = unit - 4f,
                onChange = { nv -> apply(nv) },
                exact = { showNumber("EQ $label dB", value, -18f, 18f, 1) { apply(it) } },
                reset = { apply(0f) })
        }
    }

    private fun drawUpperFx(c: Canvas, sec: RectF, s: SamplerAppController.UpperState) {
        section(c, sec, "FX · ИЗПРАЩАНЕ КЪМ ЕФЕКТИТЕ · INSERT")
        val u = selectedUpper
        val p = uParams(u)
        for (n in 1..4) {
            val value = when (n) { 1 -> s.fxSend1; 2 -> s.fxSend2; 3 -> p.getOrElse(46) { 0f }; else -> p.getOrElse(47) { 0f } }
            val apply = { nv: Float ->
                when (n) {
                    1 -> commitUpperFx(currentUpper(s).copy(fxSend1 = nv)); 2 -> commitUpperFx(currentUpper(s).copy(fxSend2 = nv))
                    3 -> setUParam(u, 46, nv); else -> setUParam(u, 47, nv)
                }
            }
            val fxType = fxP(n - 1).getOrElse(0) { 0f }.toInt()
            knobControl(c, sec.left + 90f + (n - 1) * 180f, 640f, 36f, value, 0f, 1f, 0.01f, 250f, false,
                "${"%.2f".format(value * 100f)} %", "FX$n · ${effectName(fxType)}", hitW = 170f,
                onChange = { nv -> apply(nv) },
                exact = { showNumber("FX$n SEND %", value * 100f, 0f, 100f, 0) { pct -> apply((pct / 100f).coerceIn(0f, 1f)) } },
                reset = { apply(0f) })
        }
        val ins = fxP(4 + u)
        val insR = RectF(sec.left + 760f, sec.top + 60f, sec.right - 16f, sec.top + 120f)
        lcd(c, insR, 8f)
        text(c, "INSERT U${u + 1}", insR.left + 14f, insR.top + 22f, 11f, C_LCD_DIM, face = faceMono, spacing = 0.2f)
        val insType = ins.getOrElse(0) { 0f }.toInt()
        text(c, effectName(insType) + if (insType != 0) "   MIX ${"%.2f".format(ins.getOrElse(1) { 0f } * 100f)} %" else "", insR.left + 14f, insR.top + 48f, 20f, C_LCD, face = faceMono, maxWidth = insR.width() - 28f)
        hit(insR, { _, _ -> fxUnitFocus = 4 + u; page = Page.EFFECTS; refreshFx() })
        val open = RectF(sec.left + 760f, sec.top + 136f, sec.right - 16f, sec.top + 184f)
        lightKey(c, open, "ОТВОРИ ЕФЕКТИТЕ (FX1–FX4 · INSERT)")
        hit(open, { _, _ -> fxUnitFocus = 0; page = Page.EFFECTS; refreshFx() })
        hintLine(c, sec)
    }



    // SILVETON_SYNTH_MENU_OFF_V1: the SYNTH and MATRIX pages were removed; every sound plays as the plain recording.

    // ---------------------------------------------------------------- SILVETON_EQ5_V1 page (finger design)
    private fun eqFreqX(hz: Float, x0: Float, x1: Float) = x0 + (x1 - x0) * (kotlin.math.log10(hz.coerceIn(20f, 20000f) / 20f) / 3f)
    private fun eqXFreq(x: Float, x0: Float, x1: Float) = (20.0 * Math.pow(1000.0, ((x - x0) / (x1 - x0)).coerceIn(0f, 1f).toDouble())).toFloat()
    private fun eqGainY(db: Float, y0: Float, y1: Float) = (y0 + y1) * 0.5f - (y1 - y0) * 0.5f * db / 18f
    private fun eqYGain(y: Float, y0: Float, y1: Float) = (((y0 + y1) * 0.5f - y) / ((y1 - y0) * 0.5f) * 18f).coerceIn(-18f, 18f)

    // Band b: 0 = HP, 1..5 = bands, 6 = LP. Parameter ids in the Upper params.
    private fun eqBandIds(b: Int): IntArray = when (b) { 0 -> intArrayOf(36, 37, 38); 6 -> intArrayOf(39, 40, 41); else -> intArrayOf(16 + (b - 1) * 4, 17 + (b - 1) * 4, 18 + (b - 1) * 4, 19 + (b - 1) * 4) }

    // Magnitude (dB) of the whole EQ at hz, analog prototypes (what the matched filters follow).
    private fun eqResponseDb(p: FloatArray, hz: Float): Float {
        if (p.getOrElse(15) { 1f } < 0.5f) return 0f
        var db = 0.0
        val w = hz.toDouble()
        for (b in 1..5) {
            val ids = eqBandIds(b)
            if (p.getOrElse(ids[0]) { 1f } < 0.5f) continue
            val f0 = p.getOrElse(ids[1]) { 1000f }.toDouble(); val g = p.getOrElse(ids[2]) { 0f }.toDouble(); val q = p.getOrElse(ids[3]) { 0.7f }.toDouble().coerceAtLeast(0.1)
            if (abs(g) < 0.01) continue
            val A = Math.pow(10.0, g / 40.0)
            val w2 = w * w; val f2 = f0 * f0
            val mag = when (b) {
                1 -> { val sa = Math.sqrt(A); val nr = A * f2 - w2; val ni = sa * f0 * w; val dr = f2 - A * w2; val di = sa * f0 * w
                       A * Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di)) }
                5 -> { val sa = Math.sqrt(A); val nr = f2 - A * w2; val ni = sa * f0 * w; val dr = A * f2 - w2; val di = sa * f0 * w
                       A * Math.sqrt((nr * nr + ni * ni) / (dr * dr + di * di)) }
                else -> { val d = (f2 - w2) * (f2 - w2); val k = w * f0 / q
                       Math.sqrt((d + A * A * k * k) / (d + k * k / (A * A))) }
            }
            db += 20.0 * Math.log10(mag)
        }
        if (p.getOrElse(36) { 0f } >= 0.5f) { val n = if (p.getOrElse(38) { 12f } >= 18f) 4.0 else 2.0; val r = w / p.getOrElse(37) { 40f }.toDouble(); db += 20.0 * Math.log10(Math.pow(r, n) / Math.sqrt(1.0 + Math.pow(r, 2.0 * n))) }
        if (p.getOrElse(39) { 0f } >= 0.5f) { val n = if (p.getOrElse(41) { 12f } >= 18f) 4.0 else 2.0; val r = w / p.getOrElse(40) { 18000f }.toDouble(); db += 20.0 * Math.log10(1.0 / Math.sqrt(1.0 + Math.pow(r, 2.0 * n))) }
        return db.toFloat().coerceIn(-60f, 30f)
    }

    private fun drawEqPage(c: Canvas) {
        val u = selectedUpper
        val p = uParams(u)
        val s = upperStates[u]
        board(c, RectF(112f, 108f, 1326f, 786f))
        // display
        val d = RectF(124f, 118f, 1314f, 466f)
        lcd(c, d, 8f)
        val x0 = d.left + 30f; val x1 = d.right - 30f; val y0 = d.top + 14f; val y1 = d.bottom - 24f
        listOf(50f to "50", 100f to "100", 200f to "200", 500f to "500", 1000f to "1k", 2000f to "2k", 5000f to "5k", 10000f to "10k").forEach { (hz, l) ->
            val x = eqFreqX(hz, x0, x1); line(c, x, y0, x, y1, Color.rgb(34, 40, 44), 1f); text(c, l, x, d.bottom - 8f, 10f, Color.rgb(120, 150, 160), center = true, face = faceMono)
        }
        for (db in listOf(-12f, -6f, 0f, 6f, 12f)) {
            val y = eqGainY(db, y0, y1); line(c, x0, y, x1, y, if (db == 0f) Color.rgb(90, 100, 90) else Color.rgb(34, 40, 44), 1f)
            text(c, if (db == 0f) "0" else "%+d".format(db.toInt()), d.left + 6f, y + 4f, 10f, Color.rgb(120, 150, 160), face = faceMono)
        }
        // analyzer
        if (analyzerOn) drawAnalyzer(c, u, x0, x1, y0, y1)
        // the curve
        val n = 240
        val path = android.graphics.Path()
        val fillPath = android.graphics.Path()
        for (i in 0..n) {
            val hz = (20.0 * Math.pow(1000.0, i / n.toDouble())).toFloat()
            val x = x0 + (x1 - x0) * i / n
            val y = eqGainY(eqResponseDb(p, hz).coerceIn(-18f, 18f), y0, y1)
            if (i == 0) { path.moveTo(x, y); fillPath.moveTo(x, y1); fillPath.lineTo(x, y) } else { path.lineTo(x, y); fillPath.lineTo(x, y) }
        }
        fillPath.lineTo(x1, y1); fillPath.close()
        paint.shader = null; paint.style = Paint.Style.FILL; paint.color = Color.argb(60, 255, 170, 50); c.drawPath(fillPath, paint)
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 14f; paint.color = Color.argb(45, 255, 180, 60); c.drawPath(path, paint)
        paint.strokeWidth = 7f; paint.color = Color.argb(90, 255, 180, 60); c.drawPath(path, paint)
        paint.strokeWidth = 2.5f; paint.color = Color.rgb(255, 215, 120); c.drawPath(path, paint)
        paint.style = Paint.Style.FILL
        // handles (registered after the display hit so they win)
        for (b in 0..6) {
            val ids = eqBandIds(b)
            val hz = p.getOrElse(ids[1]) { 1000f }
            val gain = if (b in 1..5) p.getOrElse(ids[2]) { 0f } else 0f
            val on = p.getOrElse(ids[0]) { 1f } >= 0.5f
            val x = eqFreqX(hz, x0, x1); val y = eqGainY(gain.coerceIn(-18f, 18f), y0, y1)
            val sel = b == eqBand
            val r = if (sel) 24f else 19f
            if (sel) { paint.color = Color.argb(90, 255, 150, 40); c.drawCircle(x, y, r + 16f, paint) }
            val col = if (!on) Color.rgb(110, 112, 116) else if (sel) C_ORANGE else if (b == 0 || b == 6) C_LCD_OK else C_LCD_VALUE
            paint.color = Color.argb(160, 0, 0, 0); c.drawCircle(x, y + 3f, r, paint)
            paint.color = col; c.drawCircle(x, y, r, paint)
            paint.color = Color.argb(60, 0, 0, 0); c.drawCircle(x, y + r * 0.45f, r * 0.55f, paint)
            paint.color = col; c.drawCircle(x, y + r * 0.35f, r * 0.5f, paint)
            paint.color = Color.argb(110, 255, 255, 255); c.drawCircle(x - r * 0.25f, y - r * 0.35f, r * 0.35f, paint)
            text(c, when (b) { 0 -> "HP"; 6 -> "LP"; else -> "$b" }, x, y + 6f, if (sel) 15f else 12f, Color.rgb(35, 20, 0), center = true, face = faceBold)
            val hr = RectF(x - 34f, y - 34f, x + 34f, y + 34f)
            hit(hr, { _, _ -> eqBand = b }, { if (b in 1..5) setUParam(u, ids[2], 0f) },
                { px, py, _ ->
                    eqBand = b
                    setUParam(u, ids[1], round(eqXFreq(px, x0, x1)))
                    if (b in 1..5) setUParam(u, ids[2], round(eqYGain(py, y0, y1) * 10f) / 10f)
                })
        }
        // analyzer keys in the display
        val ak = listOf("АНАЛ." to analyzerOn, (if (analyzerAfter) "СЛЕД EQ" else "ПРЕДИ EQ") to analyzerAfter, "ПИК" to analyzerPeak)
        ak.forEachIndexed { k, (label, on) ->
            val kr = RectF(d.right - 20f - (3 - k) * 118f, d.top + 10f, d.right - 20f - (2 - k) * 118f - 8f, d.top + 42f)
            stripToggle(c, kr, label, on, if (k == 0) C_LCD_OK else C_AMBER, size = 10f)
            hit(kr, { _, _ -> when (k) { 0 -> analyzerOn = !analyzerOn; 1 -> analyzerAfter = !analyzerAfter; else -> { analyzerPeak = !analyzerPeak; analyzerHold = null } } })
        }
        // band keys
        val kw = (1314f - 124f - 6f * 8f) / 7f
        for (b in 0..6) {
            val kr = RectF(124f + b * (kw + 8f), 478f, 124f + b * (kw + 8f) + kw, 522f)
            stripToggle(c, kr, when (b) { 0 -> "HP"; 6 -> "LP"; else -> "ЛЕНТА $b" }, eqBand == b, C_ORANGE, size = 14f)
            hit(kr, { _, _ -> eqBand = b })
        }
        // sliders of the chosen band
        val ids = eqBandIds(eqBand)
        val isPass = eqBand == 0 || eqBand == 6
        val hz = p.getOrElse(ids[1]) { 1000f }
        fun hzText(v: Float) = if (v < 1000f) "%.2f Hz".format(v) else "%.2f kHz".format(v / 1000f)
        val fMin = if (eqBand == 6) 1000f else if (eqBand == 0) 10f else 20f
        val fMax = if (eqBand == 0) 2000f else 20000f
        eqSlider(c, RectF(124f, 534f, 700f, 600f), "FREQ", hzText(hz), (kotlin.math.ln(hz / fMin) / kotlin.math.ln(fMax / fMin)).coerceIn(0f, 1f),
            { t -> setUParam(u, ids[1], round((fMin * Math.pow((fMax / fMin).toDouble(), t.toDouble())).toFloat())) },
            { setUParam(u, ids[1], (hz * 1.02f).coerceAtMost(fMax)) }, { setUParam(u, ids[1], (hz / 1.02f).coerceAtLeast(fMin)) },
            { showNumber("ЛЕНТА ${eqBand} FREQ Hz", hz, fMin, fMax, 0) { setUParam(u, ids[1], it) } })
        if (!isPass) {
            val gain = p.getOrElse(ids[2]) { 0f }
            eqSlider(c, RectF(124f, 610f, 700f, 676f), "GAIN", "%+.1f dB".format(gain), (gain + 18f) / 36f,
                { t -> setUParam(u, ids[2], round((t * 36f - 18f) * 10f) / 10f) },
                { setUParam(u, ids[2], (gain + 0.5f).coerceAtMost(18f)) }, { setUParam(u, ids[2], (gain - 0.5f).coerceAtLeast(-18f)) },
                { showNumber("ЛЕНТА $eqBand GAIN dB", gain, -18f, 18f, 1) { setUParam(u, ids[2], it) } })
            val q = p.getOrElse(ids[3]) { 0.7f }
            eqSlider(c, RectF(714f, 534f, 1314f, 600f), "Q", "%.2f".format(q), (kotlin.math.ln(q / 0.1f) / kotlin.math.ln(100f)).coerceIn(0f, 1f),
                { t -> setUParam(u, ids[3], round((0.1f * Math.pow(100.0, t.toDouble())).toFloat() * 100f) / 100f) },
                { setUParam(u, ids[3], (q * 1.05f).coerceAtMost(10f)) }, { setUParam(u, ids[3], (q / 1.05f).coerceAtLeast(0.1f)) },
                { showNumber("ЛЕНТА $eqBand Q", q, 0.1f, 10f, 2) { setUParam(u, ids[3], it) } })
        } else {
            val slope = p.getOrElse(ids[2]) { 12f }
            val a = RectF(124f, 610f, 400f, 676f); val b = RectF(412f, 610f, 700f, 676f)
            stripToggle(c, a, "12 dB / ОКТ", slope < 18f, C_AMBER, size = 14f); stripToggle(c, b, "24 dB / ОКТ", slope >= 18f, C_AMBER, size = 14f)
            hit(a, { _, _ -> setUParam(u, ids[2], 12f) }); hit(b, { _, _ -> setUParam(u, ids[2], 24f) })
        }
        val onR = RectF(714f, 610f, 900f, 676f); val eqR = RectF(912f, 610f, 1098f, 676f); val zR = RectF(1110f, 610f, 1314f, 676f)
        val bandOn = p.getOrElse(ids[0]) { 1f } >= 0.5f
        stripToggle(c, onR, if (isPass) (if (eqBand == 0) "HP ON" else "LP ON") else "ЛЕНТА ON", bandOn, C_LCD_OK, size = 14f); hit(onR, { _, _ -> setUParam(u, ids[0], if (bandOn) 0f else 1f) })
        val eqOn = p.getOrElse(15) { 1f } >= 0.5f
        stripToggle(c, eqR, "EQ ON", eqOn, C_LCD_OK, size = 14f); hit(eqR, { _, _ -> setUParam(u, 15, if (eqOn) 0f else 1f) })
        lightKey(c, zR, "НУЛИРАЙ", size = 14f)
        hit(zR, { _, _ ->
            for (b in 1..5) { val bi = eqBandIds(b); setUParam(u, bi[2], 0f) }
            setUParam(u, 36, 0f); setUParam(u, 39, 0f); flash("EQ НУЛИРАН U${u + 1}")
        })
        // bottom: title + back
        text(c, "UPPER ${u + 1} · ${s?.soundName ?: ""} · EQ", 124f, 720f, 16f, C_AMBER, face = faceMonoBold, maxWidth = 800f)
        text(c, "влачи точка: ляво/дясно = честота · горе/долу = сила · двоен допир на точка = 0 dB · двоен допир на звук = ★", 124f, 748f, 11f, C_HINT, maxWidth = 900f)
        val back = RectF(1110f, 700f, 1314f, 760f)
        lightKey(c, back, "◀ КОМБИ", size = 14f)
        hit(back, { _, _ -> editSound = 0; tab = UpperTab.TUNE })
        if (analyzerOn) postInvalidateDelayed(33L)
    }

    private fun eqSlider(c: Canvas, r: RectF, label: String, value: String, t: Float, onDrag: (Float) -> Unit, plus: () -> Unit, minus: () -> Unit, exact: () -> Unit) {
        fill(c, r, 10f, Color.rgb(41, 44, 49))
        text(c, label, r.left + 14f, r.top + 30f, 12f, C_KNOB_LABEL, face = faceBold)
        val vr = RectF(r.left + 90f, r.top + 8f, r.left + 290f, r.top + 42f)
        lcd(c, vr, 6f); text(c, value, vr.centerX(), vr.top + 25f, 17f, C_AMBER, center = true, face = faceMono)
        hit(vr, { _, _ -> exact() })
        val mR = RectF(r.right - 150f, r.top + 8f, r.right - 82f, r.bottom - 8f); val pR = RectF(r.right - 74f, r.top + 8f, r.right - 6f, r.bottom - 8f)
        lightKey(c, mR, "−", size = 22f); lightKey(c, pR, "+", size = 22f)
        hit(mR, { _, _ -> minus() }); hit(pR, { _, _ -> plus() })
        val tx0 = r.left + 14f; val tx1 = r.right - 166f
        fill(c, RectF(tx0, r.bottom - 20f, tx1, r.bottom - 12f), 4f, Color.rgb(16, 18, 16))
        val px = tx0 + (tx1 - tx0) * t.coerceIn(0f, 1f)
        fill(c, RectF(tx0, r.bottom - 20f, px, r.bottom - 12f), 4f, C_AMBER)
        paint.color = Color.BLACK; c.drawCircle(px, r.bottom - 16f + 2f, 16f, paint)
        paint.color = Color.rgb(236, 238, 241); c.drawCircle(px, r.bottom - 16f, 16f, paint)
        hit(RectF(tx0 - 10f, r.top + 44f, tx1 + 10f, r.bottom), { x, _ -> onDrag(((x - tx0) / (tx1 - tx0)).coerceIn(0f, 1f)) }, null,
            { x, _, _ -> onDrag(((x - tx0) / (tx1 - tx0)).coerceIn(0f, 1f)) })
    }

    // ---------------------------------------------------------------- SILVETON_ANALYZER_V1 (FFT 8192, 1/12 oct, 3 dB/oct tilt, peak hold)
    private val fftRe = FloatArray(8192)
    private val fftIm = FloatArray(8192)
    private val fftWin = FloatArray(8192) { i -> val a = 2.0 * Math.PI * i / 8191.0; (0.35875 - 0.48829 * Math.cos(a) + 0.14128 * Math.cos(2 * a) - 0.01168 * Math.cos(3 * a)).toFloat() }
    private val fftWinSum = fftWin.sum()
    private val kAnalyzerBins = 240

    private fun fft(re: FloatArray, im: FloatArray) {
        val n = re.size
        var j = 0
        for (i in 1 until n) {
            var bit = n shr 1
            while (j and bit != 0) { j = j xor bit; bit = bit shr 1 }
            j = j xor bit
            if (i < j) { val tr = re[i]; re[i] = re[j]; re[j] = tr; val ti = im[i]; im[i] = im[j]; im[j] = ti }
        }
        var len = 2
        while (len <= n) {
            val ang = -2.0 * Math.PI / len
            val wr = Math.cos(ang).toFloat(); val wi = Math.sin(ang).toFloat()
            var i = 0
            while (i < n) {
                var cr = 1f; var ci = 0f
                for (k in 0 until len / 2) {
                    val ar = re[i + k]; val ai = im[i + k]
                    val br = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci
                    val bi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr
                    re[i + k] = ar + br; im[i + k] = ai + bi
                    re[i + k + len / 2] = ar - br; im[i + k + len / 2] = ai - bi
                    val ncr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = ncr
                }
                i += len
            }
            len = len shl 1
        }
    }

    private fun drawAnalyzer(c: Canvas, u: Int, x0: Float, x1: Float, y0: Float, y1: Float) {
        val now = android.os.SystemClock.uptimeMillis()
        val sr = frameAudioStatus.sampleRate.takeIf { it > 0 } ?: 96000
        if (now - analyzerLastMs >= 30L) {
            analyzerLastMs = now
            val frame = sampler.analyzerFrame(u, analyzerAfter)
            if (frame.size >= 8192) {
                for (i in 0 until 8192) { fftRe[i] = frame[i] * fftWin[i]; fftIm[i] = 0f }
                fft(fftRe, fftIm)
                val cur = analyzerCurve ?: FloatArray(kAnalyzerBins) { -90f }.also { analyzerCurve = it }
                val hold = analyzerHold ?: FloatArray(kAnalyzerBins) { -90f }.also { analyzerHold = it }
                val binHz = sr / 8192f
                for (k in 0 until kAnalyzerBins) {
                    val hz = (20.0 * Math.pow(1000.0, k / (kAnalyzerBins - 1).toDouble()))
                    val lo = (hz / Math.pow(2.0, 1.0 / 24.0) / binHz).toInt().coerceIn(1, 4095)
                    val hi = (hz * Math.pow(2.0, 1.0 / 24.0) / binHz).toInt().coerceIn(lo, 4095)
                    var e = 0.0
                    for (b in lo..hi) e += (fftRe[b] * fftRe[b] + fftIm[b] * fftIm[b]).toDouble()
                    e /= (hi - lo + 1)
                    val mag = Math.sqrt(e) * 2.0 / fftWinSum
                    var db = (20.0 * Math.log10(mag + 1e-9)).toFloat() + 3f * (kotlin.math.ln(hz.toFloat() / 1000f) / kotlin.math.ln(2f))
                    db = db.coerceIn(-90f, 0f)
                    cur[k] = if (db > cur[k]) db else max(cur[k] - 0.9f, db)
                    hold[k] = if (analyzerPeak) max(db, hold[k] - 0.08f) else -90f
                }
            }
        }
        val cur = analyzerCurve ?: return
        val hold = analyzerHold ?: return
        fun sy(db: Float) = y1 - (y1 - y0) * (db + 90f) / 90f
        val path = android.graphics.Path(); path.moveTo(x0, y1)
        for (k in 0 until kAnalyzerBins) path.lineTo(x0 + (x1 - x0) * k / (kAnalyzerBins - 1), sy(cur[k]))
        path.lineTo(x1, y1); path.close()
        paint.shader = LinearGradient(0f, y0, 0f, y1, Color.argb(150, 70, 150, 210), Color.argb(12, 70, 150, 210), Shader.TileMode.CLAMP)
        paint.style = Paint.Style.FILL; c.drawPath(path, paint); paint.shader = null
        val lp = android.graphics.Path()
        for (k in 0 until kAnalyzerBins) { val x = x0 + (x1 - x0) * k / (kAnalyzerBins - 1); if (k == 0) lp.moveTo(x, sy(cur[k])) else lp.lineTo(x, sy(cur[k])) }
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 2f; paint.color = Color.argb(230, 150, 205, 245); c.drawPath(lp, paint)
        if (analyzerPeak) {
            val hp = android.graphics.Path()
            for (k in 0 until kAnalyzerBins) { val x = x0 + (x1 - x0) * k / (kAnalyzerBins - 1); if (k == 0) hp.moveTo(x, sy(hold[k])) else hp.lineTo(x, sy(hold[k])) }
            paint.strokeWidth = 1f; paint.color = Color.argb(110, 230, 240, 250); c.drawPath(hp, paint)
        }
        paint.style = Paint.Style.FILL
        for (db in listOf(-20, -40, -60, -80)) text(c, "$db", x1 + 4f, sy(db.toFloat()) + 4f, 9f, Color.rgb(100, 140, 170), face = faceMono)
    }

    // ---------------------------------------------------------------- SILVETON_PICKER_V1 full-screen sound picker
    private fun openPicker(upper: Int, layer: Int) {
        if (sampler.soundCatalog().isEmpty()) { showSoundPicker(upper); return }
        val prevSlot = if (layer < 0) (lastPickedSound.getOrNull(upper)?.slot ?: sampler.soundCatalog().firstOrNull { it.name == upperStates.getOrNull(upper)?.soundName }?.slot ?: 0) else layerSlot(upper, layer)
        val prevName = if (layer < 0) (upperStates.getOrNull(upper)?.soundName ?: "") else lName(upper, layer)
        val pk = PickerState(upper, layer, prevSlot, prevName)
        val all = sampler.soundCatalog()
        val idx = all.indexOfFirst { it.slot == prevSlot }
        pk.page = if (idx >= 0) idx / 40 else 0
        picker = pk
        page = Page.COMBI
        invalidate()
    }

    private fun pickerItems(pk: PickerState): List<SamplerAppController.SoundItem> {
        val all = sampler.soundCatalog()
        return when {
            pk.search.isNotEmpty() -> all.filter { it.name.contains(pk.search, ignoreCase = true) }
            pk.favOnly -> { val f = sampler.soundFavourites(); all.filter { it.slot in f } }
            pk.recentOnly -> { val r = sampler.recentSounds(); r.mapNotNull { slot -> all.firstOrNull { it.slot == slot } } }
            else -> all
        }
    }

    private fun pickerAssign(pk: PickerState, item: SamplerAppController.SoundItem?) {
        if (pk.layer < 0) {
            if (item == null) { sampler.clearUpperSound(pk.upper); lastPickedSound[pk.upper] = null } else { sampler.assignUpperSound(pk.upper, item); lastPickedSound[pk.upper] = item }
        } else {
            sampler.setLayerSound(pk.upper, pk.layer, item?.slot ?: 0)
        }
        pk.chosenSlot = item?.slot ?: 0
        refreshUpper(pk.upper)
    }

    private fun drawPicker(c: Canvas, pk: PickerState) {
        board(c, RectF(112f, 108f, 1326f, 786f))
        val items = pickerItems(pk)
        val pages = max(1, (items.size + 39) / 40)
        pk.page = pk.page.coerceIn(0, pages - 1)
        // top row: ТЪРСИ · ПОСЛЕДНИ · ★ · target · CANCEL · OK
        val fr = RectF(120f, 116f, 230f, 158f)
        stripToggle(c, fr, "ТЪРСИ", pk.search.isNotEmpty(), C_AMBER, size = 12f)
        hit(fr, { _, _ -> if (pk.search.isNotEmpty()) { pk.search = ""; pk.page = 0; invalidate() } else showTextPage("ТЪРСИ ЗВУК ПО ИМЕ", "") { q -> pk.search = q.trim(); pk.favOnly = false; pk.recentOnly = false; pk.page = 0; invalidate() } })
        val rr = RectF(238f, 116f, 358f, 158f)
        stripToggle(c, rr, "ПОСЛЕДНИ", pk.recentOnly, C_AMBER, size = 12f)
        hit(rr, { _, _ -> pk.recentOnly = !pk.recentOnly; pk.favOnly = false; pk.search = ""; pk.page = 0 })
        val sr = RectF(366f, 116f, 426f, 158f)
        stripToggle(c, sr, "★", pk.favOnly, C_ORANGE, size = 18f)
        hit(sr, { _, _ -> pk.favOnly = !pk.favOnly; pk.recentOnly = false; pk.search = ""; pk.page = 0 })
        val tgt = RectF(436f, 116f, 1010f, 158f)
        lcd(c, tgt, 8f)
        val who = if (pk.layer < 0) "UPPER ${pk.upper + 1} · ОСНОВЕН" else "UPPER ${pk.upper + 1} · LAYER ${pk.layer + 1}"
        val nowName = if (pk.layer < 0) (upperStates.getOrNull(pk.upper)?.soundName ?: "") else lName(pk.upper, pk.layer)
        text(c, "$who   ▶   ${if (pk.chosenSlot > 0) "%03d ".format(pk.chosenSlot) else ""}$nowName", tgt.left + 14f, tgt.top + 29f, 16f, C_LCD, face = faceMono, maxWidth = tgt.width() - 28f)
        val cancel = RectF(1020f, 116f, 1166f, 158f); val ok = RectF(1174f, 116f, 1318f, 158f)
        lightKey(c, cancel, "CANCEL", size = 13f); stripToggle(c, ok, "OK", true, C_LCD_OK, size = 15f)
        hit(cancel, { _, _ ->
            val all = sampler.soundCatalog(); val prev = all.firstOrNull { it.slot == pk.previousSlot }
            if (pk.chosenSlot != pk.previousSlot) pickerAssign(pk, prev)
            picker = null
        })
        hit(ok, { _, _ -> if (pk.chosenSlot > 0) sampler.rememberRecentSound(pk.chosenSlot); picker = null })
        // page keys
        val pw = (1198f - (pages - 1) * 4f) / pages
        for (pg in 0 until pages) {
            val pr = RectF(120f + pg * (pw + 4f), 166f, 120f + pg * (pw + 4f) + pw, 196f)
            stripToggle(c, pr, "${pg + 1}", pk.page == pg, C_AMBER, size = 11f)
            hit(pr, { _, _ -> pk.page = pg })
        }
        // grid 5 x 8
        val favs = sampler.soundFavourites()
        val gx = 120f; val gy = 204f; val cw = (1198f - 4f * 6f) / 5f; val ch = (778f - gy - 7f * 4f) / 8f
        for (k in 0 until 40) {
            val idx = pk.page * 40 + k
            if (idx >= items.size) break
            val item = items[idx]
            val col = k % 5; val row = k / 5
            val cr = RectF(gx + col * (cw + 6f), gy + row * (ch + 4f), gx + col * (cw + 6f) + cw, gy + row * (ch + 4f) + ch)
            val chosen = item.slot == pk.chosenSlot
            fill(c, cr, 8f, if (chosen) C_DARK_KEY else C_KEY_LIGHT)
            text(c, "%03d".format(item.slot), cr.left + 10f, cr.top + 24f, 11f, if (chosen) C_LCD_DIM else C_LABEL_GREY, face = faceMono)
            text(c, item.name, cr.left + 10f, cr.bottom - 14f, 15f, if (chosen) C_AMBER else C_INK, face = faceBold, maxWidth = cw - 40f)
            if (item.slot in favs) text(c, "★", cr.right - 24f, cr.top + 24f, 14f, C_ORANGE, face = faceBold)
            hit(cr, { _, _ -> pickerAssign(pk, item) },
                { val f = favs.toMutableSet(); if (item.slot in f) f.remove(item.slot) else f.add(item.slot); sampler.saveSoundFavourites(f); invalidate() })
        }
        if (items.isEmpty()) text(c, "НЯМА ЗВУЦИ", 719f, 480f, 24f, C_KNOB_LABEL, center = true, face = faceBold)
    }

    // ------------------------------------------------------------------ LIST

    // SILVETON_LIST_GRID_V1: the LIST like the BLENDS picker - pages of 20 (4 x 5).
    // Tap a BLEND = play it. Tap an empty place = choose it for STORE.
    // STORE writes the COMBI that sounds now to the chosen place (asks before
    // writing over a BLEND). EDIT opens the chosen BLEND in COMBI.
    // SILVETON_LIST_V2: 30 BLENDS a page (5 x 6), 7 page keys, favourites and search,
    // one row of big keys for thick fingers (TRANSPOSE - [0] + : both at once = 0).
    private fun listVisible(): List<Int> = when {
        listSearch.isNotEmpty() -> (0 until LIST_SIZE).filter { realListItem(it)?.name?.contains(listSearch, ignoreCase = true) == true }
        listFavOnly -> (0 until LIST_SIZE).filter { it in listFavs && realListItem(it) != null }
        else -> (0 until LIST_SIZE).toList()
    }

    private fun drawList(c: Canvas) {
        if (!listCacheLoaded) refreshListCache()
        val active = cachedActiveListEntry.takeIf { realListItem(it) != null } ?: -1
        val visible = listVisible()
        val pages = max(1, (visible.size + LIST_PER_PAGE - 1) / LIST_PER_PAGE)
        if (active != listFollowedActive) {
            listFollowedActive = active
            if (active >= 0) {
                val vi = visible.indexOf(active)
                if (vi >= 0) listPage = vi / LIST_PER_PAGE
                listSelected = active
            }
        }
        listPage = listPage.coerceIn(0, pages - 1)
        if (listSelected !in 0 until LIST_SIZE) listSelected = if (active >= 0) active else -1

        val keyW = (1214f - (pages - 1) * 6f) / pages
        for (i in 0 until pages) {
            val pr = RectF(112f + i * (keyW + 6f), 108f, 112f + i * (keyW + 6f) + keyW, 148f)
            val hasData = (i * LIST_PER_PAGE until min(visible.size, (i + 1) * LIST_PER_PAGE)).any { realListItem(visible[it]) != null }
            val activeHere = active >= 0 && visible.indexOf(active) / LIST_PER_PAGE == i && visible.contains(active)
            pageKey(c, pr, (i + 1).toString(), i == listPage, hasData, activeHere)
            hit(pr, { _, _ -> listPage = i })
        }

        val gridTop = 156f
        val gridBottom = 640f
        val gap = 7f
        val cols = 5
        val rows = 6
        val colW = (1214f - (cols - 1) * gap) / cols
        val rowH = (gridBottom - gridTop - (rows - 1) * gap) / rows
        for (j in 0 until LIST_PER_PAGE) {
            val vi = listPage * LIST_PER_PAGE + j
            if (vi >= visible.size) break
            val idx = visible[vi]
            val col = j % cols
            val row = j / cols
            val rr = RectF(112f + col * (colW + gap), gridTop + row * (rowH + gap),
                112f + col * (colW + gap) + colW, gridTop + row * (rowH + gap) + rowH)
            val item = realListItem(idx)
            listGridCell(c, rr, idx, item, idx == active, idx == listSelected)
            if (item != null) {
                hit(rr, { _, _ ->
                    listSelected = idx
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
            } else {
                hit(rr, { _, _ -> listSelected = idx; flash("ЛИСТ %03d ИЗБРАН - STORE ЗАПИСВА ТУК".format(idx + 1)) })
            }
        }
        if (visible.isEmpty()) {
            text(c, if (listSearch.isNotEmpty()) "НЯМА БЛЕНДА С „$listSearch“" else "НЯМА ЛЮБИМИ - ЗАДРЪЖ БЛЕНДА → ★",
                719f, 400f, 20f, C_INK_SOFT, center = true, face = faceBold)
        }
        text(c, "◀  плъзни за страница  ▶", 719f, 652f, 10f, C_LABEL_GREY, center = true)

        // ---- one row of big keys
        val top = 664f
        val bottom = 786f
        val chosenItem = realListItem(listSelected)
        var x = 112f
        val storeR = RectF(x, top, x + 186f, bottom); x = storeR.right + 8f
        accentKey(c, storeR, if (listSelected >= 0) "STORE ► %03d".format(listSelected + 1) else "STORE", true)
        hit(storeR, { _, _ -> storeToList() })
        val editR = RectF(x, top, x + 118f, bottom); x = editR.right + 8f
        lightKey(c, editR, "EDIT", enabled = chosenItem != null, size = 17f)
        if (chosenItem != null) hit(editR, { _, _ -> editListCombi(chosenItem, listSelected) })
        val tR = RectF(x, top, x + 96f, bottom); x = tR.right + 6f
        val qR = RectF(x, top, x + 96f, bottom); x = qR.right + 22f
        stripToggle(c, tR, if (sampler.hasTercaUppers()) "T  " + (if (sampler.tercaMasterOn()) "ON" else "OFF") else "T  —",
            sampler.tercaMasterOn(), C_AMBER, size = 18f)
        stripToggle(c, qR, if (sampler.hasScaleUppers()) "Q  " + (if (sampler.scaleMasterOn()) "ON" else "OFF") else "Q  —",
            sampler.scaleMasterOn(), C_AMBER, size = 18f)
        hit(tR, { _, _ -> if (!sampler.hasTercaUppers()) flash("НЯМА UPPER С T - ИЗБЕРИ T В COMBI") else sampler.toggleTercaMaster(); invalidate() })
        hit(qR, { _, _ -> if (!sampler.hasScaleUppers()) flash("НЯМА UPPER С Q - ИЗБЕРИ Q В COMBI") else sampler.toggleScaleMaster(); invalidate() })
        // TRANSPOSE: big - [value] +, both at once = 0
        val g = globalState ?: sampler.globalUiState()?.also { globalState = it }
        val tm = RectF(x, top, x + 118f, bottom); x = tm.right + 4f
        val tv = RectF(x, top, x + 104f, bottom); x = tv.right + 4f
        val tp = RectF(x, top, x + 118f, bottom); x = tp.right + 16f
        stripToggle(c, tm, "−", false, C_AMBER, size = 44f)
        stripToggle(c, tp, "+", false, C_AMBER, size = 44f)
        lcd(c, tv, 8f)
        text(c, "TRANSPOSE", tv.centerX(), tv.top + 20f, 10f, C_LCD_DIM, center = true, face = faceMono)
        text(c, signed(g?.transpose ?: 0), tv.centerX(), tv.bottom - 24f, 36f, C_LCD, center = true, face = faceMono)
        text(c, "− и + заедно = 0", tv.centerX(), tv.bottom - 7f, 8f, C_LCD_DIM, center = true)
        hit(tv, { _, _ -> }, { sampler.stepGlobalTranspose(0); refreshGlobal() })
        hit(tm, { _, _ -> sampler.stepGlobalTranspose(-1); refreshGlobal() })
        hit(tp, { _, _ -> sampler.stepGlobalTranspose(1); refreshGlobal() })
        pairHits.add(PairHit(tm, tp) { sampler.stepGlobalTranspose(0); refreshGlobal() })
        val ts = RectF(x, top, x + 150f, bottom); x = ts.right + 6f
        scanKey(c, ts, 1, "T SCAN\n${tercaText(sampler.tercaState())}")
        val qs = RectF(x, top, min(x + 150f, 1326f), bottom)
        val notes = sampler.quarterNotes()
        val names = (0 until 12).filter { (notes and (1 shl it)) != 0 }.joinToString(" ") { SamplerAppController.NOTE_NAMES[it] }
        scanKey(c, qs, 2, "Q SCAN\n${names.ifEmpty { "—" }}")
    }

    private fun listGridCell(c: Canvas, r: RectF, idx: Int, item: SamplerAppController.ListItem?, active: Boolean, chosen: Boolean) {
        if (chosen) fill(c, RectF(r.left - 3f, r.top - 3f, r.right + 3f, r.bottom + 3f), 13f, C_ORANGE)
        else shadow(c, r, 10f, 2f, 60)
        val face = when { active -> Color.rgb(255, 150, 50); item != null -> Color.rgb(44, 50, 58); else -> Color.rgb(30, 34, 39) }
        fill(c, r, 10f, face)
        if (!active) line(c, r.left + 10f, r.top + 1f, r.right - 10f, r.top + 1f, Color.rgb(78, 88, 100), 1f)
        if (isDown(r)) fill(c, r, 10f, Color.argb(60, 0, 0, 0))
        val numColor = when { active -> Color.rgb(20, 20, 20); item != null -> C_AMBER; else -> Color.rgb(110, 118, 128) }
        text(c, "%03d".format(idx + 1), r.left + 14f, r.top + 24f, 15f, numColor, face = faceMonoBold)
        if (idx in listFavs) text(c, "★", r.right - 12f, r.top + 24f, 15f, if (active) Color.rgb(20, 20, 20) else Color.rgb(255, 200, 90), right = true)
        if (item != null) {
            text(c, item.name, r.left + 14f, r.bottom - 16f, 18f, if (active) Color.rgb(20, 20, 20) else Color.WHITE,
                face = faceBold, maxWidth = r.width() - 28f)
        } else {
            text(c, "—", r.left + 14f, r.bottom - 22f, 20f, Color.rgb(96, 104, 114))
        }
    }

    /** STORE: the COMBI that sounds now goes to the chosen LIST place. */
    private fun storeToList() {
        val list = sampler.listItems(0, 0, 0)
        fun nameAt(pos: Int): String? = list.getOrNull(pos)?.takeUnless { sampler.isEmptyListItem(it) }?.name
        val pos = if (listSelected in 0 until LIST_SIZE) listSelected
            else (0 until LIST_SIZE).firstOrNull { nameAt(it) == null } ?: -1
        if (pos < 0) { flash("ЛИСТЪТ Е ПЪЛЕН"); return }
        val old = nameAt(pos)
        val askName = {
            showTextPage("STORE  →  ЛИСТ %03d".format(pos + 1), old ?: sampler.combiName()) { newName ->
                doWriteList(pos, newName.trim().ifBlank { sampler.combiName() })
                listSelected = pos
            }
        }
        if (old == null) { askName(); return }
        AlertDialog.Builder(context)
            .setTitle("ЛИСТ %03d".format(pos + 1))
            .setMessage("Тук вече има „$old“.\nДа запиша ли отгоре?")
            .setPositiveButton("ДА, ЗАПИШИ") { _, _ -> askName() }
            .setNegativeButton("НЕ", null)
            .show()
    }

    // SILVETON_SET_SAVE_AS_V1
    private fun saveSetAs() {
        showTextPage("SAVE AS  (ЦЕЛИЯТ СЕТ)", "SILVETON SET") { name ->
            val doSave = {
                val err = sampler.saveSetAs(name)
                flash(if (err == null) "СЕТЪТ Е ЗАПИСАН: ${name.trim()}" else "ГРЕШКА: $err")
            }
            if (sampler.setExists(name)) {
                AlertDialog.Builder(context)
                    .setTitle("SAVE AS")
                    .setMessage("Сет „${name.trim()}“ вече има.\nДа запиша ли отгоре?")
                    .setPositiveButton("ДА, ЗАПИШИ") { _, _ -> doSave() }
                    .setNegativeButton("НЕ", null)
                    .show()
            } else doSave()
        }
    }

    private fun loadSet() {
        val sets = sampler.savedSets()
        if (sets.isEmpty()) {
            AlertDialog.Builder(context).setTitle("LOAD")
                .setMessage("Няма записани сетове.\nЗапиши с SAVE AS. Папка: ${sampler.setsFolders()}")
                .setPositiveButton("OK", null).show()
            return
        }
        showGridPicker("LOAD  -  ЗАПИСАНИ СЕТОВЕ", sets.mapIndexed { i, n -> "%02d".format(i + 1) to n }, -1, null) { index ->
            val name = sets[index]
            AlertDialog.Builder(context)
                .setTitle("LOAD  „$name“")
                .setMessage("Целият ЛИСТ, блендите и настройките се сменят с тези от сета.\nСегашните (ако не са записани със SAVE AS) се губят.")
                .setPositiveButton("LOAD") { _, _ ->
                    val err = sampler.loadSavedSet(name)
                    if (err == null) {
                        loadedFromList = -1
                        listSelected = -1
                        refreshListCache()
                        refreshAllUppers()
                        refreshFx()
                        refreshGlobal()
                        flash("ЗАРЕДЕН СЕТ: $name")
                    } else flash("ГРЕШКА: $err")
                }
                .setNegativeButton("CANCEL", null)
                .show()
        }
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
        val favLabel = if (index in listFavs) "☆ МАХНИ ОТ ЛЮБИМИ" else "★ ЛЮБИМА"
        val actions = arrayOf("EDIT COMBI", favLabel, "RENAME", "COPY", "MOVE", "DELETE")
        AlertDialog.Builder(context)
            .setTitle("%03d   %s".format(index + 1, item.name))
            .setItems(actions) { dialog, which ->
                when (actions[which]) {
                    "EDIT COMBI" -> editListCombi(item, index)
                    favLabel -> { if (!listFavs.remove(index)) listFavs.add(index); sampler.saveListFavourites(listFavs); invalidate() }
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
        listPage = (index / LIST_PER_PAGE).coerceIn(0, LIST_PAGES - 1)
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
        listPage = (to / LIST_PER_PAGE).coerceIn(0, LIST_PAGES - 1)
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
        // SILVETON_FX_V3: FX1..FX4 (common sends) and INSERT of every Upper; one big board.
        val names = (1..4).map { "FX$it" } + (1..7).map { "INS U$it" }
        val kw = (1214f - 10f * 6f) / 11f
        for (k in 0 until 11) {
            val r = RectF(112f + k * (kw + 6f), 108f, 112f + k * (kw + 6f) + kw, 146f)
            val t = fxP(k).getOrElse(0) { 0f }.toInt()
            stripToggle(c, r, names[k] + if (t != 0) " ●" else "", fxUnitFocus == k, C_AMBER, size = 11f)
            hit(r, { _, _ -> fxUnitFocus = k })
        }
        drawFxBoardV3(c, RectF(112f, 154f, 1326f, 722f), fxUnitFocus)
        val fx = fxState ?: sampler.combiFxState()?.also { fxState = it }
        val route = RectF(112f, 734f, 1326f, 786f)
        val routing = fx?.routing ?: 0
        lightKey(c, route, if (routing == 0) "ROUTING:   FX1 + FX2   ПАРАЛЕЛНО" else "ROUTING:   FX1 → FX2   ПОСЛЕДОВАТЕЛНО", size = 14f)
        hit(route, { _, _ -> val next = if (routing == 0) 1 else 0; if (sampler.updateCombiFxRouting(next)) refreshFx() },
            { if (sampler.updateCombiFxRouting(0)) refreshFx() })
    }

    private fun reverbSeconds(feedback: Float): Float = 0.6f * Math.pow(20.0, (feedback.coerceIn(0f, 0.95f) / 0.95f).toDouble()).toFloat()
    private fun reverbFeedbackFor(seconds: Float): Float =
        (0.95f * (kotlin.math.ln(seconds.coerceIn(0.6f, 12f) / 0.6f) / kotlin.math.ln(20f))).coerceIn(0f, 0.95f)

    // One knob of an effect: id into the FX params, label, min, max, step, how it is shown.
    private class FxKnob(val id: Int, val label: String, val minV: Float, val maxV: Float, val step: Float, val def: Float, val show: (Float) -> String, val decimals: Int = 0)

    private fun fxKnobsFor(type: Int): List<FxKnob> {
        val pct = { v: Float -> "${"%.2f".format(v * 100f)} %" }
        val ms = { v: Float -> if (v < 1000f) "${round(v).toInt()} ms" else "%.2f s".format(v / 1000f) }
        val hz = { v: Float -> "%.2f Hz".format(v) }
        val wet = FxKnob(1, "WET", 0f, 1f, 0.01f, 0.3f, pct)
        val reverb = listOf(wet, FxKnob(7, "PRE-DELAY", 0f, 200f, 1f, 10f, ms), FxKnob(3, "DECAY", 0f, 0.95f, 0.005f, 0.6f, { "%.2f s".format(reverbSeconds(it)) }, 1),
            FxKnob(6, "TONE", 0f, 1f, 0.01f, 0.6f, pct), FxKnob(8, "DAMPING", 0f, 1f, 0.01f, 0.4f, pct), FxKnob(9, "SIZE", 0f, 1f, 0.01f, 0.5f, pct), FxKnob(10, "MOD", 0f, 1f, 0.01f, 0.3f, pct))
        val delay = listOf(wet, FxKnob(2, "TIME", 1f, 2500f, 1f, 320f, ms), FxKnob(3, "FEEDBACK", 0f, 0.95f, 0.005f, 0.25f, pct), FxKnob(6, "TONE", 0f, 1f, 0.01f, 0.5f, pct))
        return when (type) {
            1 -> delay
            8 -> delay + listOf(FxKnob(10, "WOW/FLUTTER", 0f, 1f, 0.01f, 0.3f, pct), FxKnob(5, "SATURATION", 0f, 1f, 0.01f, 0.35f, pct))
            9 -> delay
            10 -> delay + listOf(FxKnob(5, "SPREAD", 0f, 1f, 0.01f, 0.35f, pct))
            11 -> delay + listOf(FxKnob(5, "DUCKING", 0f, 1f, 0.01f, 0.6f, pct))
            2 -> listOf(wet, FxKnob(4, "RATE", 0.02f, 8f, 0.01f, 0.8f, hz, 2), FxKnob(5, "DEPTH", 0f, 1f, 0.01f, 0.35f, pct), FxKnob(6, "TONE", 0f, 1f, 0.01f, 0.5f, pct), FxKnob(14, "VOICES", 2f, 4f, 1f, 3f, { "${it.toInt()}" }))
            12 -> listOf(wet, FxKnob(4, "RATE", 0.02f, 8f, 0.01f, 0.3f, hz, 2), FxKnob(5, "DEPTH", 0f, 1f, 0.01f, 0.5f, pct), FxKnob(3, "FEEDBACK", -0.95f, 0.95f, 0.01f, 0.4f, pct))
            17 -> listOf(wet, FxKnob(4, "SPEED", 0.1f, 2f, 0.01f, 0.6f, hz, 2), FxKnob(5, "DEPTH", 0f, 1f, 0.01f, 0.7f, pct))
            13 -> listOf(wet, FxKnob(4, "RATE", 0.02f, 8f, 0.01f, 0.4f, hz, 2), FxKnob(5, "DEPTH", 0f, 1f, 0.01f, 0.6f, pct), FxKnob(3, "FEEDBACK", 0f, 0.9f, 0.01f, 0.3f, pct), FxKnob(14, "STAGES", 4f, 8f, 2f, 6f, { "${it.toInt()}" }))
            14 -> listOf(wet, FxKnob(4, "SLOW Hz", 0.1f, 2f, 0.01f, 0.8f, hz, 2), FxKnob(14, "FAST Hz", 2f, 10f, 0.1f, 6.5f, hz, 1), FxKnob(5, "DEPTH", 0f, 1f, 0.01f, 0.6f, pct), FxKnob(15, "SPEED", 0f, 1f, 1f, 0f, { if (it >= 0.5f) "FAST" else "SLOW" }))
            15 -> listOf(FxKnob(14, "THRESHOLD", -60f, 0f, 0.5f, -20f, { "%.2f dB".format(it) }, 1), FxKnob(15, "RATIO", 1f, 20f, 0.1f, 4f, { "%.1f : 1".format(it) }, 1), FxKnob(2, "ATTACK", 0.1f, 300f, 0.1f, 10f, ms, 1), FxKnob(16, "RELEASE", 10f, 3000f, 1f, 120f, ms), FxKnob(6, "MAKE-UP", 0f, 24f, 0.1f, 0f, { "%.2f dB".format(it) }, 1))
            16 -> listOf(wet, FxKnob(5, "DRIVE", 0f, 1f, 0.01f, 0.4f, pct), FxKnob(6, "TONE", 0f, 1f, 0.01f, 0.6f, pct), FxKnob(14, "OUTPUT", -24f, 12f, 0.5f, 0f, { "%.2f dB".format(it) }, 1))
            3, 4, 5, 6, 7 -> reverb
            else -> emptyList()
        }
    }

    private fun drawFxBoardV3(c: Canvas, r: RectF, unit: Int) {
        board(c, r)
        val p = fxP(unit)
        val type = p.getOrElse(0) { 0f }.toInt()
        val title = if (unit < 4) "FX${unit + 1}" else "INSERT U${unit - 3}"
        text(c, title, r.left + 22f, r.top + 42f, 24f, C_AMBER, face = faceMonoBold)
        // type keys: two rows of 9 (17 types)
        val typeNames = listOf("OFF", "DIGITAL DLY", "CHORUS", "PLATE", "HALL", "ROOM", "CHAMBER", "SPRING", "TAPE DLY", "PING-PONG", "MULTI-TAP", "DUCK DLY", "FLANGER", "PHASER", "ROTARY", "COMP", "DRIVE", "ENSEMBLE")
        val order = listOf(0, 4, 3, 5, 6, 7, 1, 8, 9, 10, 11, 2, 17, 12, 13, 14, 15, 16)
        val tx0 = r.left + 200f
        val tw = (r.right - 20f - tx0 - 8f * 6f) / 9f
        order.forEachIndexed { k, t ->
            val row = k / 9; val col = k % 9
            val b = RectF(tx0 + col * (tw + 6f), r.top + 14f + row * 40f, tx0 + col * (tw + 6f) + tw, r.top + 48f + row * 40f)
            segKey(c, b, typeNames[t], type == t, size = 11f)
            hit(b, { _, _ -> setFxP(unit, 0, t.toFloat()) })
        }
        val sec = RectF(r.left + 16f, r.top + 104f, r.right - 16f, r.bottom - 16f)
        section(c, sec, "ПАРАМЕТРИ · " + effectName(type))
        val knobs = fxKnobsFor(type)
        if (knobs.isEmpty()) {
            text(c, "$title е изключен. Избери ефект отгоре.", sec.centerX(), sec.top + 160f, 16f, C_KNOB_LABEL, center = true, maxWidth = sec.width() - 40f)
            return
        }
        val n = knobs.size
        knobs.forEachIndexed { k, kn ->
            val cx = sec.left + sec.width() * (k + 0.5f) / n
            val cy = sec.top + 150f
            val v = p.getOrElse(kn.id) { kn.def }
            val apply = { nv: Float -> setFxP(unit, kn.id, if (kn.step >= 1f) round(nv / kn.step) * kn.step else nv) }
            knobControl(c, cx, cy, 40f, v, kn.minV, kn.maxV, kn.step, 300f, kn.minV < 0f && kn.maxV > 0f, kn.show(v), kn.label,
                hitW = min(200f, sec.width() / n - 10f),
                onChange = { nv -> apply(nv) },
                exact = {
                    if (kn.id == 3 && type in listOf(3, 4, 5, 6, 7)) showNumber("$title DECAY s", reverbSeconds(v), 0.6f, 12f, 1, restore = { apply(v) }) { apply(reverbFeedbackFor(it)) }
                    else if (kn.maxV <= 1f && kn.minV >= -1f && kn.step < 1f) showNumber("$title ${kn.label} %", v * 100f, kn.minV * 100f, kn.maxV * 100f, 0, restore = { apply(v) }) { apply((it / 100f).coerceIn(kn.minV, kn.maxV)) }
                    else showNumber("$title ${kn.label}", v, kn.minV, kn.maxV, kn.decimals) { apply(it) }
                },
                reset = { apply(kn.def) })
        }
        // delays: tempo sync keys
        if (type in listOf(1, 8, 9, 10, 11)) {
            val sync = p.getOrElse(11) { 0f } >= 0.5f
            val y = sec.top + 300f
            val sr = RectF(sec.left + 16f, y, sec.left + 176f, y + 44f)
            stripToggle(c, sr, "TEMPO SYNC", sync, C_AMBER, size = 12f); hit(sr, { _, _ -> setFxP(unit, 11, if (sync) 0f else 1f) })
            val bpm = p.getOrElse(12) { 120f }
            val br = RectF(sec.left + 188f, y, sec.left + 348f, y + 44f)
            lightKey(c, br, "${bpm.toInt()} BPM", enabled = sync, size = 13f)
            hit(br, { _, _ -> showNumber("TEMPO (BPM)", bpm, 30f, 300f, 0) { setFxP(unit, 12, round(it)) } })
            val tapR = RectF(sec.left + 360f, y, sec.left + 470f, y + 44f)
            lightKey(c, tapR, "TAP", enabled = sync, size = 13f)
            hit(tapR, { _, _ -> tapTempo(unit) })
            val divs = listOf(0.25f to "1/4", 0.1875f to "1/8.", 0.125f to "1/8", 0.0833333f to "1/8T", 0.0625f to "1/16", 0.5f to "1/2")
            val cur = p.getOrElse(13) { 0.25f }
            divs.forEachIndexed { k, (d, label) ->
                val dr = RectF(sec.left + 490f + k * 96f, y, sec.left + 578f + k * 96f, y + 44f)
                stripToggle(c, dr, label, sync && abs(cur - d) < 0.002f, C_AMBER, size = 12f)
                hit(dr, { _, _ -> setFxP(unit, 13, d) })
            }
        }
        if (unit < 4) {
            val slot = unit + 1
            val sends = (0 until 7).mapNotNull { uu ->
                val st = upperStates[uu] ?: return@mapNotNull null
                val send = when (slot) { 1 -> st.fxSend1; 2 -> st.fxSend2; 3 -> uParams(uu).getOrElse(46) { 0f }; else -> uParams(uu).getOrElse(47) { 0f } }
                if (send > 0.004f && st.soundName != "EMPTY" && st.enabled) "U${uu + 1} ${round(send * 100f).toInt()}%" else null
            }
            val y = sec.bottom - 60f
            text(c, "ПРАЩАТ КЪМ FX$slot", sec.left + 16f, y, 11f, C_SECTION_TITLE, spacing = 0.25f)
            text(c, if (sends.isEmpty()) "Никой UPPER не праща тук – вдигни FX$slot SEND (КОМБИ → FX)" else sends.joinToString("   "),
                sec.left + 16f, y + 28f, 15f, if (sends.isEmpty()) C_AMBER else C_LCD_OK, face = faceMonoBold, maxWidth = sec.width() - 32f)
        }
    }

    private var tapTimes = ArrayList<Long>()
    private fun tapTempo(unit: Int) {
        val now = android.os.SystemClock.uptimeMillis()
        if (tapTimes.isNotEmpty() && now - tapTimes.last() > 2000L) tapTimes.clear()
        tapTimes.add(now)
        if (tapTimes.size >= 2) {
            val avg = (tapTimes.last() - tapTimes.first()).toFloat() / (tapTimes.size - 1)
            setFxP(unit, 12, round(60000f / avg).coerceIn(30f, 300f)); setFxP(unit, 11, 1f)
        }
        if (tapTimes.size > 8) tapTimes.removeAt(0)
    }

    // ------------------------------------------------------------------ SETTINGS

    private fun drawSettings(c: Canvas) {
        // SILVETON_GUI_V3: ОБЩИ / MIDI / SCALE · ТЕРЦА
        val tabs = arrayOf("ОБЩИ", "MIDI", "INFO")
        for (t in tabs.indices) {
            val tr = RectF(112f + t * 226f, 108f, 332f + t * 226f, 146f)
            stripToggle(c, tr, tabs[t], settingsTab == t, C_AMBER, size = 14f)
            hit(tr, { _, _ -> settingsTab = t; if (t == 2) bankInfoCache = sampler.bankInfo() })
        }
        when (settingsTab) {
            1 -> { drawMidiSettings(c, RectF(112f, 154f, 1326f, 786f)); return }
            2 -> { drawBankInfo(c, RectF(112f, 154f, 1326f, 786f)); return }
        }
        val g = globalState ?: sampler.globalUiState()?.also { globalState = it } ?: return
        val b = RectF(112f, 154f, 1326f, 786f)
        board(c, b)
        val labels = arrayOf("MASTER TUNE", "TRANSPOSE", "MASTER GAIN", "EQ LOW", "EQ MID", "EQ HIGH", "LIMITER", "CEILING",
            "VELOCITY CURVE", "BREATH CC", "EXPRESSION CC", "DAMPER CC", "PITCH BEND", "AFTERTOUCH", "ТЕМА")   // SILVETON_THEME_V1
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
                14 -> {   // SILVETON_THEME_V1: СВЕТЛА / ТЪМНА
                    val a = RectF(rr.left + 14f, rr.top + 56f, cx - 4f, rr.top + 146f)
                    val bb = RectF(cx + 4f, rr.top + 56f, rr.right - 14f, rr.top + 146f)
                    stripToggle(c, a, "СВЕТЛА", !darkTheme, C_AMBER, size = 13f)
                    stripToggle(c, bb, "ТЪМНА", darkTheme, C_AMBER, size = 13f)
                    hit(a, { _, _ -> sampler.setThemeDark(false); applyTheme(false); woodBitmap = null; invalidate() })
                    hit(bb, { _, _ -> sampler.setThemeDark(true); applyTheme(true); invalidate() })
                }
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

    // SILVETON_BANK_INFO_V1: what is in SILVETON_BANK.sv, DELETE of whole SOUNDs.
    private fun mb(bytes: Long): String = "%.1f MB".format(bytes / 1048576.0)

    private fun drawBankInfo(c: Canvas, b: RectF) {
        board(c, b)
        val info = bankInfoCache ?: sampler.bankInfo()?.also { bankInfoCache = it }
        val box = RectF(b.left + 24f, b.top + 24f, b.left + 640f, b.top + 300f)
        lcd(c, box)
        if (info == null) {
            text(c, "SILVETON_BANK.sv НЕ Е НАМЕРЕН", box.left + 24f, box.top + 60f, 22f, C_LCD_BAD, face = faceMono)
            return
        }
        val rows = listOf(
            "ЗВУЦИ (SOUND)" to info.sounds.toString(),
            "СЕМПЛИ" to info.samples.toString(),
            "АУДИО" to mb(info.audioBytes),
            "ФАЙЛ" to mb(info.fileBytes))
        rows.forEachIndexed { k, (label, value) ->
            val y = box.top + 56f + k * 56f
            text(c, label, box.left + 24f, y, 14f, C_LCD_DIM, face = faceMono, spacing = 0.15f)
            text(c, value, box.right - 24f, y, 26f, C_LCD, right = true, face = faceMono)
        }
        text(c, info.path, b.left + 24f, box.bottom + 30f, 12f, C_LABEL_GREY, face = faceMono, maxWidth = b.width() - 48f)
        val big = info.list.sortedByDescending { it.bytes }.take(8)
        val side = RectF(b.left + 670f, b.top + 24f, b.right - 24f, b.top + 470f)
        section(c, side, "НАЙ-ГОЛЕМИ ЗВУЦИ")
        big.forEachIndexed { k, snd ->
            val y = side.top + 64f + k * 48f
            text(c, "%03d  %s".format(snd.slot, snd.name), side.left + 20f, y, 17f, C_INK, face = faceBold, maxWidth = side.width() - 190f)
            text(c, mb(snd.bytes), side.right - 20f, y, 17f, C_INK_SOFT, right = true, face = faceMonoBold)
        }
        val delR = RectF(b.left + 24f, b.bottom - 90f, b.left + 520f, b.bottom - 30f)
        accentKey(c, delR, "ИЗБЕРИ ЗВУЦИ ЗА ТРИЕНЕ", true)
        hit(delR, { _, _ -> showDeleteSounds(info) })
        val refR = RectF(delR.right + 16f, b.bottom - 90f, delR.right + 236f, b.bottom - 30f)
        lightKey(c, refR, "ОБНОВИ", size = 15f)
        hit(refR, { _, _ -> bankInfoCache = sampler.bankInfo(); invalidate() })
    }

    private fun showDeleteSounds(info: SamplerAppController.BankInfo) {
        if (info.list.isEmpty()) return
        val labels = info.list.map { "%03d   %s   ·   %d семпъла   ·   %s".format(it.slot, it.name, it.samples, mb(it.bytes)) }.toTypedArray()
        val checked = BooleanArray(labels.size)
        AlertDialog.Builder(context)
            .setTitle("ТРИЕНЕ НА ЗВУЦИ ОТ БАНКАТА")
            .setMultiChoiceItems(labels, checked) { _, which, isChecked -> checked[which] = isChecked }
            .setPositiveButton("ИЗТРИЙ") { _, _ ->
                val chosen = info.list.filterIndexed { k, _ -> checked[k] }
                if (chosen.isEmpty()) return@setPositiveButton
                val freed = chosen.sumOf { it.bytes }
                AlertDialog.Builder(context)
                    .setTitle("ИЗТРИВАНЕ НА ${chosen.size} ЗВУКА")
                    .setMessage("Звуците се трият от SILVETON_BANK.sv завинаги (заедно със семплите, които само те ползват, до ${mb(freed)}).\n" +
                        "Блендите, които ги ползват, остават без тях.\nДа изтрия ли?")
                    .setPositiveButton("ДА, ИЗТРИЙ") { _, _ -> runDeleteSounds(chosen.map { it.slot }) }
                    .setNegativeButton("НЕ", null)
                    .show()
            }
            .setNegativeButton("CANCEL", null)
            .show()
    }

    private fun runDeleteSounds(slots: List<Int>) {
        val wait = AlertDialog.Builder(context).setTitle("ТРИЕНЕ…").setMessage("Банката се записва наново. Не изключвай таблета.")
            .setCancelable(false).create()
        wait.show()
        Thread({
            val err = sampler.deleteBankSounds(slots)
            post {
                wait.dismiss()
                bankInfoCache = sampler.bankInfo()
                cachedCombiName = sampler.combiName()
                refreshAllUppers()
                refreshListCache()
                flash(if (err == null) "ИЗТРИТИ: ${slots.size} ЗВУКА" else "ГРЕШКА: $err")
            }
        }, "SilvetonBankDelete").start()
    }

    private fun globalStep(i: Int): Float = when (i) { 1, 8, 9, 10, 11 -> 1f; else -> 0.1f }
    private fun globalTravel(i: Int): Float = when (i) {
        0 -> 400f; 1 -> 240f; 2 -> 390f; 3, 4, 5 -> 360f; 7 -> 240f; 8 -> 160f; else -> 508f
    }

    private fun editGlobal(i: Int, g: SamplerAppController.GlobalUiState) {
        when (i) {
            0 -> showNumber("Master Tune Hz", g.masterTuneHz, 400f, 480f, 1) { commitGlobal(g.copy(masterTuneHz = it)) }
            1 -> showNumber("Transpose", g.transpose.toFloat(), -12f, 12f, 0) { commitGlobal(g.copy(transpose = it.toInt())) }
            2 -> showNumber("Master Gain dB", g.masterGainDb, -60f, 18f, 1) { commitGlobal(g.copy(masterGainDb = it)) }
            3 -> showNumber("EQ Low dB", g.eqLowDb, -18f, 18f, 1) { commitGlobal(g.copy(eqLowDb = it)) }
            4 -> showNumber("EQ Mid dB", g.eqMidDb, -18f, 18f, 1) { commitGlobal(g.copy(eqMidDb = it)) }
            5 -> showNumber("EQ High dB", g.eqHighDb, -18f, 18f, 1) { commitGlobal(g.copy(eqHighDb = it)) }
            6 -> flash("ЛИМИТЕРЪТ Е ВИНАГИ ВКЛЮЧЕН (ТАВАН -6 dB)")
            7 -> showNumber("Limiter Ceiling", g.limiterCeilingDb.coerceAtMost(-6f), -12f, -6f, 1) { commitGlobal(g.copy(limiterCeilingDb = it)) }
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
            6 -> g.copy(limiterEnabled = true); 7 -> g.copy(limiterCeilingDb = -6f); 8 -> g.copy(velocityCurve = 0)
            9 -> g.copy(breathCc = 2); 10 -> g.copy(expressionCc = 11); 11 -> g.copy(damperCc = 64)
            12 -> g.copy(pitchBendEnabled = true); else -> g.copy(aftertouchEnabled = true)
        }
        commitGlobal(n)
    }

    private fun globalSliderSpec(i: Int, g: SamplerAppController.GlobalUiState): Triple<Float, Float, Float> = when (i) {
        0 -> Triple(g.masterTuneHz, 400f, 480f); 1 -> Triple(g.transpose.toFloat(), -12f, 12f); 2 -> Triple(g.masterGainDb, -60f, 18f)
        3 -> Triple(g.eqLowDb, -18f, 18f); 4 -> Triple(g.eqMidDb, -18f, 18f); 5 -> Triple(g.eqHighDb, -18f, 18f)
        7 -> Triple(g.limiterCeilingDb.coerceAtMost(-6f), -12f, -6f); 8 -> Triple(g.velocityCurve.toFloat(), 0f, 4f)
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
        0 -> "%.2f Hz".format(g.masterTuneHz); 1 -> signed(g.transpose); 2 -> "%.2f dB".format(g.masterGainDb)
        3 -> "%.2f dB".format(g.eqLowDb); 4 -> "%.2f dB".format(g.eqMidDb); 5 -> "%.2f dB".format(g.eqHighDb)
        7 -> "%.2f dB".format(g.limiterCeilingDb); 8 -> g.velocityCurve.toString(); 9 -> "CC ${g.breathCc}"
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
        val items = blends.map { "%03d".format(it.first) to it.second.ifBlank { "COMBI ${it.first}" } }
        // Open on the blend that is loaded now, not on position 1.
        val current = editingCombiSlot?.let { slot -> blends.indexOfFirst { it.first == slot } } ?: -1
        showGridPicker("BLENDS  1–500", items, current, null) { index ->
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
        // Open on this Upper's own sound (e.g. position 53), not on position 1.
        val picked = lastPickedSound.getOrNull(upper)
        val currentName = upperStates.getOrNull(upper)?.soundName ?: sampler.upperState(upper)?.soundName
        var current = -1
        if (picked != null && picked.name == currentName) {
            current = sounds.indexOfFirst { it.bank == picked.bank && it.page == picked.page && it.slot == picked.slot }
        }
        if (current < 0 && currentName != null && currentName != "EMPTY") current = sounds.indexOfFirst { it.name == currentName }
        val items = sounds.map { "%03d".format(it.slot) to it.name }
        showGridPicker("UPPER ${upper + 1} — SOUND", items, current, "EMPTY") { index ->
            if (index < 0) { sampler.clearUpperSound(upper); lastPickedSound[upper] = null }
            else { sampler.assignUpperSound(upper, sounds[index]); lastPickedSound[upper] = sounds[index] }
            refreshUpper(upper); invalidate()
        }
    }

    // SILVETON_GUI_V3: big pages of 20 (4 x 5), page keys on top, opens on the current item.
    // pick(-1) = the extra key (EMPTY) when [extra] is given.
    private fun showGridPicker(title: String, items: List<Pair<String, String>>, current: Int, extra: String?, pick: (Int) -> Unit) {
        val perPage = 20
        val pages = max(1, (items.size + perPage - 1) / perPage)
        var pageNow = if (current >= 0) current / perPage else 0
        val dialog = Dialog(context)
        dialog.requestWindowFeature(Window.FEATURE_NO_TITLE)
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(uiPx(20f), uiPx(16f), uiPx(20f), uiPx(16f))
            background = GradientDrawable().apply {
                shape = GradientDrawable.RECTANGLE
                cornerRadius = uiPx(16f).toFloat()
                setColor(Color.rgb(20, 24, 29))
                setStroke(uiPx(1f), Color.rgb(68, 84, 102))
            }
        }
        val head = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
        head.addView(TextView(context).apply {
            text = title; setTextColor(Color.WHITE); textSize = 22f; setTypeface(typeface, Typeface.BOLD)
        }, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
        if (extra != null) head.addView(pickerKey(extra, false) { dialog.dismiss(); pick(-1) })
        head.addView(pickerKey("CANCEL", false) { dialog.dismiss() })
        root.addView(head)
        val pageRow = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, uiPx(10f), 0, uiPx(10f)) }
        val pageScroll = android.widget.HorizontalScrollView(context).apply { addView(pageRow) }
        root.addView(pageScroll)
        val grid = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        root.addView(grid)
        lateinit var render: () -> Unit
        render = {
            pageRow.removeAllViews()
            for (pg in 0 until pages) {
                pageRow.addView(pickerKey((pg + 1).toString(), pg == pageNow, small = true) { pageNow = pg; render() })
            }
            grid.removeAllViews()
            for (row in 0 until 5) {
                val line = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL }
                for (col in 0 until 4) {
                    val idx = pageNow * perPage + row * 4 + col
                    val lp = LinearLayout.LayoutParams(0, uiPx(64f), 1f).apply { setMargins(uiPx(4f), uiPx(4f), uiPx(4f), uiPx(4f)) }
                    val item = items.getOrNull(idx)
                    if (item == null) {
                        line.addView(View(context), lp)
                    } else {
                        line.addView(pickerKey("${item.first}  ${item.second}", idx == current, tall = true) {
                            dialog.dismiss(); pick(idx)
                        }, lp)
                    }
                }
                grid.addView(line)
            }
        }
        render()
        dialog.setContentView(root)
        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawableResource(android.R.color.transparent)
            setDimAmount(0.76f)
            addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            setLayout((resources.displayMetrics.widthPixels * 0.92f).toInt(), WindowManager.LayoutParams.WRAP_CONTENT)
        }
    }

    private fun pickerKey(label: String, selected: Boolean, small: Boolean = false, tall: Boolean = false, onTap: () -> Unit): TextView =
        TextView(context).apply {
            text = label
            textSize = if (small) 15f else 16f
            setTextColor(if (selected) Color.rgb(20, 20, 20) else Color.WHITE)
            setTypeface(typeface, Typeface.BOLD)
            gravity = if (tall) Gravity.CENTER_VERTICAL or Gravity.START else Gravity.CENTER
            maxLines = 2
            setPadding(uiPx(14f), uiPx(8f), uiPx(14f), uiPx(8f))
            minWidth = uiPx(if (small) 56f else 110f)
            background = GradientDrawable().apply {
                cornerRadius = uiPx(10f).toFloat()
                setColor(if (selected) Color.rgb(255, 150, 50) else Color.rgb(44, 50, 58))
                setStroke(uiPx(1f), Color.rgb(78, 88, 100))
            }
            if (!tall) layoutParams = LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, uiPx(52f)).apply {
                setMargins(uiPx(4f), 0, uiPx(4f), 0)
            }
            setOnClickListener { onTap() }
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
                               onCancel: (() -> Unit)? = null, focus: EditText? = null, onApply: () -> Unit) {
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
            // SILVETON_GUI_V3: the name field takes the keyboard at once (no second tap).
            if (focus != null) setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE)
        }
        if (focus != null) {
            focus.requestFocus()
            focus.selectAll()
            focus.post {
                val imm = context.getSystemService(Context.INPUT_METHOD_SERVICE) as? android.view.inputmethod.InputMethodManager
                imm?.showSoftInput(focus, android.view.inputmethod.InputMethodManager.SHOW_IMPLICIT)
            }
        }
    }

    private fun showTextPage(title: String, current: String, done: (String) -> Unit) {
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("NAME"))
        val edit = editorField(current, false); body.addView(edit)
        showEditorPage(title, "Edit the name and confirm.", body, focus = edit) {
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
    // SILVETON_SIMPLE_NUMBER_V1: simple fine setting - the number, a big − and a big +.
    // Tap = one small step, hold = it keeps going and gets faster. You hear it at once.
    // The number can also be typed. APPLY keeps it, CANCEL goes back.
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
        // SILVETON_FINE_KNOBS_V1: every value can be set to the hundredth; only small
        // whole-number ranges (BALANCE, BEND, MIDI channel ...) stay whole.
        val dec = if (decimals > 0 || maxV - minV >= 50f) 2 else decimals
        fun formatEditorValue(value: Float): String = if (dec == 0) round(value).toInt().toString() else "% .${dec}f".format(value).trim()
        val scale = when { dec <= 0 -> 1; dec == 1 -> 10; else -> 100 }
        val step = 1f / scale.toFloat()
        val edit = editorField(formatEditorValue(current.coerceIn(minV, maxV))).apply {
            textSize = 40f
            gravity = Gravity.CENTER
            setTypeface(typeface, Typeface.BOLD)
        }
        body.addView(edit)
        fun currentTyped(): Float = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
        fun setEditorValue(value: Float) {
            val newText = formatEditorValue(value.coerceIn(minV, maxV))
            edit.setText(newText)
            edit.setSelection(newText.length)
        }
        val handler = android.os.Handler(android.os.Looper.getMainLooper())
        fun stepKey(label: String, dir: Int): TextView = TextView(context).apply {
            text = label
            textSize = 40f
            gravity = Gravity.CENTER
            setTextColor(Color.rgb(255, 179, 71))
            setTypeface(typeface, Typeface.BOLD)
            background = GradientDrawable().apply {
                cornerRadius = uiPx(14f).toFloat()
                setColor(Color.rgb(44, 50, 58))
                setStroke(uiPx(1f), Color.rgb(78, 88, 100))
            }
            var count = 0
            fun once(mult: Float) {
                val v = (currentTyped() + dir * step * mult).coerceIn(minV, maxV)
                setEditorValue(v)
                preview(v)
            }
            val repeat = object : Runnable {
                override fun run() {
                    count++
                    once(when { count > 40 -> 100f; count > 15 -> 10f; else -> 1f })
                    handler.postDelayed(this, 70L)
                }
            }
            setOnTouchListener { view, ev ->
                when (ev.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        count = 0
                        view.alpha = 0.6f
                        once(1f)
                        handler.postDelayed(repeat, 420L)
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        handler.removeCallbacks(repeat)
                        view.alpha = 1f
                        if (ev.actionMasked == MotionEvent.ACTION_UP) view.performClick()
                    }
                }
                true
            }
        }
        val keys = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, uiPx(12f), 0, uiPx(4f)) }
        keys.addView(stepKey("−", -1), LinearLayout.LayoutParams(0, uiPx(84f), 1f).apply { setMargins(0, 0, uiPx(8f), 0) })
        keys.addView(stepKey("+", 1), LinearLayout.LayoutParams(0, uiPx(84f), 1f).apply { setMargins(uiPx(8f), 0, 0, 0) })
        body.addView(keys)
        val subtitle = if (live) "−  /  +  : една стъпка.  Задръж = по-бързо.  Чуваш веднага.   (${formatEditorValue(minV)} … ${formatEditorValue(maxV)})"
            else "${formatEditorValue(minV)} … ${formatEditorValue(maxV)}"
        showEditorPage(title, subtitle, body, onCancel = {
            handler.removeCallbacksAndMessages(null)
            if (previewed) {
                if (restore != null) restore() else done(current.coerceIn(minV, maxV))
                invalidate()
            }
        }) {
            handler.removeCallbacksAndMessages(null)
            val value = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
            done(value)
            invalidate()
        }
    }

    // ------------------------------------------------------------------ SILVETON_GUI_V3 helpers

    /** Title, value window and two big keys - / +. Both keys at the same time = onReset. */
    private fun stepper(c: Canvas, r: RectF, title: String, value: String,
                        onMinus: () -> Unit, onPlus: () -> Unit, onReset: () -> Unit) {
        text(c, title, r.centerX(), r.top + 14f, 12f, C_KNOB_LABEL, center = true, face = faceBold, spacing = 0.12f, maxWidth = r.width())
        val box = RectF(r.left + 30f, r.top + 26f, r.right - 30f, r.top + 74f)
        lcd(c, box, 8f)
        text(c, value, box.centerX(), box.centerY() + 10f, 28f, C_LCD, center = true, face = faceMono)
        hit(box, { _, _ -> }, onReset)   // double tap on the value = 0
        val kw = (r.width() - 12f) / 2f
        val minus = RectF(r.left, r.top + 86f, r.left + kw, r.bottom)
        val plus = RectF(minus.right + 12f, r.top + 86f, r.right, r.bottom)
        stripToggle(c, minus, "−", false, C_AMBER, size = 34f)
        stripToggle(c, plus, "+", false, C_AMBER, size = 34f)
        hit(minus, { _, _ -> onMinus(); invalidate() })
        hit(plus, { _, _ -> onPlus(); invalidate() })
        pairHits.add(PairHit(minus, plus) { onReset(); invalidate() })
    }

    private fun pasteUpper(target: Int) {
        val src = copiedUpper ?: return
        val cur = upperStates[target] ?: sampler.upperState(target) ?: return
        // Everything from portamento to the effects; the target keeps its own sound,
        // ON / SOLO / MUTE, MIDI channel and keyboard / velocity zone.
        val merged = src.copy(
            enabled = cur.enabled, solo = cur.solo, mute = cur.mute, soundName = cur.soundName,
            midiChannel = cur.midiChannel, keyLow = cur.keyLow, keyHigh = cur.keyHigh,
            velocityLow = cur.velocityLow, velocityHigh = cur.velocityHigh
        )
        sampler.updateUpperMain(target, merged)
        sampler.updateUpperTune(target, merged)
        sampler.updateUpperControl(target, merged)
        sampler.updateUpperTone(target, merged)
        sampler.updateUpperFx(target, merged)
        val tone = floatArrayOf(src.toneAttackMs, src.toneDecayMs, src.toneSustain, src.toneReleaseMs, src.toneCutoffHz, src.toneResonance)
        for (k in tone.indices) sampler.setUpperTone(target, k, tone[k])
        refreshUpper(target)
        flash("ПОСТАВЕНО: U${copiedFrom + 1} → U${target + 1}")
        invalidate()
    }

    private fun scanKey(c: Canvas, r: RectF, mode: Int, label: String) {
        val on = scanMode == mode
        val blink = on && (android.os.SystemClock.uptimeMillis() / 300L) % 2L == 0L
        val lines = (if (on) "SCAN…\nНАТИСНИ НОТИТЕ" else label).split("\n")
        stripToggle(c, r, "", on, if (blink) C_ORANGE else C_AMBER, size = 13f)
        lines.forEachIndexed { k, ln ->
            text(c, ln, r.centerX(), r.centerY() + 5f + (k - (lines.size - 1) / 2f) * 18f, 13f,
                if (on) (if (blink) C_ORANGE else C_AMBER) else C_TOGGLE_TEXT, center = true, face = faceBold, maxWidth = r.width() - 6f)
        }
        hit(r, { _, _ ->
            if (scanMode == mode) sampler.cancelScan()
            else if (mode == 1) sampler.startTercaScan() else sampler.startScaleScan()
            scanMode = sampler.pollScan()
            invalidate()
        })
    }

    private fun tercaText(t: SamplerAppController.TercaState): String =
        if (t.root in 0..11 && t.cell in 0..5)
            "${SamplerAppController.NOTE_NAMES[t.root]} ${SamplerAppController.TERCA_CELLS[t.cell]}"
        else "—"

    private fun cellKey(c: Canvas, r: RectF, title: String, value: String, on: Boolean? = null, tap: () -> Unit) {
        if (on != null) {
            stripToggle(c, r, "$title  ${if (on) "ON" else "OFF"}", on, C_AMBER, size = 13f)
        } else {
            fill(c, r, 10f, C_STRIP)
            text(c, title, r.left + 12f, r.top + 18f, 11f, C_LABEL_GREY, face = faceBold, spacing = 0.1f, maxWidth = r.width() - 20f)
            text(c, value, r.left + 12f, r.bottom - 12f, 19f, C_INK, face = faceMonoBold, maxWidth = r.width() - 20f)
        }
        hit(r, { _, _ -> tap(); invalidate() })
    }

    /** SILVETON_MIDI_FULL_V1: every MIDI setting of the sampler on one page. */
    private fun drawMidiSettings(c: Canvas, b: RectF) {
        board(c, b)
        val g = globalState ?: sampler.globalUiState()?.also { globalState = it } ?: return
        val a = sampler.globalAdvanced() ?: return
        val L = b.left + 16f
        val W = b.width() - 32f
        // Row 1: MIDI channel of each Upper
        text(c, "MIDI КАНАЛ НА ВСЕКИ UPPER", L, b.top + 26f, 12f, C_AMBER, face = faceBold, spacing = 0.12f)
        val uw = (W - 6f * 8f) / 7f
        for (i in 0 until 7) {
            val st = upperStates[i] ?: sampler.upperState(i)?.also { upperStates[i] = it } ?: continue
            val r = RectF(L + i * (uw + 8f), b.top + 36f, L + i * (uw + 8f) + uw, b.top + 96f)
            cellKey(c, r, "UPPER ${i + 1}", "CH ${st.midiChannel + 1}") {
                showNumber("UPPER ${i + 1} MIDI КАНАЛ", (st.midiChannel + 1).toFloat(), 1f, 16f, 0, live = false) { nv ->
                    val cur = upperStates[i] ?: st
                    val next = cur.copy(midiChannel = round(nv).toInt().coerceIn(1, 16) - 1)
                    upperStates[i] = next
                    if (!sampler.updateUpperControl(i, next)) refreshUpper(i)
                }
            }
        }
        // Row 2: MIDI IN channels
        text(c, "MIDI IN - КОИ КАНАЛИ СЕ ПРИЕМАТ", L, b.top + 124f, 12f, C_AMBER, face = faceBold, spacing = 0.12f)
        val cw = (W - 15f * 6f) / 16f
        for (ch in 0 until 16) {
            val r = RectF(L + ch * (cw + 6f), b.top + 134f, L + ch * (cw + 6f) + cw, b.top + 178f)
            val on = (a.midiInMask and (1 shl ch)) != 0
            stripToggle(c, r, (ch + 1).toString(), on, C_AMBER, size = 15f)
            hit(r, { _, _ -> sampler.updateGlobalAdvanced(a.copy(midiInMask = a.midiInMask xor (1 shl ch))); invalidate() })
        }
        // Row 3: filters + damper polarity
        text(c, "ФИЛТРИ (ON = СЕ ИГНОРИРА)", L, b.top + 206f, 12f, C_AMBER, face = faceBold, spacing = 0.12f)
        val fw = (W - 4f * 8f) / 5f
        val filters = listOf(
            Triple("НОТИ", a.filterNotes) { sampler.updateGlobalAdvanced(a.copy(filterNotes = !a.filterNotes)) },
            Triple("CC", a.filterCc) { sampler.updateGlobalAdvanced(a.copy(filterCc = !a.filterCc)) },
            Triple("PITCH BEND", a.filterBend) { sampler.updateGlobalAdvanced(a.copy(filterBend = !a.filterBend)) },
            Triple("AFTERTOUCH", a.filterAftertouch) { sampler.updateGlobalAdvanced(a.copy(filterAftertouch = !a.filterAftertouch)) },
            Triple("ПЕДАЛ ОБЪРНАТ", a.damperReversed) { sampler.updateGlobalAdvanced(a.copy(damperReversed = !a.damperReversed)) }
        )
        filters.forEachIndexed { k, f ->
            val r = RectF(L + k * (fw + 8f), b.top + 216f, L + k * (fw + 8f) + fw, b.top + 266f)
            cellKey(c, r, f.first, "", on = f.second) { f.third() }
        }
        // Rows 4/5: BREATH, EXPRESSION, DAMPER, VELOCITY, BEND, AFTERTOUCH
        text(c, "КОНТРОЛЕРИ", L, b.top + 294f, 12f, C_AMBER, face = faceBold, spacing = 0.12f)
        val kw = (W - 4f * 8f) / 5f
        fun row(y: Float, k: Int) = RectF(L + k * (kw + 8f), y, L + k * (kw + 8f) + kw, y + 62f)
        val y1 = b.top + 304f
        val y2 = b.top + 374f
        val y3 = b.top + 444f
        cellKey(c, row(y1, 0), "BREATH CC", "CC ${g.breathCc}") { editGlobal(9, g) }
        cellKey(c, row(y1, 1), "BREATH MIN", "%.2f %%".format(a.breathMin * 100f)) {
            showNumber("BREATH MIN %", a.breathMin * 100f, 0f, 99f, 0, live = false) { sampler.updateGlobalAdvanced(a.copy(breathMin = it / 100f)) } }
        cellKey(c, row(y1, 2), "BREATH MAX", "%.2f %%".format(a.breathMax * 100f)) {
            showNumber("BREATH MAX %", a.breathMax * 100f, 1f, 100f, 0, live = false) { sampler.updateGlobalAdvanced(a.copy(breathMax = it / 100f)) } }
        cellKey(c, row(y1, 3), "BREATH CURVE", "%.2f".format(a.breathCurve)) {
            showNumber("BREATH CURVE", a.breathCurve, 0.1f, 8f, 2, live = false) { sampler.updateGlobalAdvanced(a.copy(breathCurve = it)) } }
        cellKey(c, row(y1, 4), "VELOCITY CURVE", g.velocityCurve.toString()) { editGlobal(8, g) }
        cellKey(c, row(y2, 0), "EXPRESSION CC", "CC ${g.expressionCc}") { editGlobal(10, g) }
        cellKey(c, row(y2, 1), "EXPRESSION MIN", "%.2f %%".format(a.expressionMin * 100f)) {
            showNumber("EXPRESSION MIN %", a.expressionMin * 100f, 0f, 99f, 0, live = false) { sampler.updateGlobalAdvanced(a.copy(expressionMin = it / 100f)) } }
        cellKey(c, row(y2, 2), "EXPRESSION MAX", "%.2f %%".format(a.expressionMax * 100f)) {
            showNumber("EXPRESSION MAX %", a.expressionMax * 100f, 1f, 100f, 0, live = false) { sampler.updateGlobalAdvanced(a.copy(expressionMax = it / 100f)) } }
        cellKey(c, row(y2, 3), "EXPRESSION CURVE", "%.2f".format(a.expressionCurve)) {
            showNumber("EXPRESSION CURVE", a.expressionCurve, 0.1f, 8f, 2, live = false) { sampler.updateGlobalAdvanced(a.copy(expressionCurve = it)) } }
        cellKey(c, row(y2, 4), "DAMPER CC", "CC ${g.damperCc}") { editGlobal(11, g) }
        cellKey(c, row(y3, 0), "PITCH BEND", "", on = g.pitchBendEnabled) { editGlobal(12, g) }
        cellKey(c, row(y3, 1), "AFTERTOUCH", "", on = g.aftertouchEnabled) { editGlobal(13, g) }
        text(c, "Контролерите на всеки Upper (BREATH / EXPRESSION / AFTERTOUCH / ПЕДАЛ) са в КОМБИ → CONTROL.",
            L, b.bottom - 16f, 12f, C_INK_SOFT, maxWidth = W)
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

    private fun refreshUpper(i: Int) { upperStates[i] = sampler.upperState(i); dropParamCaches(i) }
    private fun refreshAllUppers() { for (i in 0 until 7) refreshUpper(i) }
    private fun refreshFx() { fxState = sampler.combiFxState(); for (k in 0 until 11) fxParamsCache[k] = null }
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
        if (darkTheme) {   // SILVETON_THEME_V1: lit = on
            if (on) { fill(c, r, 12f, C_KEY_ON); litGlow(c, r, 12f, C_LOGO) } else fillV(c, r, 12f, Color.rgb(72, 76, 86), Color.rgb(58, 62, 70))
        } else {
            fillV(c, r, 12f, if (on) Color.rgb(66, 69, 75) else Color.rgb(50, 53, 58), if (on) Color.rgb(54, 57, 63) else Color.rgb(41, 44, 49))
        }
        line(c, r.left + 10f, r.top + 1f, r.right - 10f, r.top + 1f, C_NAV_EDGE, 1f)
        if (isDown(r)) fill(c, r, 12f, Color.argb(70, 0, 0, 0))
    }

    // SILVETON_THEME_V1: a soft inner glow of a lit key (no blur filters: a few translucent rings)
    private fun litGlow(c: Canvas, r: RectF, radius: Float, color: Int) {
        for (k in 0 until 4) {
            val inset = 2f + k * 3f
            fill(c, RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset), max(2f, radius - inset), colorWithAlpha(color, 14 + k * 6))
        }
    }

    private fun lightKey(c: Canvas, r: RectF, label: String, enabled: Boolean = true, size: Float = 12f, radius: Float = 8f) {
        val down = enabled && isDown(r)
        if (darkTheme) {   // SILVETON_THEME_V1: matte dark key with a light top edge
            if (!down) shadow(c, r, radius, 3f, 120)
            fill(c, r, radius, C_KEY_LIGHT_EDGE)
            val face = RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f)
            fillV(c, face, radius, if (down) Color.rgb(52, 56, 64) else Color.rgb(74, 78, 88), if (down) Color.rgb(46, 50, 58) else Color.rgb(60, 64, 72))
            if (!down) line(c, face.left + radius, face.top + 1f, face.right - radius, face.top + 1f, C_KEY_DARK_EDGE, 1f)
            text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (enabled) C_KEY_DARK_TEXT else C_EMPTY_TEXT,
                center = true, face = faceBold, spacing = 0.1f, maxWidth = r.width() - 12f)
            return
        }
        if (!down) shadow(c, r, radius, 4f, 90)   // SILVETON_LOOK_V2: relief
        fill(c, r, radius, Color.rgb(150, 154, 162))
        val face = RectF(r.left + 1f, r.top + if (down) 2f else 1f, r.right - 1f, r.bottom - if (down) 1f else 3f)
        if (!enabled) fill(c, face, radius - 1f, Color.rgb(214, 216, 220))
        else if (down) fill(c, face, radius - 1f, Color.rgb(205, 208, 214))
        else fillV(c, face, radius - 1f, Color.rgb(248, 249, 251), Color.rgb(214, 216, 221))
        if (enabled && !down) line(c, face.left + radius, face.top + 1f, face.right - radius, face.top + 1f, Color.WHITE, 1.5f)
        text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (enabled) C_KEY_TEXT else C_EMPTY_TEXT,
            center = true, face = faceBold, spacing = 0.1f, maxWidth = r.width() - 12f)
    }

    private fun darkKey(c: Canvas, r: RectF, label: String) {
        val down = isDown(r)
        fill(c, r, 6f, Color.BLACK)
        fill(c, RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f), 6f, if (down) Color.rgb(28, 30, 34) else C_DARK_KEY)
        text(c, label, r.centerX(), r.centerY() + 4f, 11f, C_AMBER, center = true, face = faceMonoBold, spacing = 0.06f, maxWidth = r.width() - 8f)
    }

    private fun stripToggle(c: Canvas, r: RectF, label: String, on: Boolean, onColor: Int, size: Float = 11f) {
        val down = isDown(r)
        if (darkTheme) {   // SILVETON_THEME_V1: lit = on, dark matte = off
            if (!down) shadow(c, r, 7f, 3f, 120)
            if (on) {
                fill(c, r, 7f, Color.rgb(8, 9, 10))
                val face = RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f)
                fill(c, face, 7f, C_KEY_ON)
                litGlow(c, face, 7f, onColor)
                line(c, face.left + 7f, face.top + 1f, face.right - 7f, face.top + 1f, Color.rgb(70, 74, 84), 1f)
            } else {
                fill(c, r, 7f, Color.rgb(16, 17, 20))
                val face = RectF(r.left, r.top, r.right, r.bottom - if (down) 0.5f else 2f)
                fillV(c, face, 7f, if (down) Color.rgb(52, 56, 64) else Color.rgb(74, 78, 88), if (down) Color.rgb(46, 50, 58) else Color.rgb(60, 64, 72))
                if (!down) line(c, face.left + 7f, face.top + 1f, face.right - 7f, face.top + 1f, C_KEY_DARK_EDGE, 1f)
            }
            text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (on) onColor else C_KEY_DARK_TEXT, center = true, face = faceBold,
                maxWidth = r.width() - 4f)
            return
        }
        if (!down) shadow(c, r, 7f, 3f, 80)   // SILVETON_LOOK_V2: relief keys, lit keys glow
        if (on) {
            fill(c, r, 7f, Color.rgb(12, 12, 14))
            val face = RectF(r.left + 1f, r.top + 1f, r.right - 1f, r.bottom - if (down) 1f else 3f)
            fillV(c, face, 6f, Color.rgb(58, 61, 67), Color.rgb(30, 32, 36))
            line(c, face.left + 6f, face.top + 1f, face.right - 6f, face.top + 1f, Color.rgb(96, 100, 108), 1f)
        } else {
            fill(c, r, 7f, Color.rgb(150, 154, 162))
            val face = RectF(r.left + 1f, r.top + if (down) 2f else 1f, r.right - 1f, r.bottom - if (down) 1f else 3f)
            if (down) fill(c, face, 6f, Color.rgb(205, 208, 214)) else fillV(c, face, 6f, Color.rgb(246, 247, 249), Color.rgb(214, 216, 221))
            if (!down) line(c, face.left + 6f, face.top + 1f, face.right - 6f, face.top + 1f, Color.WHITE, 1.5f)
        }
        text(c, label, r.centerX(), r.centerY() + size * 0.36f, size, if (on) onColor else C_TOGGLE_TEXT, center = true, face = faceBold,
            maxWidth = r.width() - 4f)
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
        if (darkTheme) {   // SILVETON_THEME_V1: the display's own warm light on the panel
            for (k in 3 downTo 1) fill(c, RectF(r.left - k * 3f, r.top - k * 3f, r.right + k * 3f, r.bottom + k * 3f), radius + k * 3f, Color.argb(10, 255, 160, 40))
        } else fill(c, RectF(r.left, r.top + 1f, r.right, r.bottom + 1f), radius, Color.argb(150, 255, 255, 255))
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

    // SILVETON_THEME_V1: the title of a sound page - left in the light theme, right in the dark one
    // (there the knob names are printed above the knobs and would collide with it).
    private fun secTitle(c: Canvas, sec: RectF, title: String) {
        if (darkTheme) text(c, title, sec.right - 6f, sec.top + 12f, 11f, C_SECTION_TITLE, right = true, face = faceBold, spacing = 0.25f, maxWidth = sec.width() * 0.5f)
        else text(c, title, sec.left + 6f, sec.top + 12f, 11f, C_SECTION_TITLE, spacing = 0.25f)
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
        if (label.isNotEmpty()) {
            if (darkTheme) text(c, label, cx, cy - radius * 1.42f - 8f, if (radius >= 40f) 12f else 11f, C_KNOB_LABEL, center = true, face = faceBold, spacing = 0.12f, maxWidth = hitW + 20f)   // SILVETON_THEME_V1: printed above, like on the instrument
            else text(c, label, cx, vy + 16f, if (radius >= 40f) 11f else 10f, C_KNOB_LABEL, center = true, spacing = 0.15f, maxWidth = hitW - 6f)
        }
        knobHit(RectF(cx - hitW / 2f, cy - radius - 16f, cx + hitW / 2f, cy + radius + 10f), value, minV, maxV, step, travel,
            onChange, exact, reset, onFinal)
        hit(RectF(cx - hitW / 2f, cy + radius + 10f, cx + hitW / 2f, vy + 22f), { _, _ -> exact() }, reset)
    }

    private fun drawKnob(c: Canvas, cx: Float, cy: Float, r: Float, t: Float, bipolar: Boolean, ring: Boolean) {
        val tt = t.coerceIn(0f, 1f)
        paint.style = Paint.Style.FILL
        if (darkTheme) { drawRhodesKnob(c, cx, cy, r, tt, ring); return }   // SILVETON_THEME_V1
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

    // SILVETON_THEME_V1: matte black knob, flat brushed-aluminium insert, white line on the rim,
    // thin scale ticks printed on the panel (like the Rhodes Mark I). No value arc.
    private fun drawRhodesKnob(c: Canvas, cx: Float, cy: Float, r: Float, tt: Float, ticks: Boolean) {
        paint.shader = null
        if (ticks) {
            paint.style = Paint.Style.STROKE
            paint.strokeCap = Paint.Cap.BUTT
            val sk = r * 1.42f
            for (i in 0..10) {
                val a = Math.toRadians(135.0 + 270.0 * i / 10.0)
                val cs = kotlin.math.cos(a).toFloat(); val sn = kotlin.math.sin(a).toFloat()
                val l1 = sk * 0.86f; val l2 = if (i % 5 == 0) sk else sk * 0.95f
                paint.color = Color.rgb(196, 196, 190); paint.strokeWidth = if (i % 5 == 0) 1.6f else 1.1f
                c.drawLine(cx + cs * l1, cy + sn * l1, cx + cs * l2, cy + sn * l2, paint)
            }
            paint.style = Paint.Style.FILL
        }
        // shadow under the body
        paint.shader = RadialGradient(cx, cy + r * 0.25f, r * 1.25f, Color.argb(170, 0, 0, 0), Color.argb(0, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy + r * 0.2f, r * 1.25f, paint)
        // matte black body, a little lighter in the middle
        paint.shader = RadialGradient(cx, cy, r, Color.rgb(40, 40, 42), Color.rgb(22, 22, 24), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy, r, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 1f; paint.color = Color.rgb(58, 58, 60)
        c.drawCircle(cx, cy, r, paint)
        paint.color = Color.argb(60, 255, 255, 255); paint.strokeWidth = 1.5f
        c.drawArc(RectF(cx - r + 1.5f, cy - r + 1.5f, cx + r - 1.5f, cy + r - 1.5f), 200f, 140f, false, paint)
        paint.style = Paint.Style.FILL
        // flat brushed-aluminium insert
        val cr = r * 0.56f
        paint.shader = LinearGradient(0f, cy - cr, 0f, cy + cr,
            intArrayOf(Color.rgb(176, 178, 182), Color.rgb(150, 152, 156), Color.rgb(184, 186, 190), Color.rgb(146, 148, 152), Color.rgb(172, 174, 178), Color.rgb(152, 154, 158), Color.rgb(180, 182, 186), Color.rgb(148, 150, 154), Color.rgb(170, 172, 176)),
            null, Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy, cr, paint)
        paint.shader = RadialGradient(cx, cy + cr * 0.6f, cr, Color.argb(0, 0, 0, 0), Color.argb(50, 0, 0, 0), Shader.TileMode.CLAMP)
        c.drawCircle(cx, cy, cr, paint)
        paint.shader = null
        paint.style = Paint.Style.STROKE; paint.strokeWidth = 1.2f; paint.color = Color.rgb(90, 90, 94)
        c.drawCircle(cx, cy, cr, paint)
        paint.strokeWidth = 1.4f; paint.color = Color.rgb(8, 8, 9)
        c.drawCircle(cx, cy, cr + 2f, paint)
        // white pointer line on the rim
        val ang = Math.toRadians(135.0 + 270.0 * tt)
        val cos = kotlin.math.cos(ang).toFloat(); val sin = kotlin.math.sin(ang).toFloat()
        paint.color = Color.rgb(236, 234, 226); paint.strokeWidth = max(2.4f, r * 0.08f); paint.strokeCap = Paint.Cap.BUTT
        c.drawLine(cx + cos * (cr + 3f), cy + sin * (cr + 3f), cx + cos * r * 0.96f, cy + sin * r * 0.96f, paint)
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

    private fun faderCap(c: Canvas, cx: Float, cy: Float, big: Boolean = false) {
        val hw = if (big) 26f else 20f
        val hh = if (big) 15f else 11f
        val r = RectF(cx - hw, cy - hh, cx + hw, cy + hh)
        if (darkTheme) {   // SILVETON_THEME_V1: black cap with a white line, like the Rhodes sliders
            shadow(c, r, 3f, 4f, 150)
            fill(c, r, 3f, Color.rgb(60, 60, 64))
            fill(c, RectF(r.left + 1f, r.top + 1f, r.right - 1f, r.bottom - 1f), 2.5f, Color.rgb(26, 26, 28))
            fill(c, RectF(r.left + 3f, r.top + 3f, r.right - 3f, cy - 1f), 2f, Color.rgb(38, 38, 41))
            fill(c, RectF(r.left + 5f, cy - 1.2f, r.right - 5f, cy + 1.2f), 1f, Color.rgb(236, 234, 226))
            return
        }
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
    private fun dbText(db: Float): String = "%.2f dB".format(db)
    private fun voiceModeName(m: Int) = when (m) { 1 -> "MONO"; 2 -> "LEGATO"; else -> "POLY" }
    private fun effectName(t: Int) = when (t) {
        1 -> "DIGITAL DELAY"; 2 -> "CHORUS"; 3 -> "PLATE REVERB"; 4 -> "HALL"; 5 -> "ROOM"; 6 -> "CHAMBER"; 7 -> "SPRING"
        8 -> "TAPE DELAY"; 9 -> "PING-PONG"; 10 -> "MULTI-TAP"; 11 -> "DUCKING DELAY"; 12 -> "FLANGER"; 13 -> "PHASER"; 14 -> "ROTARY"
        15 -> "COMPRESSOR"; 16 -> "DRIVE"; 17 -> "ENSEMBLE"; else -> "OFF"
    }
    private fun panText(p: Float) = when { p < -0.02f -> "L${(-p * 100).toInt()}"; p > 0.02f -> "R${(p * 100).toInt()}"; else -> "C" }
    private fun noteName(key: Int): String {
        val names = arrayOf("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
        val k = key.coerceIn(0, 127)
        return names[k % 12] + (k / 12 - 1)
    }

    companion object {
        private const val DESIGN_W = 1340f
        // SILVETON_SLOW_DRAG_V1: knobs and faders move 2.5 x slower under the finger (finer).
        private const val DRAG_SLOW = 2.5f
        private const val LIST_SIZE = 200
        private const val LIST_PER_PAGE = 30
        private const val LIST_PAGES = (LIST_SIZE + LIST_PER_PAGE - 1) / LIST_PER_PAGE
        private const val DESIGN_H = 800f
        // brushed metal + side navigation
        private var C_METAL_A = Color.rgb(217, 218, 222); private var C_METAL_B = Color.rgb(222, 223, 227)
        private var C_SIDE_A = Color.rgb(38, 40, 44); private var C_SIDE_B = Color.rgb(51, 54, 59)
        private var C_NAV_EDGE = Color.rgb(72, 76, 82); private var C_NAV_TEXT = Color.rgb(158, 163, 171)
        private var C_LOGO = Color.rgb(233, 196, 106)
        // LCD
        private val C_LCD_TOP = Color.rgb(13, 15, 13); private val C_LCD_BOT = Color.rgb(23, 26, 22)
        private val C_LCD = Color.rgb(255, 179, 71); private val C_LCD_DIM = Color.rgb(184, 132, 60)
        private val C_LCD_VALUE = Color.rgb(255, 207, 138); private val C_LCD_OK = Color.rgb(123, 216, 143); private val C_LCD_BAD = Color.rgb(255, 107, 94)
        // light parts
        private var C_KEY_LIGHT = Color.rgb(236, 238, 241); private var C_KEY_LIGHT_EDGE = Color.rgb(185, 188, 194); private var C_KEY_TEXT = Color.rgb(48, 52, 58)
        private var C_STRIP = Color.rgb(236, 238, 241); private var C_STRIP_SEL = Color.WHITE; private var C_EMPTY_CARD = Color.rgb(226, 228, 232)
        private var C_INK = Color.rgb(35, 38, 43); private var C_INK_SOFT = Color.rgb(48, 52, 58); private var C_LABEL_GREY = Color.rgb(123, 128, 138)
        private var C_SCALE = Color.rgb(138, 143, 151); private var C_EMPTY_TEXT = Color.rgb(160, 165, 172)
        private var C_TOGGLE = Color.rgb(223, 226, 230); private var C_TOGGLE_TEXT = Color.rgb(91, 97, 107); private var C_DARK_KEY = Color.rgb(43, 46, 51)
        private var C_TRACK_LIGHT = Color.rgb(201, 204, 209); private var C_METER_BG = Color.rgb(27, 29, 32)
        private val C_ORANGE = Color.rgb(255, 122, 26); private val C_ORANGE_DARK = Color.rgb(194, 90, 15); private val C_AMBER = Color.rgb(255, 179, 71)
        private val C_SOLO = Color.rgb(255, 215, 106); private val C_MUTE = Color.rgb(255, 107, 94); private val C_FREE = Color.rgb(46, 140, 80)
        private val C_GREEN = Color.rgb(60, 157, 90); private val C_YELLOW = Color.rgb(233, 196, 106); private val C_RED = Color.rgb(255, 107, 61)
        // dark boards
        private var C_BOARD_TOP = Color.rgb(52, 55, 61); private var C_BOARD_BOT = Color.rgb(39, 41, 46); private var C_BOARD_EDGE = Color.rgb(85, 89, 95)
        private val C_SECTION = Color.argb(38, 0, 0, 0); private var C_SECTION_TITLE = Color.rgb(201, 204, 209)
        private var C_KNOB_LABEL = Color.rgb(170, 176, 184); private var C_HINT = Color.rgb(143, 148, 156)
        private var C_TAB = Color.rgb(201, 204, 209); private var C_TAB_TEXT = Color.rgb(87, 92, 101)
        private val C_KNOB_A = Color.rgb(75, 79, 86); private val C_KNOB_B = Color.rgb(42, 44, 49); private val C_KNOB_C = Color.rgb(32, 34, 38)
        private val C_PANIC = Color.rgb(179, 38, 30); private val C_PANIC_TOP = Color.rgb(224, 100, 92)
        private val C_WARN_BG = Color.rgb(179, 38, 30); private val C_WARN_TEXT = Color.WHITE
        private val C_LED_OFF = Color.rgb(85, 85, 85); private var C_LED_OFF_LIGHT = Color.rgb(187, 187, 187)
        // SILVETON_THEME_V1: СВЕТЛА (the look above) / ТЪМНА (dark panel, wooden side, Rhodes knobs and
        // faders, lit = on). The colours above are the light set; applyTheme() swaps them.
        private var darkTheme = false
        private var C_NAV_ON_TEXT = Color.WHITE
        private var C_KEY_DARK_FACE = Color.rgb(66, 70, 80); private var C_KEY_DARK_EDGE = Color.rgb(96, 100, 112); private var C_KEY_DARK_TEXT = Color.rgb(190, 194, 204)
        private var C_KEY_ON = Color.rgb(24, 26, 30)
        fun applyTheme(dark: Boolean) {
            darkTheme = dark
            if (!dark) {
                C_METAL_A = Color.rgb(217, 218, 222); C_METAL_B = Color.rgb(222, 223, 227)
                C_SIDE_A = Color.rgb(38, 40, 44); C_SIDE_B = Color.rgb(51, 54, 59)
                C_NAV_EDGE = Color.rgb(72, 76, 82); C_NAV_TEXT = Color.rgb(158, 163, 171); C_NAV_ON_TEXT = Color.WHITE
                C_LOGO = Color.rgb(233, 196, 106)
                C_KEY_LIGHT = Color.rgb(236, 238, 241); C_KEY_LIGHT_EDGE = Color.rgb(185, 188, 194); C_KEY_TEXT = Color.rgb(48, 52, 58)
                C_STRIP = Color.rgb(236, 238, 241); C_STRIP_SEL = Color.WHITE; C_EMPTY_CARD = Color.rgb(226, 228, 232)
                C_INK = Color.rgb(35, 38, 43); C_INK_SOFT = Color.rgb(48, 52, 58); C_LABEL_GREY = Color.rgb(123, 128, 138)
                C_SCALE = Color.rgb(138, 143, 151); C_EMPTY_TEXT = Color.rgb(160, 165, 172)
                C_TOGGLE = Color.rgb(223, 226, 230); C_TOGGLE_TEXT = Color.rgb(91, 97, 107); C_DARK_KEY = Color.rgb(43, 46, 51)
                C_TRACK_LIGHT = Color.rgb(201, 204, 209); C_METER_BG = Color.rgb(27, 29, 32)
                C_BOARD_TOP = Color.rgb(52, 55, 61); C_BOARD_BOT = Color.rgb(39, 41, 46); C_BOARD_EDGE = Color.rgb(85, 89, 95)
                C_SECTION_TITLE = Color.rgb(201, 204, 209); C_KNOB_LABEL = Color.rgb(170, 176, 184); C_HINT = Color.rgb(143, 148, 156)
                C_TAB = Color.rgb(201, 204, 209); C_TAB_TEXT = Color.rgb(87, 92, 101); C_LED_OFF_LIGHT = Color.rgb(187, 187, 187)
            } else {
                C_METAL_A = Color.rgb(30, 32, 37); C_METAL_B = Color.rgb(31, 33, 38)
                C_SIDE_A = Color.rgb(74, 46, 28); C_SIDE_B = Color.rgb(112, 70, 40)
                C_NAV_EDGE = Color.rgb(96, 100, 112); C_NAV_TEXT = Color.rgb(190, 194, 204); C_NAV_ON_TEXT = Color.rgb(222, 186, 110)
                C_LOGO = Color.rgb(222, 186, 110)
                C_KEY_LIGHT = Color.rgb(66, 70, 80); C_KEY_LIGHT_EDGE = Color.rgb(16, 17, 20); C_KEY_TEXT = Color.rgb(190, 194, 204)
                C_STRIP = Color.rgb(42, 45, 52); C_STRIP_SEL = Color.rgb(50, 54, 62); C_EMPTY_CARD = Color.rgb(36, 38, 44)
                C_INK = Color.rgb(226, 228, 232); C_INK_SOFT = Color.rgb(200, 204, 210); C_LABEL_GREY = Color.rgb(150, 156, 168)
                C_SCALE = Color.rgb(120, 126, 138); C_EMPTY_TEXT = Color.rgb(120, 126, 138)
                C_TOGGLE = Color.rgb(66, 70, 80); C_TOGGLE_TEXT = Color.rgb(190, 194, 204); C_DARK_KEY = Color.rgb(24, 26, 30)
                C_TRACK_LIGHT = Color.rgb(14, 15, 18); C_METER_BG = Color.rgb(14, 15, 18)
                C_BOARD_TOP = Color.rgb(26, 28, 32); C_BOARD_BOT = Color.rgb(22, 24, 28); C_BOARD_EDGE = Color.rgb(52, 56, 64)
                C_SECTION_TITLE = Color.rgb(222, 186, 110); C_KNOB_LABEL = Color.rgb(222, 224, 230); C_HINT = Color.rgb(150, 156, 168)
                C_TAB = Color.rgb(66, 70, 80); C_TAB_TEXT = Color.rgb(190, 194, 204); C_LED_OFF_LIGHT = Color.rgb(85, 85, 85)
            }
        }
    }
}
