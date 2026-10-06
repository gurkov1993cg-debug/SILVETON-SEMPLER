package com.silveton.sampler

import android.app.AlertDialog
import android.app.Dialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.text.InputType
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.ViewConfiguration
import android.view.Window
import android.view.WindowManager
import android.view.View
import android.widget.EditText
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import kotlin.math.max
import kotlin.math.min
import android.widget.SeekBar

/**
 * Functional SILVETON SAMPLER touchscreen surface.
 *
 * Visual baseline: SILVETON LIVE MIDI native Canvas renderer; sampler labels/content remain unchanged.
 * Visible controls are intentionally limited to functions that have a native
 * state/DSP/action path. PROGRAM, SAMPLE and MIXER are not tablet top-level
 * pages; prepared sounds are selected directly into COMBI UPPER 1..7.
 */
class SilvetonSamplerView(
    context: Context,
    private val sampler: SamplerAppController
) : View(context) {

    private enum class Page { LIST, COMBI, EFFECTS, SETTINGS }
    private enum class UpperTab { TUNE, RANGE, CONTROL, TONE, FX }

    private data class Hit(
        val rect: RectF,
        val tap: (Float, Float) -> Unit,
        val reset: (() -> Unit)? = null,
        val drag: ((Float, Float, Boolean) -> Unit)? = null
    )

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val hits = ArrayList<Hit>(96)
    private var page = Page.COMBI
    private var tab = UpperTab.TUNE
    private var selectedUpper = 0
    private var editingCombiSlot: Int? = null
    private var midiConnections = 0
    private var warning: String? = null
    private var listPage = 0
    private data class SmoothSliderHit(
        val rect: RectF,
        val minValue: Float,
        val maxValue: Float,
        val step: Float,
        val onChange: (Float) -> Unit,
        var lastValue: Float = Float.NaN
    )
    private data class SmoothRangeHit(
        val rect: RectF,
        val minValue: Float,
        val maxValue: Float,
        val step: Float,
        var low: Float,
        var high: Float,
        val onChange: (Float, Float) -> Unit
    )
    private val smoothSliderHits = ArrayList<SmoothSliderHit>(48)
    private val smoothRangeHits = ArrayList<SmoothRangeHit>(4)
    private var activeSmoothSlider: SmoothSliderHit? = null
    private var activeSmoothRange: SmoothRangeHit? = null
    private var activeSmoothRangeHigh = false
    private var smoothPointerId = -1
    private val portamentoDragRect = RectF()
    private var portamentoDragActive = false
    private var lastPortamentoDragValue = -1
    private data class ListLongHit(val rect: RectF, val action: (Float, Float) -> Unit)
    private val listLongHits = ArrayList<ListLongHit>(24)
    private data class ListClipboard(val item: SamplerAppController.ListItem, val cut: Boolean)
    private var listClipboard: ListClipboard? = null
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
    private val touchSlop = android.view.ViewConfiguration.get(context).scaledTouchSlop.toFloat()

    // Typeface.create() used to run for virtually every label on every frame.
    // Keep the two faces for the lifetime of the view instead.
    private val normalTypeface = Typeface.create(Typeface.SANS_SERIF, Typeface.NORMAL)
    private val boldTypeface = Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD)

    // LIST/COMBI identity data is not realtime telemetry. Cache it instead of
    // allocating arrays/strings through JNI every redraw.
    private var cachedCombiName = "COMBI"
    private var cachedListItems: List<SamplerAppController.ListItem> = emptyList()
    private var cachedActiveListEntry = -1
    private var listCacheLoaded = false

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
                    if (!dragMoved && dx*dx + dy*dy >= touchSlop*touchSlop) dragMoved = true
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
                                // A short tap on a slider jumps directly to that position.
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

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (handleSmoothControlTouch(event)) return true
        if (page == Page.COMBI && tab == UpperTab.TUNE) {
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    if (portamentoDragRect.contains(event.x, event.y)) {
                        portamentoDragActive = true
                        lastPortamentoDragValue = -1
                        parent?.requestDisallowInterceptTouchEvent(true)
                        updatePortamentoFromX(event.x)
                        return true
                    }
                }
                MotionEvent.ACTION_MOVE -> {
                    if (portamentoDragActive) {
                        updatePortamentoFromX(event.x)
                        return true
                    }
                }
                MotionEvent.ACTION_UP -> {
                    if (portamentoDragActive) {
                        updatePortamentoFromX(event.x)
                        portamentoDragActive = false
                        parent?.requestDisallowInterceptTouchEvent(false)
                        return true
                    }
                }
                MotionEvent.ACTION_CANCEL -> {
                    if (portamentoDragActive) {
                        portamentoDragActive = false
                        parent?.requestDisallowInterceptTouchEvent(false)
                        return true
                    }
                }
            }
        }

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

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        hits.clear()
        smoothSliderHits.clear()
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

        if (activeHit?.drag != null) postInvalidateOnAnimation() else postInvalidateDelayed(100L)
    }

    private fun drawHeader(c: Canvas, w: Float, h: Float) {
        val sx = w / DESIGN_W
        val sy = h / DESIGN_H
        val ss = min(sx, sy)
        val hh = 76f * sy
        paint.shader = null
        paint.color = C_BG_TOP
        c.drawRect(0f, 0f, w, hh, paint)
        line(c, 16f*sx, hh-1f*sy, w-16f*sx, hh-1f*sy, Color.argb(28,255,255,255), 1f*ss)

        val gear = RectF(16f*sx,12f*sy,84f*sx,64f*sy)
        silvKey(c,gear,page==Page.SETTINGS,false)
        drawGear(c,gear.centerX(),gear.centerY(),12f*ss)
        hit(gear,{_,_-> page=Page.SETTINGS; refreshGlobal()})

        val panic = RectF(96f*sx,12f*sy,244f*sx,64f*sy)
        silvKey(c,panic,false,true)
        silvLed(c,panic.left+20f*sx,panic.centerY(),true,true,5.5f*ss)
        text(c,"PANIC",panic.centerX()+8f*sx,panic.centerY()+6f*sy,17f*ss,C_TEXT,alignCenter=true,bold=true)
        hit(panic,{_,_->sampler.panic()})

        drawBrand(c,w*0.5f,38f*sy,sx,sy)

        val a = frameAudioStatus
        val m = frameMidiStatus
        val now = android.os.SystemClock.elapsedRealtime()
        val midiRx = now < midiFlashUntilMs
        val keyFlash = midiRx && (m.status and 0xF0) == 0x90 && m.data2 > 0
        val midiConnected = max(midiConnections,m.deviceCount) > 0
        val midiValue = when {
            keyFlash -> "${midiNoteName(m.data1)}"
            midiRx -> "RX"
            midiConnected -> "ON"
            m.lastError != 0 -> "E${-m.lastError}"
            else -> "OFF"
        }

        val running = sampler.audioRunning()
        val lastAudioError = sampler.audioLastError()
        val audioOk = running && a.sampleRate == 48000 && a.framesPerBurst == 96 && a.bufferSizeFrames == 192
        val audioSignal = a.activeVoices > 0 || frameUpperMeters.any { it.first > 0.0001f || it.second > 0.0001f }
        val audioValue = when {
            audioOk -> "96k 96x2"
            running -> "${a.sampleRate/1000}k ${a.framesPerBurst}"
            lastAudioError != 0 -> "OFF E${-lastAudioError}"
            else -> "OFF"
        }

        val chipY=16f*sy; val chipH=44f*sy; val gap=8f*sx; var right=w-16f*sx
        val midiR=RectF(right-124f*sx,chipY,right,chipY+chipH)
        silvStatus(c,midiR,"MIDI",midiValue,keyFlash,sx,sy); right=midiR.left-gap
        val xrunR=RectF(right-108f*sx,chipY,right,chipY+chipH)
        silvStatus(c,xrunR,"XRUN",a.xrunCount.toString(),audioOk&&a.xrunCount==0,sx,sy); right=xrunR.left-gap
        val audioR=RectF(right-164f*sx,chipY,right,chipY+chipH)
        silvStatus(c,audioR,"AUDIO",audioValue,audioSignal,sx,sy)
    }

    private fun drawRightNav(c: Canvas, r: RectF) {
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
                if (p == Page.LIST) refreshListCache()
                if (p == Page.COMBI) cachedCombiName = sampler.combiName()
                if (p == Page.EFFECTS) refreshFx()
                if (p == Page.SETTINGS) refreshGlobal()
            })
        }
    }

    private fun drawCombi(c: Canvas, r: RectF) {
        panel(c, r, C_PANEL, 14f.dp)
        val pad = 16f.dp
        val titleH = r.height() * 0.145f
        text(c, "●  К О М Б И", r.left + pad, r.top + 27f.dp, 13f.dp, C_TEXT_DIM)
        val combiName = cachedCombiName
        text(c, combiName, r.left + pad, r.top + titleH * 0.70f, 31f.dp, C_TEXT, bold = true, maxWidth = r.width() - 330f.dp)
        val st = frameAudioStatus
        text(c, "ГЛАСОВЕ", r.right - 145f.dp, r.top + 34f.dp, 10f.dp, C_TEXT_MUTED)
        text(c, st.activeVoices.toString(), r.right - 120f.dp, r.top + 60f.dp, 22f.dp, C_TEXT)
        text(c, "ПУКАНЕ", r.right - 68f.dp, r.top + 34f.dp, 10f.dp, C_TEXT_MUTED)
        text(c, st.xrunCount.toString(), r.right - 46f.dp, r.top + 60f.dp, 22f.dp, C_TEXT)

        val rowsTop = r.top + titleH
        val lowerH = r.height() * 0.37f
        val rowsBottom = r.bottom - lowerH - 10f.dp
        val rowH = (rowsBottom - rowsTop) / 7f
        val meters = frameUpperMeters

        for (i in 0 until 7) {
            val state = upperStates[i] ?: sampler.upperState(i)?.also { upperStates[i] = it } ?: continue
            val rr = RectF(r.left + pad, rowsTop + i * rowH + 2f.dp, r.right - pad, rowsTop + (i + 1) * rowH - 2f.dp)
            drawUpperRow(c, rr, i, state, meters.getOrElse(i) { 0f to 0f })
        }

        val lower = RectF(r.left + pad, rowsBottom + 8f.dp, r.right - pad, r.bottom - pad)
        drawUpperEditor(c, lower)
        val writeR = RectF(r.right - 500f.dp, r.top + 15f.dp, r.right - 360f.dp, r.top + 57f.dp)
        button(c, writeR, "WRITE COMBI", true)
        hit(writeR, { _, _ -> showWriteCombi(false) })

        val addListR = RectF(r.right - 350f.dp, r.top + 15f.dp, r.right - 195f.dp, r.top + 57f.dp)
        button(c, addListR, "ADD TO LIST", true)
        hit(addListR, { _, _ -> showWriteCombi(true) })

        val blendsR = RectF(r.right - 665f.dp, r.top + 15f.dp, r.right - 510f.dp, r.top + 57f.dp)
        button(c, blendsR, "BLENDS", true)
        hit(blendsR, { _, _ -> showBlendPicker() })

    }

    private fun drawUpperRow(c: Canvas, r: RectF, i: Int, s: SamplerAppController.UpperState, meter: Pair<Float, Float>) {
        val selected = i == selectedUpper
        panel(c, r, if (selected) C_ROW_SELECTED else C_ROW, 9f.dp, if (selected) C_ACCENT_DARK else C_BORDER)
        val x0 = r.left + 13f.dp
        text(c, "UPPER ${i + 1}", x0, r.centerY() + 5f.dp, 12f.dp, if (selected) C_ACCENT else C_TEXT_DIM, bold = selected, maxWidth = r.width() * 0.11f)

        val soundL = r.left + r.width() * 0.13f
        val soundR = r.left + r.width() * 0.36f
        text(c, s.soundName, soundL, r.centerY() + 6f.dp, 13f.dp, if (s.soundName == "EMPTY") C_TEXT_MUTED else C_TEXT, maxWidth = soundR - soundL - 8f.dp)
        hit(RectF(soundL - 4f.dp, r.top, soundR, r.bottom), { _, _ -> selectedUpper = i; showSoundPicker(i) })

        val onR = RectF(r.left + r.width() * 0.365f, r.top + 6f.dp, r.left + r.width() * 0.405f, r.bottom - 6f.dp)
        tinyToggle(c, onR, "ON", s.enabled)
        hit(onR, { _, _ -> if (s.soundName != "EMPTY") commitMain(i, s.copy(enabled = !s.enabled)) })

        val soloR = RectF(r.left + r.width() * 0.410f, r.top + 6f.dp, r.left + r.width() * 0.455f, r.bottom - 6f.dp)
        tinyToggle(c, soloR, "S", s.solo)
        hit(soloR, { _, _ -> commitMain(i, s.copy(solo = !s.solo)) })

        val muteR = RectF(r.left + r.width() * 0.460f, r.top + 6f.dp, r.left + r.width() * 0.505f, r.bottom - 6f.dp)
        tinyToggle(c, muteR, "M", s.mute)
        hit(muteR, { _, _ -> commitMain(i, s.copy(mute = !s.mute)) })

        val volR = RectF(r.left + r.width() * 0.515f, r.top + 5f.dp, r.left + r.width() * 0.655f, r.bottom - 5f.dp)
        val volValue = RectF(volR.left, volR.top, volR.left + 48f.dp, volR.bottom)
        val volBar = RectF(volR.left + 52f.dp, volR.centerY() - 5f.dp, volR.right - 5f.dp, volR.centerY() + 5f.dp)
        text(c, "%.1f".format(s.levelDb), volValue.left, volValue.centerY() + 4f.dp, 10f.dp, C_TEXT, maxWidth = volValue.width() - 3f.dp)
        drawValueSlider(c, volBar, s.levelDb, -60f, 12f)
        registerSmoothSlider(volBar, -60f, 12f, 0.1f) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(levelDb = nv))
        }
        hit(volValue, { _, _ -> showNumber("UPPER ${i + 1} LEVEL dB", s.levelDb, -60f, 12f, 1) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(levelDb = nv))
        } }, { val current = upperStates[i] ?: s; commitMain(i, current.copy(levelDb = 0f)) })

        val panR = RectF(r.left + r.width() * 0.665f, r.top + 5f.dp, r.left + r.width() * 0.755f, r.bottom - 5f.dp)
        val panValue = RectF(panR.left, panR.top, panR.left + 35f.dp, panR.bottom)
        val panBar = RectF(panR.left + 39f.dp, panR.centerY() - 5f.dp, panR.right - 5f.dp, panR.centerY() + 5f.dp)
        text(c, panText(s.pan), panValue.left, panValue.centerY() + 4f.dp, 10f.dp, C_TEXT, maxWidth = panValue.width() - 2f.dp)
        drawValueSlider(c, panBar, s.pan, -1f, 1f)
        registerSmoothSlider(panBar, -1f, 1f, 0.01f) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(pan = nv))
        }
        hit(panValue, { _, _ -> showNumber("UPPER ${i + 1} PAN", s.pan, -1f, 1f, 2) { nv ->
            val current = upperStates[i] ?: s
            commitMain(i, current.copy(pan = nv))
        } }, { val current = upperStates[i] ?: s; commitMain(i, current.copy(pan = 0f)) })

        val rangeR = RectF(r.left + r.width() * 0.765f, r.top, r.left + r.width() * 0.855f, r.bottom)
        text(c, "${noteName(s.keyLow)}  –  ${noteName(s.keyHigh)}", rangeR.centerX(), r.centerY() + 5f.dp, 11f.dp, C_TEXT, alignCenter = true, maxWidth = rangeR.width() - 8f.dp)
        hit(rangeR, { _, _ -> selectedUpper = i; showKeyRange(i, s) })

        val modeR = RectF(r.left + r.width() * 0.86f, r.top + 5f.dp, r.left + r.width() * 0.94f, r.bottom - 5f.dp)
        button(c, modeR, voiceModeName(s.voiceMode), false)
        hit(modeR, { _, _ -> commitMain(i, s.copy(voiceMode = (s.voiceMode + 1) % 3)) }, { commitMain(i, s.copy(voiceMode = 0)) })

        val meterR = RectF(r.left + r.width() * 0.947f, r.top + 10f.dp, r.right - 9f.dp, r.bottom - 10f.dp)
        drawUpperMeter(c, meterR, max(meter.first, meter.second))
        hit(RectF(r.left, r.top, soundL - 6f.dp, r.bottom), { _, _ -> selectedUpper = i })
    }

    private fun drawUpperEditor(c: Canvas, r: RectF) {
        panel(c, r, C_PANEL_DARK, 12f.dp)
        val s = upperStates[selectedUpper] ?: return
        text(c, "●  UPPER ${selectedUpper + 1}", r.left + 15f.dp, r.top + 27f.dp, 15f.dp, C_ACCENT)
        val tabsY = r.top + 40f.dp
        val tabH = 38f.dp
        val tabStart = r.left + 165f.dp
        val tabGap = 8f.dp
        val tabW = (r.right - tabStart - 10f.dp - tabGap * 4f) / 5f
        UpperTab.entries.forEachIndexed { idx, t ->
            val rr = RectF(tabStart + idx * (tabW + tabGap), tabsY - 29f.dp, tabStart + idx * (tabW + tabGap) + tabW, tabsY + 9f.dp)
            button(c, rr, t.name, tab == t)
            hit(rr, { _, _ -> tab = t })
        }
        val body = RectF(r.left + 12f.dp, tabsY + 18f.dp, r.right - 12f.dp, r.bottom - 10f.dp)
        when (tab) {
            UpperTab.TUNE -> drawTune(c, body, s)
            UpperTab.RANGE -> drawRange(c, body, s)
            UpperTab.CONTROL -> drawControl(c, body, s)
            UpperTab.TONE -> drawTone(c, body, s)
            UpperTab.FX -> drawUpperFx(c, body, s)
        }
    }

    private fun quantizeControl(value: Float, minValue: Float, maxValue: Float, step: Float): Float {
        val clamped = value.coerceIn(minValue, maxValue)
        if (step <= 0f) return clamped
        val q = kotlin.math.round((clamped - minValue) / step) * step + minValue
        return q.coerceIn(minValue, maxValue)
    }

    private fun registerSmoothSlider(r: RectF, minValue: Float, maxValue: Float, step: Float, onChange: (Float) -> Unit) {
        smoothSliderHits.add(SmoothSliderHit(RectF(r), minValue, maxValue, step, onChange))
    }

    private fun registerSmoothRange(r: RectF, minValue: Float, maxValue: Float, step: Float, low: Float, high: Float, onChange: (Float, Float) -> Unit) {
        smoothRangeHits.add(SmoothRangeHit(RectF(r), minValue, maxValue, step, low, high, onChange))
    }

    private fun sliderValueAt(hit: SmoothSliderHit, x: Float): Float {
        if (hit.rect.width() <= 1f) return hit.minValue
        val t = ((x - hit.rect.left) / hit.rect.width()).coerceIn(0f, 1f)
        return quantizeControl(hit.minValue + (hit.maxValue - hit.minValue) * t, hit.minValue, hit.maxValue, hit.step)
    }

    private fun emitSmoothSlider(hit: SmoothSliderHit, x: Float) {
        val value = sliderValueAt(hit, x)
        if (hit.lastValue.isNaN() || kotlin.math.abs(value - hit.lastValue) >= max(0.000001f, hit.step * 0.49f)) {
            hit.lastValue = value
            hit.onChange(value)
            invalidate()
        }
    }

    private fun emitSmoothRange(hit: SmoothRangeHit, x: Float) {
        if (hit.rect.width() <= 1f) return
        val t = ((x - hit.rect.left) / hit.rect.width()).coerceIn(0f, 1f)
        val value = quantizeControl(hit.minValue + (hit.maxValue - hit.minValue) * t, hit.minValue, hit.maxValue, hit.step)
        if (activeSmoothRangeHigh) hit.high = max(hit.low, value) else hit.low = min(hit.high, value)
        hit.onChange(hit.low, hit.high)
        invalidate()
    }

    private fun handleSmoothControlTouch(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val slider = smoothSliderHits.asReversed().firstOrNull { it.rect.contains(event.x, event.y) }
                if (slider != null) {
                    activeSmoothSlider = slider
                    smoothPointerId = event.getPointerId(0)
                    parent?.requestDisallowInterceptTouchEvent(true)
                    emitSmoothSlider(slider, event.x)
                    return true
                }
                val range = smoothRangeHits.asReversed().firstOrNull { it.rect.contains(event.x, event.y) }
                if (range != null) {
                    activeSmoothRange = range
                    smoothPointerId = event.getPointerId(0)
                    val lowX = range.rect.left + range.rect.width() * ((range.low - range.minValue) / (range.maxValue - range.minValue)).coerceIn(0f, 1f)
                    val highX = range.rect.left + range.rect.width() * ((range.high - range.minValue) / (range.maxValue - range.minValue)).coerceIn(0f, 1f)
                    activeSmoothRangeHigh = kotlin.math.abs(event.x - highX) < kotlin.math.abs(event.x - lowX)
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
                        activeSmoothSlider?.let { emitSmoothSlider(it, event.getX(index)); return true }
                        activeSmoothRange?.let { emitSmoothRange(it, event.getX(index)); return true }
                    }
                }
            }
            MotionEvent.ACTION_UP -> {
                activeSmoothSlider?.let { emitSmoothSlider(it, event.x) }
                activeSmoothRange?.let { emitSmoothRange(it, event.x) }
                if (activeSmoothSlider != null || activeSmoothRange != null) {
                    activeSmoothSlider = null
                    activeSmoothRange = null
                    smoothPointerId = -1
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
            MotionEvent.ACTION_CANCEL -> {
                if (activeSmoothSlider != null || activeSmoothRange != null) {
                    activeSmoothSlider = null
                    activeSmoothRange = null
                    smoothPointerId = -1
                    parent?.requestDisallowInterceptTouchEvent(false)
                    return true
                }
            }
        }
        return false
    }

    private fun drawValueSlider(c: Canvas, r: RectF, value: Float, minValue: Float, maxValue: Float) {
        paint.style = Paint.Style.FILL
        paint.shader = null
        paint.color = C_TRACK
        c.drawRoundRect(r, r.height() * 0.5f, r.height() * 0.5f, paint)
        val t = if (maxValue > minValue) ((value - minValue) / (maxValue - minValue)).coerceIn(0f, 1f) else 0f
        val x = r.left + r.width() * t
        paint.color = C_ACCENT
        if (x > r.left) c.drawRoundRect(RectF(r.left, r.top, x, r.bottom), r.height() * 0.5f, r.height() * 0.5f, paint)
        paint.color = C_KNOB_LIGHT
        c.drawCircle(x, r.centerY(), max(6f.dp, r.height() * 0.68f), paint)
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = 1f.dp
        paint.color = Color.argb(150, 0, 0, 0)
        c.drawCircle(x, r.centerY(), max(6f.dp, r.height() * 0.68f), paint)
        paint.style = Paint.Style.FILL
    }

    // PORTAMENTO TIME like the SILVETON keyboard processor: 0 = OFF, 1..100 ms (linear,
    // the same total time for every interval).
    private fun portamentoUiValue(seconds: Float): Float {
        if (seconds <= 0f) return 0f
        return (seconds * 1000f).coerceIn(1f, 100f)
    }

    private fun portamentoSecondsFromUi(value: Float): Float {
        val ms = kotlin.math.round(value.coerceIn(0f, 100f))
        return if (ms <= 0f) 0f else ms / 1000f
    }

    private fun updatePortamentoFromX(x: Float) {
        if (portamentoDragRect.width() <= 1f) return
        val normalized = ((x - portamentoDragRect.left) / portamentoDragRect.width()).coerceIn(0f, 1f)
        val value = kotlin.math.round(normalized * 100f).toInt().coerceIn(0, 100)
        if (value == lastPortamentoDragValue) return
        lastPortamentoDragValue = value
        val state = upperStates[selectedUpper] ?: return
        commitTune(state.copy(portamentoSec = portamentoSecondsFromUi(value.toFloat())))
        invalidate()
    }

    private fun drawTune(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val gap = 10f.dp
        val cols = 5
        val cw = (r.width() - gap * (cols - 1)) / cols

        run {
            val rr = RectF(r.left, r.top, r.left + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            text(c, "PORTAMENTO", rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 16f.dp)
            val value = kotlin.math.round(portamentoUiValue(s.portamentoSec)).toInt().coerceIn(0, 100)
            val bar = RectF(rr.left + 24f.dp, rr.centerY() - 7f.dp, rr.right - 24f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value.toFloat(), 0f, 100f)
            portamentoDragRect.set(bar.left - 8f.dp, bar.top - 18f.dp, bar.right + 8f.dp, bar.bottom + 18f.dp)
            val valueR = RectF(rr.left + 18f.dp, rr.bottom - 48f.dp, rr.right - 18f.dp, rr.bottom)
            val glideSec = portamentoSecondsFromUi(value.toFloat())
            val glideText = if (value == 0) "OFF" else "${kotlin.math.round(glideSec * 1000f).toInt()} ms"
            text(c, glideText, rr.centerX(), rr.bottom - 18f.dp, 14f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ ->
                showNumber("PORTAMENTO", value.toFloat(), 0f, 100f, 0) { exact ->
                    commitTune(s.copy(portamentoSec = portamentoSecondsFromUi(kotlin.math.round(exact).coerceIn(0f, 100f))))
                }
            }, { commitTune(s.copy(portamentoSec = 0f)) })
        }

        for (j in 0 until 4) {
            val i = j + 1
            val rr = RectF(r.left + i * (cw + gap), r.top, r.left + i * (cw + gap) + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            val title = when (j) { 0 -> "ОКТАВА"; 1 -> "ТРАНСПОНИРАНЕ"; 2 -> "ФИНА НАСТРОЙКА"; else -> "PITCH BEND" }
            val value = when (j) { 0 -> s.octave.toFloat(); 1 -> s.transpose.toFloat(); 2 -> s.fineCents; else -> s.pitchBendRange.toFloat() }
            val minV = when (j) { 0 -> -4f; 1 -> -48f; 2 -> -100f; else -> 0f }
            val maxV = when (j) { 0 -> 4f; 1 -> 48f; 2 -> 100f; else -> 24f }
            val step = if (j == 2) 0.1f else 1f
            val decimals = if (j == 2) 1 else 0
            text(c, title, rr.centerX(), rr.top + 23f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 16f.dp)
            val bar = RectF(rr.left + 22f.dp, rr.centerY() - 7f.dp, rr.right - 22f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value, minV, maxV)
            registerSmoothSlider(bar, minV, maxV, step) { nv ->
                val current = upperStates[selectedUpper] ?: s
                when (j) {
                    0 -> commitTune(current.copy(octave = nv.toInt()))
                    1 -> commitTune(current.copy(transpose = nv.toInt()))
                    2 -> commitTune(current.copy(fineCents = nv))
                    else -> commitTune(current.copy(pitchBendRange = nv.toInt()))
                }
            }
            val valueR = RectF(rr.left + 12f.dp, rr.bottom - 48f.dp, rr.right - 12f.dp, rr.bottom)
            val display = if (decimals == 0) value.toInt().toString() else "%.1f".format(value)
            text(c, display, rr.centerX(), rr.bottom - 18f.dp, 13f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ ->
                showNumber(title, value, minV, maxV, decimals) { nv ->
                    val current = upperStates[selectedUpper] ?: s
                    when (j) {
                        0 -> commitTune(current.copy(octave = nv.toInt()))
                        1 -> commitTune(current.copy(transpose = nv.toInt()))
                        2 -> commitTune(current.copy(fineCents = nv))
                        else -> commitTune(current.copy(pitchBendRange = nv.toInt()))
                    }
                }
            }, {
                val current = upperStates[selectedUpper] ?: s
                when (j) {
                    0 -> commitTune(current.copy(octave = 0))
                    1 -> commitTune(current.copy(transpose = 0))
                    2 -> commitTune(current.copy(fineCents = 0f))
                    else -> commitTune(current.copy(pitchBendRange = 2))
                }
            })
        }
    }

    private fun drawRange(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val rr = RectF(r.left, r.top, r.left + r.width() * 0.62f, r.bottom)
        panel(c, rr, C_SUBPANEL, 9f.dp)
        text(c, "VELOCITY RANGE", rr.left + 16f.dp, rr.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = rr.width() - 32f.dp)
        val bar = RectF(rr.left + 38f.dp, rr.centerY() - 7f.dp, rr.right - 38f.dp, rr.centerY() + 7f.dp)
        drawRangeBar(c, bar, s.velocityLow.toFloat(), s.velocityHigh.toFloat(), 1f, 127f)
        registerSmoothRange(bar, 1f, 127f, 1f, s.velocityLow.toFloat(), s.velocityHigh.toFloat()) { lo, hi ->
            val current = upperStates[selectedUpper] ?: s
            commitRange(current.copy(velocityLow = lo.toInt(), velocityHigh = hi.toInt()))
        }
        text(c, s.velocityLow.toString(), rr.left + 18f.dp, rr.centerY() + 5f.dp, 13f.dp, C_TEXT)
        text(c, s.velocityHigh.toString(), rr.right - 18f.dp, rr.centerY() + 5f.dp, 13f.dp, C_TEXT, alignCenter = true)
        hit(RectF(rr.left, rr.top, rr.right, bar.top - 10f.dp), { _, _ -> showVelocityRange(selectedUpper, s) })
        hit(RectF(rr.left, bar.bottom + 10f.dp, rr.right, rr.bottom), { _, _ -> showVelocityRange(selectedUpper, s) }, { commitRange(s.copy(velocityLow = 1, velocityHigh = 127)) })

        val info = RectF(rr.right + 12f.dp, r.top, r.right, r.bottom)
        panel(c, info, C_SUBPANEL, 9f.dp)
        text(c, "KEY RANGE", info.left + 16f.dp, info.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = info.width() - 32f.dp)
        text(c, "${noteName(s.keyLow)}  –  ${noteName(s.keyHigh)}", info.left + 16f.dp, info.centerY() + 6f.dp, 22f.dp, C_TEXT, maxWidth = info.width() - 32f.dp)
        text(c, "Докосни за точни граници", info.left + 16f.dp, info.bottom - 19f.dp, 10f.dp, C_TEXT_MUTED, maxWidth = info.width() - 32f.dp)
        hit(info, { _, _ -> showKeyRange(selectedUpper, s) })
    }

    private fun drawControl(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val gap = 10f.dp
        val cols = 5
        val cw = (r.width() - gap * (cols - 1)) / cols
        val items = listOf(
            "BREATH CC2" to s.breath,
            "EXPRESSION CC11" to s.expression,
            "AFTERTOUCH" to s.aftertouch,
            "DAMPER CC64" to s.damper
        )
        items.forEachIndexed { i, item ->
            val rr = RectF(r.left + i * (cw + gap), r.top, r.left + i * (cw + gap) + cw, r.bottom)
            panel(c, rr, C_SUBPANEL, 9f.dp)
            text(c, item.first, rr.centerX(), rr.top + 25f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true)
            val tr = RectF(rr.left + 22f.dp, rr.centerY() - 18f.dp, rr.right - 22f.dp, rr.centerY() + 18f.dp)
            tinyToggle(c, tr, if (item.second) "ON" else "OFF", item.second)
            val ns = when (i) {
                0 -> s.copy(breath = !s.breath)
                1 -> s.copy(expression = !s.expression)
                2 -> s.copy(aftertouch = !s.aftertouch)
                else -> s.copy(damper = !s.damper)
            }
            hit(rr, { _, _ -> commitControl(ns) })
        }
        val rr = RectF(r.left + 4 * (cw + gap), r.top, r.right, r.bottom)
        panel(c, rr, C_SUBPANEL, 9f.dp)
        text(c, "MIDI CHANNEL", rr.centerX(), rr.top + 25f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true)
        drawParamSlider(c, rr, (s.midiChannel + 1).toFloat(), 1f, 16f, "MIDI CHANNEL", (s.midiChannel + 1).toString())
        hit(rr, { _, _ -> showNumber("MIDI Channel", (s.midiChannel + 1).toFloat(), 1f, 16f, 0) { commitControl(s.copy(midiChannel = it.toInt() - 1)) } },
            { commitControl(s.copy(midiChannel = 0)) },
            { x, _, final ->
                val ch=(kotlin.math.round(sliderValueFromX(x,rr,1f,16f)).toInt()-1).coerceIn(0,15)
                val ns=s.copy(midiChannel=ch); upperStates[selectedUpper]=ns; if(final) commitControl(ns)
            })
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
                kotlin.math.round((kotlin.math.exp(n * kotlin.math.ln(20001f)) - 1f).coerceIn(0f, 20000f))
            TonePar.SUSTAIN, TonePar.RESONANCE -> kotlin.math.round(n * 1000f) / 1000f
            TonePar.CUTOFF -> if (n >= 0.999f) 20000f else kotlin.math.round(20f * Math.pow(1000.0, n.toDouble()).toFloat())
        }
    }

    private fun toneText(p: TonePar, v: Float): String = when (p) {
        TonePar.ATTACK, TonePar.DECAY, TonePar.RELEASE ->
            if (v < 1000f) "${kotlin.math.round(v).toInt()} ms" else "%.2f s".format(v / 1000f)
        TonePar.SUSTAIN, TonePar.RESONANCE -> "${kotlin.math.round(v * 100f).toInt()} %"
        TonePar.CUTOFF -> when {
            v >= 19900f -> "OPEN"
            v < 1000f -> "${kotlin.math.round(v).toInt()} Hz"
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

    private fun drawToneEnvSection(c: Canvas, r: RectF, title: String, items: List<Pair<String, TonePar>>, s: SamplerAppController.UpperState) {
        panel(c, r, C_SUBPANEL, 9f.dp)
        text(c, title, r.left + 12f.dp, r.top + 22f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = r.width() - 24f.dp)
        val gap = 5f.dp
        val top = r.top + 32f.dp
        val cellW = (r.width() - gap * (items.size + 1)) / items.size
        items.forEachIndexed { i, item ->
            val p = item.second
            val value = toneValue(s, p)
            val rr = RectF(r.left + gap + i * (cellW + gap), top, r.left + gap + i * (cellW + gap) + cellW, r.bottom - 8f.dp)
            text(c, item.first, rr.centerX(), rr.top + 15f.dp, 10f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 4f.dp)
            val bar = RectF(rr.left + 8f.dp, rr.centerY() - 6f.dp, rr.right - 8f.dp, rr.centerY() + 6f.dp)
            drawValueSlider(c, bar, toneToNorm(p, value), 0f, 1f)
            registerSmoothSlider(bar, 0f, 1f, 0.002f) { nv -> setTone(p, toneFromNorm(p, nv)) }
            val valueR = RectF(rr.left, rr.bottom - 34f.dp, rr.right, rr.bottom)
            text(c, toneText(p, value), rr.centerX(), rr.bottom - 8f.dp, 12f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = rr.width() - 2f.dp)
            val range = toneRange(p)
            val shown = if (p == TonePar.SUSTAIN || p == TonePar.RESONANCE) value * 100f else value
            hit(valueR, { _, _ ->
                showNumber("${title} ${item.first}", shown.coerceIn(range.first, range.second), range.first, range.second, range.third) { nv ->
                    setTone(p, if (p == TonePar.SUSTAIN || p == TonePar.RESONANCE) (nv / 100f).coerceIn(0f, 1f) else nv)
                }
            }, { setTone(p, -1f) })
        }
    }

    private fun drawTone(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val gap = 8f.dp
        val weights = floatArrayOf(0.36f, 0.20f, 0.18f, 0.26f)
        val usable = r.width() - gap * 3f
        var x = r.left
        val xs = FloatArray(5)
        for (k in 0 until 4) { xs[k] = x; x += usable * weights[k] + gap }
        xs[4] = r.right + gap
        fun sec(k: Int) = RectF(xs[k], r.top, if (k == 3) r.right else xs[k + 1] - gap, r.bottom)
        drawToneEnvSection(c, sec(0), "AMP ENVELOPE", listOf(
            "ATTACK" to TonePar.ATTACK, "DECAY" to TonePar.DECAY, "SUSTAIN" to TonePar.SUSTAIN, "RELEASE" to TonePar.RELEASE
        ), s)
        drawToneEnvSection(c, sec(1), "FILTER", listOf("CUTOFF" to TonePar.CUTOFF, "RESONANCE" to TonePar.RESONANCE), s)
        drawToneSection(c, sec(2), "LFO", listOf(
            "RATE" to s.lfoRateScale, "DEPTH" to s.lfoDepthScale
        )) { idx, value, final ->
            val ns = if (idx==0) s.copy(lfoRateScale=value) else s.copy(lfoDepthScale=value)
            upperStates[selectedUpper] = ns
            if (final) commitTone(ns)
        }
        drawToneSection(c, sec(3), "EQ", listOf(
            "LOW" to s.eqLowDb, "MID" to s.eqMidDb, "HIGH" to s.eqHighDb
        )) { idx, value, final ->
            val ns = when (idx) { 0 -> s.copy(eqLowDb=value); 1 -> s.copy(eqMidDb=value); else -> s.copy(eqHighDb=value) }
            upperStates[selectedUpper] = ns
            if (final) commitTone(ns)
        }
    }

    private fun drawToneSection(c: Canvas, r: RectF, title: String, values: List<Pair<String, Float>>, commit: (Int, Float, Boolean) -> Unit) {
        panel(c, r, C_SUBPANEL, 9f.dp)
        text(c, title, r.left + 12f.dp, r.top + 22f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = r.width() - 24f.dp)
        val gap = 5f.dp
        val top = r.top + 32f.dp
        val cellW = (r.width() - gap * (values.size + 1)) / values.size
        values.forEachIndexed { i, pair ->
            val rr = RectF(r.left + gap + i * (cellW + gap), top, r.left + gap + i * (cellW + gap) + cellW, r.bottom - 8f.dp)
            val limits = when (title) {
                "AMP" -> if (i == 2) (0f to 2f) else (0f to 16f)
                "FILTER" -> if (i == 0) (-10f to 10f) else (-1f to 1f)
                "LFO" -> if (i == 0) (0f to 16f) else (0f to 8f)
                else -> (-18f to 18f)
            }
            val step = if (title == "EQ") 0.1f else 0.01f
            val default = when (title) { "AMP" -> 1f; "FILTER" -> 0f; "LFO" -> 1f; else -> 0f }
            text(c, pair.first, rr.centerX(), rr.top + 15f.dp, 9f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 8f.dp)
            val bar = RectF(rr.left + 7f.dp, rr.centerY() - 6f.dp, rr.right - 7f.dp, rr.centerY() + 6f.dp)
            drawValueSlider(c, bar, pair.second, limits.first, limits.second)
            registerSmoothSlider(bar, limits.first, limits.second, step) { nv -> commit(i, nv, true) }
            val valueR = RectF(rr.left, rr.bottom - 34f.dp, rr.right, rr.bottom)
            text(c, "%.2f".format(pair.second), rr.centerX(), rr.bottom - 6f.dp, 10f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = rr.width() - 6f.dp)
            hit(valueR, { _, _ -> showNumber("$title ${pair.first}", pair.second, limits.first, limits.second, 2) { commit(i, it, true) } }, { commit(i, default, true) })
        }
    }

    private fun drawUpperFx(c: Canvas, r: RectF, s: SamplerAppController.UpperState) {
        val left = RectF(r.left, r.top, r.left + r.width() * 0.49f, r.bottom)
        val right = RectF(left.right + 10f.dp, r.top, r.right, r.bottom)
        listOf(left to 1, right to 2).forEach { (rr, n) ->
            panel(c, rr, C_SUBPANEL, 9f.dp)
            val value = if (n == 1) s.fxSend1 else s.fxSend2
            text(c, "FX$n SEND", rr.left + 16f.dp, rr.top + 25f.dp, 11f.dp, C_TEXT_MUTED, maxWidth = rr.width() - 32f.dp)
            val bar = RectF(rr.left + 28f.dp, rr.centerY() - 7f.dp, rr.right - 28f.dp, rr.centerY() + 7f.dp)
            drawValueSlider(c, bar, value, 0f, 1f)
            registerSmoothSlider(bar, 0f, 1f, 0.01f) { nv ->
                val current = upperStates[selectedUpper] ?: s
                commitUpperFx(if (n == 1) current.copy(fxSend1 = nv) else current.copy(fxSend2 = nv))
            }
            val valueR = RectF(rr.left + 22f.dp, rr.bottom - 48f.dp, rr.right - 22f.dp, rr.bottom)
            text(c, "${kotlin.math.round(value * 100f).toInt()} %", rr.centerX(), rr.bottom - 18f.dp, 12f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = valueR.width())
            hit(valueR, { _, _ -> showNumber("FX$n SEND", value * 100f, 0f, 100f, 0) { pct ->
                val nv = (pct / 100f).coerceIn(0f, 1f)
                val current = upperStates[selectedUpper] ?: s
                commitUpperFx(if (n == 1) current.copy(fxSend1 = nv) else current.copy(fxSend2 = nv))
            } }, { val current = upperStates[selectedUpper] ?: s; commitUpperFx(if (n == 1) current.copy(fxSend1 = 0f) else current.copy(fxSend2 = 0f)) })
        }
    }

    private fun drawList(c: Canvas, r: RectF) {
        val sx = width.toFloat() / DESIGN_W
        val sy = height.toFloat() / DESIGN_H
        val ss = min(sx, sy)
        silvCard(c, r, C_PANEL, 16f*ss)

        if (!listCacheLoaded) refreshListCache()
        val entries = cachedListItems
        val active = cachedActiveListEntry
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
        val currentName = activeItem?.name ?: "Силветон"
        text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT, maxWidth = right-left-380f*sx)

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
                if (selected) C_TEXT else if (item != null) C_LABEL else C_FAINT, maxWidth = rr.width()-118f*sx)
            if (item != null) {
                val addr = "COMBI  B${item.combiBank+1}  P${item.combiPage+1}  S${item.combiSlot+1}"
                text(c, addr, rr.left+98f*sx, rr.centerY()+19f*sy, 12f*ss, C_TEXT_MUTED)
                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) {
                        cachedActiveListEntry = idx
                        cachedCombiName = sampler.combiName()
                        refreshAllUppers()
                    }
                })
                            listLongHit(rr) { _, _ -> showListContextMenu(idx, item) }
}
        }
        if (activeItem != null) {
            val editR = RectF(right-365f*sx, nowTop+10f*sy, right-205f*sx, nowBottom-10f*sy)
            button(c, editR, "EDIT COMBI", true)
            hit(editR, { _, _ ->
                if (sampler.activateCombiAddress(activeItem.combiBank, activeItem.combiPage, activeItem.combiSlot)) {
                    editingCombiSlot = activeItem.combiSlot
                    page = Page.COMBI
                    refreshAllUppers()
                    refreshFx()
                }
            })
        }

    }

    private fun editListCombi(item: SamplerAppController.ListItem) {
        if (sampler.activateCombiAddress(item.combiBank, item.combiPage, item.combiSlot)) {
            editingCombiSlot = item.combiSlot
            page = Page.COMBI
            refreshAllUppers()
            refreshFx()
            invalidate()
        }
    }

    private fun showListContextMenu(index: Int, item: SamplerAppController.ListItem) {
        val actions = ArrayList<String>()
        actions.add("EDIT COMBI")
        actions.add("RENAME")
        actions.add("COPY")
        actions.add("CUT")
        actions.add("MOVE")
        if (listClipboard != null) {
            actions.add("PASTE BEFORE")
            actions.add("PASTE AFTER")
        }
        actions.add("DELETE")
        AlertDialog.Builder(context)
            .setTitle("%03d   %s".format(item.number, item.name))
            .setItems(actions.toTypedArray()) { dialog, which ->
                when (actions[which]) {
                    "EDIT COMBI" -> editListCombi(item)
                    "RENAME" -> showTextPage("RENAME BLEND", item.name) { newName ->
                        if (sampler.renameLiveListEntry(index, newName)) invalidate()
                    }
                    "COPY" -> listClipboard = ListClipboard(item, false)
                    "CUT" -> listClipboard = ListClipboard(item, true)
                    "MOVE" -> {
                        val count = sampler.listItems(0,0,0).size.coerceAtLeast(1)
                        showNumber("MOVE BLEND — POSITION", (index + 1).toFloat(), 1f, count.toFloat(), 0) { value ->
                            val to = value.toInt().coerceIn(1, count) - 1
                            if (sampler.moveLiveListEntry(index, to)) {
                                listPage = (to / 10).coerceIn(0,19)
                                invalidate()
                            }
                        }
                    }
                    "PASTE BEFORE" -> pasteListClipboard(index, false)
                    "PASTE AFTER" -> pasteListClipboard(index, true)
                    "DELETE" -> AlertDialog.Builder(context)
                        .setTitle("DELETE BLEND")
                        .setMessage("Remove '${item.name}' from LIST? The COMBI itself will not be deleted.")
                        .setPositiveButton("DELETE") { _, _ ->
                            if (sampler.deleteLiveListEntry(index)) {
                                val size = sampler.listItems(0,0,0).size
                                listPage = if (size == 0) 0 else min(listPage, (size - 1) / 10)
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

    private fun pasteListClipboard(targetIndex: Int, after: Boolean) {
        val clip = listClipboard ?: return
        val current = sampler.listItems(0,0,0)
        if (current.isEmpty()) return
        if (clip.cut) {
            val source = current.indexOf(clip.item)
            if (source < 0) { listClipboard = null; return }
            var destination = targetIndex + if (after) 1 else 0
            if (source < destination) destination -= 1
            destination = destination.coerceIn(0, current.size - 1)
            if (sampler.moveLiveListEntry(source, destination)) {
                listClipboard = null
                listPage = (destination / 10).coerceIn(0,19)
                invalidate()
            }
        } else {
            val insertion = (targetIndex + if (after) 1 else 0).coerceIn(0, current.size)
            if (sampler.copyLiveListEntry(clip.item, insertion)) {
                listPage = (insertion / 10).coerceIn(0,19)
                invalidate()
            }
        }
    }

    private fun drawEffects(c: Canvas, r: RectF) {
        panel(c, r, C_PANEL, 14f.dp)
        text(c, "●  Е Ф Е К Т И", r.left+18f.dp, r.top+30f.dp, 13f.dp, C_TEXT_DIM)
        val fx = fxState ?: sampler.combiFxState()?.also { fxState=it } ?: return
        val top = r.top+55f.dp
        val gap = 12f.dp
        val half = (r.width()-36f.dp-gap)/2f
        val f1r = RectF(r.left+18f.dp, top, r.left+18f.dp+half, r.bottom-85f.dp)
        val f2r = RectF(f1r.right+gap, top, r.right-18f.dp, r.bottom-85f.dp)
        drawFxSlot(c,f1r,1,fx.fx1)
        drawFxSlot(c,f2r,2,fx.fx2)
        val route = RectF(r.left+18f.dp, r.bottom-68f.dp, r.right-18f.dp, r.bottom-18f.dp)
        button(c,route, if(fx.routing==0) "ROUTING: PARALLEL" else "ROUTING: FX1 → FX2", true)
        hit(route,{_,_-> val next=if(fx.routing==0)1 else 0; if(sampler.updateCombiFxRouting(next)) refreshFx()},{ if(sampler.updateCombiFxRouting(0)) refreshFx() })
    }

    private fun drawFxSlot(c: Canvas, r: RectF, slot: Int, fx: SamplerAppController.FxState) {
        panel(c, r, C_SUBPANEL, 10f.dp)
        text(c, "FX$slot", r.left + 16f.dp, r.top + 28f.dp, 17f.dp, C_ACCENT, bold = true)
        val typeR = RectF(r.right - 145f.dp, r.top + 10f.dp, r.right - 15f.dp, r.top + 45f.dp)
        button(c, typeR, effectName(fx.type), false)
        hit(typeR, { _, _ -> val n = (fx.type + 1) % 4; if (sampler.updateCombiFx(slot, fx.copy(type = n))) refreshFx() }, { if (sampler.updateCombiFx(slot, fx.copy(type = 0))) refreshFx() })
        val params = listOf(
            "WET" to fx.wet, "TIME ms" to fx.timeMs, "FEEDBACK" to fx.feedback,
            "RATE Hz" to fx.rateHz, "DEPTH" to fx.depth, "TONE" to fx.tone
        )
        val cols = 3; val rows = 2; val gap = 8f.dp
        val areaTop = r.top + 58f.dp
        val cw = (r.width() - 32f.dp - gap * (cols - 1)) / cols
        val ch = (r.bottom - areaTop - 16f.dp - gap * (rows - 1)) / rows
        params.forEachIndexed { i, pair ->
            val col = i % cols; val row = i / cols
            val rr = RectF(r.left + 16f.dp + col * (cw + gap), areaTop + row * (ch + gap), r.left + 16f.dp + col * (cw + gap) + cw, areaTop + row * (ch + gap) + ch)
            val lim = when (i) { 0 -> 0f to 1f; 1 -> 1f to 2500f; 2 -> 0f to .95f; 3 -> .05f to 8f; else -> 0f to 1f }
            val def = when (i) { 0 -> 0f; 1 -> 320f; 2 -> .25f; 3 -> .8f; 4 -> .35f; else -> .5f }
            val step = when (i) { 1 -> 1f; else -> 0.01f }
            val decimals = if (i == 1) 0 else 2
            text(c, pair.first, rr.centerX(), rr.top + 16f.dp, 9f.dp, C_TEXT_MUTED, alignCenter = true, maxWidth = rr.width() - 8f.dp)
            val bar = RectF(rr.left + 8f.dp, rr.centerY() - 6f.dp, rr.right - 8f.dp, rr.centerY() + 6f.dp)
            drawValueSlider(c, bar, pair.second, lim.first, lim.second)
            registerSmoothSlider(bar, lim.first, lim.second, step) { nv ->
                val nf = when (i) { 0 -> fx.copy(wet = nv); 1 -> fx.copy(timeMs = nv); 2 -> fx.copy(feedback = nv); 3 -> fx.copy(rateHz = nv); 4 -> fx.copy(depth = nv); else -> fx.copy(tone = nv) }
                if (sampler.updateCombiFx(slot, nf)) refreshFx()
            }
            val valueR = RectF(rr.left, rr.bottom - 34f.dp, rr.right, rr.bottom)
            val shown = if (decimals == 0) pair.second.toInt().toString() else "%.2f".format(pair.second)
            text(c, shown, rr.centerX(), rr.bottom - 7f.dp, 10f.dp, C_TEXT, alignCenter = true, bold = true, maxWidth = rr.width() - 6f.dp)
            hit(valueR, { _, _ -> showNumber("FX$slot ${pair.first}", pair.second, lim.first, lim.second, decimals) { nv ->
                val nf = when (i) { 0 -> fx.copy(wet = nv); 1 -> fx.copy(timeMs = nv); 2 -> fx.copy(feedback = nv); 3 -> fx.copy(rateHz = nv); 4 -> fx.copy(depth = nv); else -> fx.copy(tone = nv) }
                if (sampler.updateCombiFx(slot, nf)) refreshFx()
            } }, { val nf = when (i) { 0 -> fx.copy(wet = def); 1 -> fx.copy(timeMs = def); 2 -> fx.copy(feedback = def); 3 -> fx.copy(rateHz = def); 4 -> fx.copy(depth = def); else -> fx.copy(tone = def) }; if (sampler.updateCombiFx(slot, nf)) refreshFx() })
        }
    }

    private fun drawSettings(c: Canvas, r: RectF) {
        panel(c,r,C_PANEL,14f.dp)
        text(c,"●  Н А С Т Р О Й К И",r.left+18f.dp,r.top+30f.dp,13f.dp,C_TEXT_DIM)
        val g=globalState ?: sampler.globalUiState()?.also{globalState=it} ?: return
        val labels=arrayOf("MASTER TUNE","TRANSPOSE","MASTER GAIN","EQ LOW","EQ MID","EQ HIGH","LIMITER","CEILING","VELOCITY CURVE","BREATH CC","EXPRESSION CC","DAMPER CC","PITCH BEND","AFTERTOUCH")
        val cols=4; val gap=10f.dp; val top=r.top+55f.dp
        val cw=(r.width()-36f.dp-gap*(cols-1))/cols
        val rows=(labels.size+cols-1)/cols
        val ch=(r.bottom-top-18f.dp-gap*(rows-1))/rows
        labels.indices.forEach{i->
            val col=i%cols; val row=i/cols
            val rr=RectF(r.left+18f.dp+col*(cw+gap),top+row*(ch+gap),r.left+18f.dp+col*(cw+gap)+cw,top+row*(ch+gap)+ch)
            panel(c,rr,C_SUBPANEL,9f.dp)
            if(i==6 || i==12 || i==13){
                val on=when(i){6->g.limiterEnabled;12->g.pitchBendEnabled;else->g.aftertouchEnabled}
                text(c,labels[i],rr.centerX(),rr.top+23f.dp,9f.dp,C_TEXT_MUTED,alignCenter=true)
                val tr=RectF(rr.left+22f.dp,rr.centerY()-18f.dp,rr.right-22f.dp,rr.centerY()+18f.dp)
                tinyToggle(c,tr,if(on)"ON" else "OFF",on)
                hit(rr,{_,_-> editGlobal(i,g)},{ resetGlobal(i,g) })
            } else {
                val spec=globalSliderSpec(i,g)
                drawParamSlider(c,rr,spec.first,spec.second,spec.third,labels[i],globalValueText(i,g))
                hit(rr,{_,_-> editGlobal(i,g)},{ resetGlobal(i,g) },{x,_,final->
                    val raw=sliderValueFromX(x,rr,spec.second,spec.third)
                    val ng=globalWithSliderValue(i,g,raw)
                    globalState=ng
                    if(final) commitGlobal(ng)
                })
            }
        }
    }

    private fun editGlobal(i:Int,g:SamplerAppController.GlobalUiState){
        when(i){
            0->showNumber("Master Tune Hz",g.masterTuneHz,400f,480f,1){commitGlobal(g.copy(masterTuneHz=it))}
            1->showNumber("Transpose",g.transpose.toFloat(),-48f,48f,0){commitGlobal(g.copy(transpose=it.toInt()))}
            2->showNumber("Master Gain dB",g.masterGainDb,-60f,18f,1){commitGlobal(g.copy(masterGainDb=it))}
            3->showNumber("EQ Low dB",g.eqLowDb,-18f,18f,1){commitGlobal(g.copy(eqLowDb=it))}
            4->showNumber("EQ Mid dB",g.eqMidDb,-18f,18f,1){commitGlobal(g.copy(eqMidDb=it))}
            5->showNumber("EQ High dB",g.eqHighDb,-18f,18f,1){commitGlobal(g.copy(eqHighDb=it))}
            6->commitGlobal(g.copy(limiterEnabled=!g.limiterEnabled))
            7->showNumber("Limiter Ceiling",g.limiterCeilingDb,-12f,0f,1){commitGlobal(g.copy(limiterCeilingDb=it))}
            8->showNumber("Velocity Curve",g.velocityCurve.toFloat(),0f,4f,0){commitGlobal(g.copy(velocityCurve=it.toInt()))}
            9->showNumber("Breath CC",g.breathCc.toFloat(),0f,127f,0){commitGlobal(g.copy(breathCc=it.toInt()))}
            10->showNumber("Expression CC",g.expressionCc.toFloat(),0f,127f,0){commitGlobal(g.copy(expressionCc=it.toInt()))}
            11->showNumber("Damper CC",g.damperCc.toFloat(),0f,127f,0){commitGlobal(g.copy(damperCc=it.toInt()))}
            12->commitGlobal(g.copy(pitchBendEnabled=!g.pitchBendEnabled))
            13->commitGlobal(g.copy(aftertouchEnabled=!g.aftertouchEnabled))
        }
    }

    private fun resetGlobal(i:Int,g:SamplerAppController.GlobalUiState){
        val n=when(i){
            0->g.copy(masterTuneHz=440f);1->g.copy(transpose=0);2->g.copy(masterGainDb=0f);3->g.copy(eqLowDb=0f);4->g.copy(eqMidDb=0f);5->g.copy(eqHighDb=0f)
            6->g.copy(limiterEnabled=true);7->g.copy(limiterCeilingDb=-0.3f);8->g.copy(velocityCurve=0);9->g.copy(breathCc=2);10->g.copy(expressionCc=11);11->g.copy(damperCc=64)
            12->g.copy(pitchBendEnabled=true);else->g.copy(aftertouchEnabled=true)
        }; commitGlobal(n)
    }

    private fun showBlendPicker(){
        val blends = sampler.combiCatalog()
        if(blends.isEmpty()){
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
        AlertDialog.Builder(context)
            .setTitle("BLENDS  1–500")
            .setItems(labels){_,index->
                val item = blends[index]
                if(sampler.activateCombiAddress(0,0,item.first)){
                    editingCombiSlot = item.first
                    page = Page.COMBI
                    refreshAllUppers()
                    refreshFx()
                    invalidate()
                }
            }
            .setNegativeButton("CANCEL", null)
            .show()
    }

    private fun showWriteCombi(addToList:Boolean){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("COMBI NAME")); val name = editorField(sampler.combiName(), false); body.addView(name)
        body.addView(editorLabel("COMBI POSITION   1 … 500"))
        val defaultSlot = editingCombiSlot ?: sampler.firstFreeCombiSlot()
        val slot = editorField(defaultSlot.toString()); body.addView(slot)
        val title = if(addToList) "WRITE COMBI + ADD TO LIST" else "WRITE COMBI"
        val subtitle = if(addToList) "Save the complete 7-UPPER COMBI, then add a LIST reference." else "Save the complete 7-UPPER COMBI to a fixed position."
        showEditorPage(title, subtitle, body, if(addToList) "WRITE + ADD" else "WRITE") {
            val position = slot.text.toString().toIntOrNull()?.coerceIn(1,500) ?: defaultSlot
            val finalName = name.text.toString().trim().ifBlank { sampler.combiName() }
            if(sampler.writeCombiSlot(position,finalName)){
                editingCombiSlot = position; refreshAllUppers(); refreshFx()
                if(addToList && sampler.addCombiToList(position,finalName)){
                    val size = sampler.listItems(0,0,0).size
                    listPage = ((size-1).coerceAtLeast(0)/10).coerceIn(0,19); page = Page.LIST
                }
            }
            invalidate()
        }
    }


    private fun showSoundPicker(upper:Int){
        val sounds=sampler.soundCatalog()
        if(sounds.isEmpty()){
            val detail=sampler.bankLoadError().ifBlank{
                "SILVETON_BANK.sv не е заредена или няма SOUND позиции."
            }
            AlertDialog.Builder(context)
                .setTitle("BANK HAS NO SOUNDS")
                .setMessage(detail)
                .setPositiveButton("OK",null)
                .show()
            return
        }
        val labels=ArrayList<String>(sounds.size+1)
        labels.add("EMPTY")
        sounds.forEach{ item -> labels.add("%03d   %s".format(item.slot,item.name)) }
        AlertDialog.Builder(context).setTitle("UPPER ${upper+1} — SOUND").setItems(labels.toTypedArray()){d,which->
            if(which==0) sampler.clearUpperSound(upper) else sampler.assignUpperSound(upper,sounds[which-1])
            refreshUpper(upper); d.dismiss(); invalidate()
        }.setNegativeButton("CANCEL",null).show()
    }

    private fun showKeyRange(upper:Int,s:SamplerAppController.UpperState){
        showTwoInts("KEY RANGE", "Low 0-127", s.keyLow, "High 0-127", s.keyHigh, 0,127){lo,hi-> if(lo<=hi) commitMain(upper,s.copy(keyLow=lo,keyHigh=hi))}
    }

    private fun showVelocityRange(upper:Int,s:SamplerAppController.UpperState){
        showTwoInts("VELOCITY RANGE", "Low 1-127", s.velocityLow, "High 1-127", s.velocityHigh,1,127){lo,hi-> if(lo<=hi) commitRange(s.copy(velocityLow=lo,velocityHigh=hi))}
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

    private fun showEditorPage(title: String, subtitle: String, body: LinearLayout, applyLabel: String = "APPLY", onApply: () -> Unit) {
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
        cancel.setOnClickListener { dialog.dismiss() }
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

    private fun showTwoInts(title:String,l1:String,v1:Int,l2:String,v2:Int,minV:Int,maxV:Int,done:(Int,Int)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel(l1)); val a = editorField(v1.toString()); body.addView(a)
        body.addView(editorLabel(l2)); val b = editorField(v2.toString()); body.addView(b)
        showEditorPage(title, "Allowed range: $minV … $maxV", body) {
            val x = a.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v1
            val y = b.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v2
            done(x,y); invalidate()
        }
    }


    private fun showNumber(title:String,current:Float,minV:Float,maxV:Float,decimals:Int,done:(Float)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        fun formatEditorValue(value: Float): String = if (decimals == 0) kotlin.math.round(value).toInt().toString() else "% .${decimals}f".format(value).trim()
        val scale = when { decimals <= 0 -> 1; decimals == 1 -> 10; else -> 100 }
        val sliderMax = max(1, kotlin.math.round((maxV - minV) * scale).toInt())
        body.addView(editorLabel("VALUE"))
        val edit = editorField(formatEditorValue(current.coerceIn(minV, maxV)))
        body.addView(edit)
        body.addView(editorLabel("RANGE   ${formatEditorValue(minV)}  …  ${formatEditorValue(maxV)}"))
        var syncing = false
        val seek = SeekBar(context).apply {
            max = sliderMax
            progress = kotlin.math.round((current.coerceIn(minV, maxV) - minV) * scale).toInt().coerceIn(0, sliderMax)
        }
        fun setEditorValue(value: Float) {
            val clamped = value.coerceIn(minV, maxV)
            syncing = true
            val newText = formatEditorValue(clamped)
            edit.setText(newText)
            edit.setSelection(newText.length)
            seek.progress = kotlin.math.round((clamped - minV) * scale).toInt().coerceIn(0, sliderMax)
            syncing = false
        }
        seek.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                if (!fromUser || syncing) return
                setEditorValue(minV + progress.toFloat() / scale.toFloat())
            }
            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        body.addView(seek)
        val fine = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; setPadding(0, uiPx(8f), 0, 0) }
        val step = 1f / scale.toFloat()
        fun currentTyped(): Float = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
        val minus = Button(context).apply { text = "−"; textSize = 20f; setOnClickListener { setEditorValue(currentTyped() - step) } }
        val plus = Button(context).apply { text = "+"; textSize = 20f; setOnClickListener { setEditorValue(currentTyped() + step) } }
        fine.addView(minus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        fine.addView(plus, LinearLayout.LayoutParams(0, uiPx(48f), 1f))
        body.addView(fine)
        showEditorPage(title, "Drag smoothly or type the exact value.", body) {
            val value = edit.text.toString().replace(',', '.').toFloatOrNull()?.coerceIn(minV, maxV) ?: current.coerceIn(minV, maxV)
            done(value)
            invalidate()
        }
    }


    private fun commitMain(i:Int,s:SamplerAppController.UpperState){
        upperStates[i]=s
        if(!sampler.updateUpperMain(i,s)) refreshUpper(i)
    }
    private fun commitTune(s:SamplerAppController.UpperState){
        upperStates[selectedUpper]=s
        if(!sampler.updateUpperTune(selectedUpper,s)) refreshUpper(selectedUpper)
    }
    private fun commitRange(s:SamplerAppController.UpperState){
        upperStates[selectedUpper]=s
        if(!sampler.updateUpperRange(selectedUpper,s)) refreshUpper(selectedUpper)
    }
    private fun commitControl(s:SamplerAppController.UpperState){
        upperStates[selectedUpper]=s
        if(!sampler.updateUpperControl(selectedUpper,s)) refreshUpper(selectedUpper)
    }
    private fun commitTone(s:SamplerAppController.UpperState){
        upperStates[selectedUpper]=s
        if(!sampler.updateUpperTone(selectedUpper,s)) refreshUpper(selectedUpper)
    }
    private fun commitUpperFx(s:SamplerAppController.UpperState){
        upperStates[selectedUpper]=s
        if(!sampler.updateUpperFx(selectedUpper,s)) refreshUpper(selectedUpper)
    }
    private fun commitGlobal(g:SamplerAppController.GlobalUiState){
        globalState=g
        if(!sampler.updateGlobalUi(g)) refreshGlobal()
    }

    private fun previewFx(slot:Int, fx:SamplerAppController.FxState, final:Boolean){
        val current=fxState ?: return
        fxState=if(slot==1) current.copy(fx1=fx) else current.copy(fx2=fx)
        if(final && !sampler.updateCombiFx(slot,fx)) refreshFx()
    }

    private fun refreshUpper(i:Int){upperStates[i]=sampler.upperState(i)}
    private fun refreshAllUppers(){for(i in 0 until 7)refreshUpper(i)}
    private fun refreshFx(){fxState=sampler.combiFxState()}
    private fun refreshGlobal(){globalState=sampler.globalUiState()}
    /** MASTER changed from outside the screen (tablet volume keys). */
    fun globalChangedOutside(){refreshGlobal();invalidate()}
    private fun refreshListCache(){
        cachedListItems=sampler.listItems(0,0,0)
        cachedActiveListEntry=sampler.activeListEntry(0,0,0)
        listCacheLoaded=true
    }

    private fun hit(r:RectF,tap:(Float,Float)->Unit,reset:(()->Unit)?=null,drag:((Float,Float,Boolean)->Unit)?=null){
        hits.add(Hit(RectF(r),tap,reset,drag))
    }
    private fun hitAt(x:Float,y:Float):Hit?=hits.asReversed().firstOrNull{it.rect.contains(x,y)}

    private fun listLongHit(r: RectF, action: (Float, Float) -> Unit) {
        listLongHits.add(ListLongHit(RectF(r), action))
    }
    private fun listLongHitAt(x: Float, y: Float): ListLongHit? =
        listLongHits.asReversed().firstOrNull { it.rect.contains(x, y) }

    private fun globalSliderSpec(i:Int,g:SamplerAppController.GlobalUiState):Triple<Float,Float,Float> = when(i){
        0->Triple(g.masterTuneHz,400f,480f);1->Triple(g.transpose.toFloat(),-48f,48f);2->Triple(g.masterGainDb,-60f,18f)
        3->Triple(g.eqLowDb,-18f,18f);4->Triple(g.eqMidDb,-18f,18f);5->Triple(g.eqHighDb,-18f,18f)
        7->Triple(g.limiterCeilingDb,-12f,0f);8->Triple(g.velocityCurve.toFloat(),0f,4f)
        9->Triple(g.breathCc.toFloat(),0f,127f);10->Triple(g.expressionCc.toFloat(),0f,127f);11->Triple(g.damperCc.toFloat(),0f,127f)
        else->Triple(0f,0f,1f)
    }
    private fun globalWithSliderValue(i:Int,g:SamplerAppController.GlobalUiState,v:Float):SamplerAppController.GlobalUiState=when(i){
        0->g.copy(masterTuneHz=v);1->g.copy(transpose=kotlin.math.round(v).toInt());2->g.copy(masterGainDb=v)
        3->g.copy(eqLowDb=v);4->g.copy(eqMidDb=v);5->g.copy(eqHighDb=v);7->g.copy(limiterCeilingDb=v)
        8->g.copy(velocityCurve=kotlin.math.round(v).toInt());9->g.copy(breathCc=kotlin.math.round(v).toInt())
        10->g.copy(expressionCc=kotlin.math.round(v).toInt());11->g.copy(damperCc=kotlin.math.round(v).toInt());else->g
    }
    private fun globalValueText(i:Int,g:SamplerAppController.GlobalUiState):String=when(i){
        0->"%.1f Hz".format(g.masterTuneHz);1->g.transpose.toString();2->"%.1f dB".format(g.masterGainDb)
        3->"%.1f dB".format(g.eqLowDb);4->"%.1f dB".format(g.eqMidDb);5->"%.1f dB".format(g.eqHighDb)
        7->"%.1f dB".format(g.limiterCeilingDb);8->g.velocityCurve.toString();9->g.breathCc.toString();10->g.expressionCc.toString();11->g.damperCc.toString();else->""
    }

    private fun drawSilvetonBackground(c: Canvas, w: Float, h: Float) {
        paint.style=Paint.Style.FILL
        paint.shader=null
        paint.color=C_BG
        c.drawRect(0f,0f,w,h,paint)
    }

    private fun silvCard(c: Canvas, r: RectF, base: Int, radius: Float) {
        paint.shader=null
        paint.style=Paint.Style.FILL
        paint.color=base
        c.drawRoundRect(r,radius,radius,paint)
        paint.style=Paint.Style.STROKE
        paint.strokeWidth=1f.dp
        paint.color=C_BORDER
        c.drawRoundRect(r,radius,radius,paint)
        paint.style=Paint.Style.FILL
    }

    private fun silvKey(c: Canvas, r: RectF, active: Boolean, red: Boolean, enabled: Boolean = true) {
        val radius=min(r.width(),r.height())*0.13f
        paint.shader=null
        paint.style=Paint.Style.FILL
        paint.color=when{
            !enabled->Color.rgb(22,26,31)
            red->if(active) Color.rgb(120,38,42) else Color.rgb(61,29,31)
            active->C_BUTTON_ACTIVE
            else->C_BUTTON
        }
        c.drawRoundRect(r,radius,radius,paint)
        paint.style=Paint.Style.STROKE
        paint.strokeWidth=if(active)2f.dp else 1f.dp
        paint.color=when{red->C_PANIC;active->C_ACCENT;else->C_BORDER}
        c.drawRoundRect(r,radius,radius,paint)
        paint.style=Paint.Style.FILL
    }

    private fun silvLed(c: Canvas, x: Float, y: Float, on: Boolean, red: Boolean, radius: Float) {
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
    }

    private fun silvStatus(c: Canvas, r: RectF, label: String, value: String, on: Boolean, sx: Float, sy: Float) {
        val ss = min(sx, sy)
        paint.shader=null
        paint.style=Paint.Style.FILL
        paint.color=C_SUBPANEL
        c.drawRoundRect(r,r.height()*0.5f,r.height()*0.5f,paint)
        paint.style=Paint.Style.STROKE
        paint.strokeWidth=1f*ss
        paint.color=if(on) C_ACCENT_DARK else C_BORDER
        c.drawRoundRect(r,r.height()*0.5f,r.height()*0.5f,paint)
        paint.style=Paint.Style.FILL
        if(label=="MIDI" || label=="AUDIO"){
            silvLed(c,r.left+18f*sx,r.centerY(),on,false,if(on)6.5f*ss else 5.5f*ss)
            text(c,label,r.left+34f*sx,r.centerY()-3f*sy,10f*ss,C_TEXT_MUTED,bold=true)
            text(c,value,r.left+34f*sx,r.centerY()+14f*sy,15f*ss,if(on)C_TEXT else C_LABEL,bold=on)
        }else{
            text(c,label,r.left+13f*sx,r.centerY()-3f*sy,10f*ss,C_TEXT_MUTED,bold=true)
            text(c,value,r.left+13f*sx,r.centerY()+14f*sy,17f*ss,C_TEXT,bold=true)
        }
    }
    private fun midiNoteName(note: Int): String {
        val n = note.coerceIn(0,127)
        val names = arrayOf("C","C#","D","D#","E","F","F#","G","G#","A","A#","B")
        return names[n%12] + (n/12 - 1)
    }

    private fun colorWithAlpha(color: Int, alpha: Int): Int = Color.argb(alpha, Color.red(color), Color.green(color), Color.blue(color))

    private fun lighten(color: Int, amount: Int): Int = Color.rgb(
        min(255, Color.red(color)+amount), min(255, Color.green(color)+amount), min(255, Color.blue(color)+amount))
    private fun darken(color: Int, amount: Int): Int = Color.rgb(
        max(0, Color.red(color)-amount), max(0, Color.green(color)-amount), max(0, Color.blue(color)-amount))

    private fun panel(c:Canvas,r:RectF,color:Int,rad:Float,border:Int=C_BORDER){paint.style=Paint.Style.FILL;paint.color=color;c.drawRoundRect(r,rad,rad,paint);paint.style=Paint.Style.STROKE;paint.strokeWidth=1f.dp;paint.color=border;c.drawRoundRect(r,rad,rad,paint);paint.style=Paint.Style.FILL}
    private fun button(c:Canvas,r:RectF,label:String,active:Boolean,accent:Int=C_ACCENT){silvKey(c,r,active,accent==C_PANIC);text(c,label,r.centerX(),r.centerY()+5f.dp,12f.dp,if(active)C_TEXT else C_TEXT_DIM,alignCenter=true,bold=active,maxWidth=r.width()-14f.dp)}
    private fun tinyToggle(c:Canvas,r:RectF,label:String,on:Boolean){panel(c,r,if(on)C_BUTTON_ACTIVE else C_BUTTON,6f.dp,if(on)C_ACCENT else C_BORDER);text(c,label,r.centerX(),r.centerY()+4f.dp,10f.dp,if(on)C_TEXT else C_TEXT_MUTED,alignCenter=true,bold=on,maxWidth=r.width()-8f.dp)}
    private fun chip(c:Canvas,x:Float,y:Float,label:String,h:Float):Float{paint.textSize=11f.dp;val ww=paint.measureText(label)+25f.dp;val r=RectF(x,y,x+ww,y+h);panel(c,r,C_CHIP,999f, C_BORDER);text(c,label,r.centerX(),r.centerY()+4f.dp,11f.dp,C_TEXT_DIM,alignCenter=true);return r.right}
    private fun text(c:Canvas,s:String,x:Float,y:Float,size:Float,color:Int,alignCenter:Boolean=false,bold:Boolean=false,maxWidth:Float=Float.POSITIVE_INFINITY){
        paint.color=color
        paint.textSize=size
        paint.typeface=if(bold)Typeface.create("sans-serif-medium",Typeface.NORMAL) else Typeface.create("sans-serif",Typeface.NORMAL)
        paint.textAlign=if(alignCenter)Paint.Align.CENTER else Paint.Align.LEFT
        var shown=s
        if(maxWidth != Float.POSITIVE_INFINITY && maxWidth>1f){
            val measured=paint.measureText(shown)
            if(measured>maxWidth){
                val minSize=max(8f.dp,size*0.72f)
                paint.textSize=max(minSize,size*(maxWidth/measured))
                if(paint.measureText(shown)>maxWidth){
                    val ell="…"
                    val room=max(0f,maxWidth-paint.measureText(ell))
                    var count=shown.length
                    while(count>0 && paint.measureText(shown,0,count)>room){ count-- }
                    shown=if(count<=0)ell else shown.take(count)+ell
                }
            }
        }
        c.drawText(shown,x,y,paint)
        paint.textAlign=Paint.Align.LEFT
    }
    private fun line(c:Canvas,x1:Float,y1:Float,x2:Float,y2:Float,color:Int,w:Float){paint.color=color;paint.strokeWidth=w;c.drawLine(x1,y1,x2,y2,paint)}
    private fun drawKnob(c:Canvas,cx:Float,cy:Float,r:Float){paint.style=Paint.Style.FILL;paint.color=C_KNOB;c.drawCircle(cx,cy,r,paint);paint.style=Paint.Style.STROKE;paint.strokeWidth=2f.dp;paint.color=C_ACCENT;c.drawArc(RectF(cx-r,cy-r,cx+r,cy+r),135f,225f,false,paint);paint.style=Paint.Style.FILL;line(c,cx,cy,cx,cy-r*0.7f,C_TEXT,2f.dp)}
    private fun drawMiniSlider(c:Canvas,r:RectF,value:Float,minV:Float,maxV:Float,label:String){text(c,label,r.left,r.centerY()+4f.dp,10f.dp,C_TEXT);val bar=RectF(r.left+50f.dp,r.centerY()-3f.dp,r.right-5f.dp,r.centerY()+3f.dp);paint.color=C_TRACK;c.drawRoundRect(bar,3f.dp,3f.dp,paint);val t=((value-minV)/(maxV-minV)).coerceIn(0f,1f);val x=bar.left+bar.width()*t;paint.color=C_ACCENT;c.drawRoundRect(RectF(bar.left,bar.top,x,bar.bottom),3f.dp,3f.dp,paint);paint.color=C_KNOB_LIGHT;c.drawCircle(x,bar.centerY(),8f.dp,paint)}
    private fun drawParamSlider(c:Canvas,r:RectF,value:Float,minV:Float,maxV:Float,label:String,valueText:String){
        text(c,label,r.centerX(),r.top+22f.dp,9f.dp,C_TEXT_MUTED,alignCenter=true,bold=true)
        val bar=RectF(r.left+14f.dp,r.centerY()-4f.dp,r.right-14f.dp,r.centerY()+4f.dp)
        paint.shader=null; paint.style=Paint.Style.FILL; paint.color=C_TRACK
        c.drawRoundRect(bar,4f.dp,4f.dp,paint)
        val t=if(maxV>minV)((value-minV)/(maxV-minV)).coerceIn(0f,1f) else 0f
        val x=bar.left+bar.width()*t
        paint.color=C_ACCENT_DARK; c.drawRoundRect(RectF(bar.left,bar.top,x,bar.bottom),4f.dp,4f.dp,paint)
        paint.color=C_KNOB_LIGHT; c.drawCircle(x,bar.centerY(),9f.dp,paint)
        paint.style=Paint.Style.STROKE; paint.strokeWidth=1.5f.dp; paint.color=C_ACCENT; c.drawCircle(x,bar.centerY(),9f.dp,paint); paint.style=Paint.Style.FILL
        text(c,valueText,r.centerX(),r.bottom-12f.dp,11f.dp,C_TEXT,alignCenter=true,bold=true)
    }
    private fun sliderValueFromX(x:Float,r:RectF,minV:Float,maxV:Float):Float{
        val l=r.left+14f.dp; val rr=r.right-14f.dp
        val t=if(rr>l)((x-l)/(rr-l)).coerceIn(0f,1f) else 0f
        return minV+(maxV-minV)*t
    }
    private fun miniSliderValueFromX(x:Float,r:RectF,minV:Float,maxV:Float):Float{
        val l=r.left+50f.dp; val rr=r.right-5f.dp
        val t=if(rr>l)((x-l)/(rr-l)).coerceIn(0f,1f) else 0f
        return minV+(maxV-minV)*t
    }

    private fun drawUpperMeter(c:Canvas,r:RectF,peak:Float){paint.color=C_TRACK;c.drawRoundRect(r,3f.dp,3f.dp,paint);val db=if(peak<=0.000001f)-60f else (20f*kotlin.math.log10(peak)).coerceIn(-60f,6f);val t=((db+60f)/66f).coerceIn(0f,1f);paint.color=when{db>-3f->C_METER_RED;db>-12f->C_METER_YELLOW;else->C_METER_GREEN};c.drawRoundRect(RectF(r.left,r.top,r.left+r.width()*t,r.bottom),3f.dp,3f.dp,paint)}
    private fun drawRangeBar(c:Canvas,r:RectF,low:Float,high:Float,minV:Float,maxV:Float){paint.color=C_TRACK;c.drawRoundRect(r,5f.dp,5f.dp,paint);val a=((low-minV)/(maxV-minV)).coerceIn(0f,1f);val b=((high-minV)/(maxV-minV)).coerceIn(0f,1f);paint.color=C_ACCENT;c.drawRoundRect(RectF(r.left+r.width()*a,r.top,r.left+r.width()*b,r.bottom),5f.dp,5f.dp,paint);paint.color=C_KNOB_LIGHT;c.drawCircle(r.left+r.width()*a,r.centerY(),7f.dp,paint);c.drawCircle(r.left+r.width()*b,r.centerY(),7f.dp,paint)}
    private fun mapX(x:Float,r:RectF,minV:Float,maxV:Float):Float=minV+(maxV-minV)*((x-r.left)/r.width()).coerceIn(0f,1f)
    private fun voiceModeName(m:Int)=when(m){1->"MONO";2->"LEGATO";else->"POLY"}
    private fun effectName(t:Int)=when(t){1->"DELAY";2->"CHORUS";3->"REVERB";else->"OFF"}
    private fun panText(p:Float)=when{p<-0.02f->"L${(-p*100).toInt()}";p>0.02f->"R${(p*100).toInt()}";else->"C"}
    private fun formatValue(v:Float,unit:String)=if(unit.isBlank())"${v.toInt()}" else if(unit=="ms")"${v.toInt()} $unit" else "%.1f %s".format(v,unit)
    private fun noteName(key:Int):String{val names=arrayOf("C","C#","D","D#","E","F","F#","G","G#","A","A#","B");val k=key.coerceIn(0,127);return names[k%12]+(k/12-1)}
    private val Float.dp get()=this*resources.displayMetrics.density

    companion object{
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
    }
}