package com.silveton.sampler

import android.content.Context
import android.net.Uri
import java.io.File
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Technical Android host for the SILVETON SAMPLER engine.
 *
 * This class intentionally contains no GUI. The final LIST/COMBI/PROGRAM/
 * SAMPLE/GLOBAL/MEDIA screens call this host, while audio and MIDI stay owned
 * by the native/runtime layer.
 */
class SamplerAppController(
    context: Context,
    private val statusChanged: (String?) -> Unit = {},
    private val midiConnectionChanged: (Int) -> Unit = {}
) : AutoCloseable {
    companion object {
        const val TONE_ATTACK = 0
        const val TONE_DECAY = 1
        const val TONE_SUSTAIN = 2
        const val TONE_RELEASE = 3
        const val TONE_CUTOFF = 4
        const val TONE_RESONANCE = 5
        const val TONE_MAX_TIME_MS = 20000f
        const val TONE_CUTOFF_OPEN_HZ = 20000f
    }

    data class MidiStatus(
        val deviceCount: Int,
        val lastError: Int,
        val activitySerial: Long,
        val status: Int,
        val data1: Int,
        val data2: Int
    )

    data class AudioStatus(
        val sampleRate: Int,
        val framesPerBurst: Int,
        val bufferSizeFrames: Int,
        val xrunCount: Int,
        val sharingMode: Int,
        val performanceMode: Int,
        val deviceId: Int,
        val estimatedLatencyMs: Double,
        val peakLeft: Float,
        val peakRight: Float,
        val limiterGainReductionDb: Float,
        val activeVoices: Int,
        val droppedRealtimeEvents: Int
    )


    data class SoundItem(val bank: Int, val page: Int, val slot: Int, val name: String)

    data class UpperState(
        val enabled: Boolean, val solo: Boolean, val mute: Boolean,
        val levelDb: Float, val pan: Float, val keyLow: Int, val keyHigh: Int,
        val velocityLow: Int, val velocityHigh: Int, val voiceMode: Int,
        val octave: Int, val transpose: Int, val fineCents: Float,
        val pitchBendRange: Int, val portamentoSec: Float,
        val breath: Boolean, val expression: Boolean, val aftertouch: Boolean, val damper: Boolean,
        val midiChannel: Int, val attackScale: Float, val decayScale: Float,
        val sustainScale: Float, val releaseScale: Float, val cutoffOctaves: Float,
        val resonanceOffset: Float, val lfoRateScale: Float, val lfoDepthScale: Float,
        val eqLowDb: Float, val eqMidDb: Float, val eqHighDb: Float,
        val fxSend1: Float, val fxSend2: Float, val soundName: String,
        // TONE page (absolute values). *Set = the Upper has its own value, otherwise the PROGRAM default is shown.
        val toneAttackMs: Float = 5f, val toneDecayMs: Float = 80f, val toneSustain: Float = 1f,
        val toneReleaseMs: Float = 120f, val toneCutoffHz: Float = 20000f, val toneResonance: Float = 0f,
        val toneSet: Int = 0
    )


    data class FxState(
        val type: Int, val wet: Float, val timeMs: Float, val feedback: Float,
        val rateHz: Float, val depth: Float, val tone: Float
    )

    data class CombiFxState(val fx1: FxState, val fx2: FxState, val routing: Int)

    data class GlobalUiState(
        val masterTuneHz: Float, val transpose: Int, val masterGainDb: Float,
        val eqLowDb: Float, val eqMidDb: Float, val eqHighDb: Float,
        val limiterEnabled: Boolean, val limiterCeilingDb: Float, val velocityCurve: Int,
        val breathCc: Int, val expressionCc: Int, val damperCc: Int,
        val pitchBendEnabled: Boolean, val aftertouchEnabled: Boolean
    )

    private val appContext = context.applicationContext
    val engine = NativeSamplerEngine()
    val media = SamplerMediaStorage(appContext)
    private val runtime = SamplerRuntime(engine, statusChanged)
    private val midi = SamplerMidiController(
        appContext,
        engine,
        connectionChanged = midiConnectionChanged,
        statusChanged = statusChanged
    )
    private val started = AtomicBoolean(false)
    private val closed = AtomicBoolean(false)
    private var defaultBankAttempted = false
    private val bankLoading = AtomicBoolean(false)

    // PORTA BALANCE, like the SILVETON keyboard processor: one number for the
    // whole sampler, kept between sessions (3..40, default 11).
    private val portaPrefs = appContext.getSharedPreferences("silveton_sampler", android.content.Context.MODE_PRIVATE)
    private var portaBalanceValue = portaPrefs.getInt("porta_balance", 11).coerceIn(3, 40)

    fun portaBalance(): Int = portaBalanceValue

    fun setPortaBalance(value: Int, save: Boolean = true) {
        portaBalanceValue = value.coerceIn(3, 40)
        engine.nativeSetPortaBalance(portaBalanceValue)
        if (save) portaPrefs.edit().putInt("porta_balance", portaBalanceValue).apply()
    }

    /** Start low-latency output first, then accept MIDI. */
    fun start(): Boolean {
        if (closed.get()) return false
        engine.nativeSetPortaBalance(portaBalanceValue)
        if (started.get()) return true
        midi.start()
        val audioOk = runtime.start()
        // The bank must be found even when audio needs another try.
        startBankLoad()
        if (!audioOk) return false
        started.set(true)
        return true
    }

    private fun startBankLoad() {
        if (!bankLoading.compareAndSet(false, true)) return
        Thread({
            try { loadDefaultBankOnce() } finally { bankLoading.set(false) }
        }, "SilvetonBankLoad").apply {
            priority = Thread.NORM_PRIORITY - 1
            start()
        }
    }

    private fun loadDefaultBankOnce() {
        if (defaultBankAttempted) return
        // The native sampler survives an Activity restart; keep its bank and BLENDS.
        if (engine.nativeSoundCatalogNames().isNotEmpty()) {
            defaultBankAttempted = true
            statusChanged(null)
            return
        }

        val access = SilvetonRootRuntime.refreshBankStorageAccess()
        val sharedRoot = File("/storage/emulated/0/SILVETON_BANK.sv")
        val sharedDownload = File("/storage/emulated/0/Download/SILVETON_BANK.sv")
        val appExternal = appContext.getExternalFilesDir(null)?.let { File(it, "SILVETON_BANK.sv") }
        val appInternal = File(appContext.filesDir, "SILVETON_BANK.sv")

        var bank: File? = null
        repeat(3) { attempt ->
            bank = listOfNotNull(sharedRoot, sharedDownload, appExternal, appInternal)
                .firstOrNull { it.isFile && it.canRead() }
            if (bank != null) return@repeat
            if (attempt < 2) try { Thread.sleep(120L) } catch (_: InterruptedException) { return }
        }

        val selected = bank
        if (selected == null) {
            val detail = if (access.ok) "all-files access granted" else access.message
            statusChanged("SILVETON_BANK.sv is not readable ($detail). Expected /storage/emulated/0/SILVETON_BANK.sv")
            return
        }
        if (!engine.nativeLoadSvBank(selected.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "SILVETON_BANK.sv load failed: ${selected.absolutePath}" })
            return
        }
        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        if (userState.exists() && !engine.nativeLoadUserPerformanceState(userState.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "BLENDS/LIST user state load failed" })
            return
        }
        val soundCount = engine.nativeSoundCatalogNames().size
        if (soundCount <= 0) {
            statusChanged("SILVETON_BANK.sv loaded but contains no visible SOUND positions")
            return
        }
        defaultBankAttempted = true
        statusChanged(null)
    }

    fun pauseAudio() {
        if (!closed.get() && started.get()) runtime.suspendAudio()
    }

    fun resumeAudio(): Boolean {
        if (closed.get()) return false
        if (!started.get()) return start()
        if (!defaultBankAttempted) startBankLoad()
        return runtime.resumeAudio()
    }

    fun midiStatus(): MidiStatus {
        val msg = engine.nativeMidiLastMessage()
        return MidiStatus(
            deviceCount = engine.nativeMidiDeviceCount().coerceAtLeast(0),
            lastError = engine.nativeMidiLastError(),
            activitySerial = engine.nativeMidiActivitySerial(),
            status = msg.getOrElse(0) { 0 },
            data1 = msg.getOrElse(1) { 0 },
            data2 = msg.getOrElse(2) { 0 }
        )
    }

    fun audioRunning(): Boolean = !closed.get() && engine.nativeAudioStarted()
    fun audioLastError(): Int = engine.nativeLastAudioError()
    fun testInstrumentEnabled(): Boolean = !closed.get() && engine.nativeTestInstrumentEnabled()
    fun testEpNoteOn(note: Int = 60, velocity: Int = 110): Boolean =
        !closed.get() && engine.nativeTestNoteOn(note.coerceIn(0,127), velocity.coerceIn(1,127))
    fun testEpNoteOff(note: Int = 60) { if (!closed.get()) engine.nativeTestNoteOff(note.coerceIn(0,127)) }

    fun panic(): Boolean = !closed.get() && engine.nativePanic()

    fun loadAudioFile(path: File): Boolean = nativeResult(engine.nativeLoadAudio(path.absolutePath))
    fun loadKmp(path: File): Boolean = nativeResult(engine.nativeLoadKmp(path.absolutePath))
    fun loadSvBank(path: File): Boolean = nativeResult(engine.nativeLoadSvBank(path.absolutePath))
    fun loadSet(path: File): Boolean = nativeResult(engine.nativeLoadSet(path.absolutePath))

    fun importKmpToLibrary(path: File): IntArray {
        val ids = engine.nativeImportKmpToLibrary(path.absolutePath)
        if (ids.isEmpty()) statusChanged(engine.nativeLastTextError().ifBlank { "KMP import failed" }) else statusChanged(null)
        return ids
    }

    fun importKscToLibrary(path: File): IntArray {
        val ids = engine.nativeImportKscToLibrary(path.absolutePath)
        if (ids.isEmpty()) statusChanged(engine.nativeLastTextError().ifBlank { "KSC import failed" }) else statusChanged(null)
        return ids
    }

    fun multisampleCatalog(): List<Pair<Int, String>> {
        val ids = engine.nativeMultisampleIds()
        val names = engine.nativeMultisampleNames()
        val count = minOf(ids.size, names.size)
        return List(count) { index -> ids[index] to names[index] }
    }

    data class ListItem(
        val number: Int,
        val name: String,
        val combiBank: Int,
        val combiPage: Int,
        val combiSlot: Int
    )

    fun listItems(bank: Int, page: Int, slot: Int): List<ListItem> {
        val numbers = engine.nativeListEntryNumbers(bank, page, slot)
        val names = engine.nativeListEntryNames(bank, page, slot)
        val addresses = engine.nativeListEntryCombiAddresses(bank, page, slot)
        val count = minOf(numbers.size, names.size, addresses.size / 3)
        return List(count) { index ->
            val base = index * 3
            ListItem(numbers[index], names[index], addresses[base], addresses[base + 1], addresses[base + 2])
        }
    }

    fun activeListEntry(bank: Int, page: Int, slot: Int): Int =
        engine.nativeActiveListEntry(bank, page, slot)

    fun activateListEntry(bank: Int, page: Int, slot: Int, index: Int): Boolean =
        nativeResult(engine.nativeActivateListEntry(bank, page, slot, index))


    private fun persistLiveListEdits(): Boolean {
        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        return nativeResult(engine.nativeSaveUserPerformanceState(userState.absolutePath))
    }

    private fun renumberLiveList(): Boolean {
        val items = listItems(0, 0, 0)
        for (i in items.indices) {
            val item = items[i]
            val expected = i + 1
            if (item.number == expected) continue
            if (!engine.nativeSetListEntry(
                    0, 0, 0, i, expected, item.name,
                    item.combiBank, item.combiPage, item.combiSlot
                )) return nativeResult(false)
        }
        return true
    }

    fun renameLiveListEntry(index: Int, name: String): Boolean {
        val item = listItems(0, 0, 0).getOrNull(index)?.takeUnless { isEmptyListItem(it) } ?: return fail("LIST entry is not available")
        val finalName = name.trim()
        if (finalName.isEmpty()) return fail("LIST name cannot be empty")
        if (!nativeResult(engine.nativeSetListEntry(
                0, 0, 0, index, item.number, finalName,
                item.combiBank, item.combiPage, item.combiSlot
            ))) return false
        return persistLiveListEdits()
    }

    // SILVETON_LIST_POSITIONS_V1: fixed LIST positions 1..200 like Korg Performances.
    // An empty position is an entry with COMBI address 0/0/0; empty positions at the
    // end are removed, so the stored LIST only grows as far as the last BLEND.
    fun isEmptyListItem(item: ListItem?): Boolean =
        item == null || (item.combiBank == 0 && item.combiPage == 0 && item.combiSlot == 0)

    private fun ensureListLength(count: Int): Boolean {
        var items = listItems(0, 0, 0)
        if (items.size >= count) return true
        if (items.isEmpty()) {
            if (!nativeResult(engine.nativeWriteList(0, 0, 0, "LIVE", intArrayOf(1), arrayOf("-"), intArrayOf(0, 0, 0)))) return false
            items = listItems(0, 0, 0)
        }
        for (i in items.size until count) {
            if (!nativeResult(engine.nativeInsertListEntry(0, 0, 0, i, i + 1, "-", 0, 0, 0))) return false
        }
        return true
    }

    private fun trimListTail(): Boolean {
        var items = listItems(0, 0, 0)
        while (items.isNotEmpty() && isEmptyListItem(items.last())) {
            if (!nativeResult(engine.nativeRemoveListEntry(0, 0, 0, items.size - 1))) return false
            items = listItems(0, 0, 0)
        }
        return true
    }

    /** DELETE: the position becomes empty; no other BLEND moves. */
    fun deleteLiveListEntry(index: Int): Boolean {
        val items = listItems(0, 0, 0)
        if (index !in items.indices || isEmptyListItem(items[index])) return fail("LIST position is empty")
        if (!nativeResult(engine.nativeSetListEntry(0, 0, 0, index, index + 1, "-", 0, 0, 0))) return false
        if (engine.nativeActiveListEntry(0, 0, 0) == index) engine.nativeSetActiveListEntry(0, 0, 0, -1)
        if (!trimListTail()) return false
        return persistLiveListEdits()
    }

    /** COPY to any LIST position 1..200 (an occupied one is overwritten). */
    fun copyListEntryTo(from: Int, to: Int): Boolean {
        val src = listItems(0, 0, 0).getOrNull(from)
        if (isEmptyListItem(src) || src == null) return fail("LIST position is empty")
        if (to !in 0 until 200 || to == from) return fail("LIST target position is invalid")
        if (!ensureListLength(to + 1)) return false
        if (!nativeResult(engine.nativeSetListEntry(0, 0, 0, to, to + 1, src.name, src.combiBank, src.combiPage, src.combiSlot))) return false
        if (engine.nativeActiveListEntry(0, 0, 0) == to) engine.nativeSetActiveListEntry(0, 0, 0, -1)
        return persistLiveListEdits()
    }

    /** MOVE to any LIST position 1..200: the old position becomes empty. */
    fun moveListEntryTo(from: Int, to: Int): Boolean {
        val src = listItems(0, 0, 0).getOrNull(from)
        if (isEmptyListItem(src) || src == null) return fail("LIST position is empty")
        if (to !in 0 until 200 || to == from) return fail("LIST target position is invalid")
        val active = engine.nativeActiveListEntry(0, 0, 0)
        if (!ensureListLength(to + 1)) return false
        if (!nativeResult(engine.nativeSetListEntry(0, 0, 0, to, to + 1, src.name, src.combiBank, src.combiPage, src.combiSlot))) return false
        if (!nativeResult(engine.nativeSetListEntry(0, 0, 0, from, from + 1, "-", 0, 0, 0))) return false
        when (active) {
            from -> engine.nativeSetActiveListEntry(0, 0, 0, to)
            to -> engine.nativeSetActiveListEntry(0, 0, 0, -1)
        }
        if (!trimListTail()) return false
        return persistLiveListEdits()
    }

    fun moveLiveListEntry(fromIndex: Int, toIndex: Int): Boolean {
        val items = listItems(0, 0, 0)
        if (fromIndex !in items.indices || toIndex !in items.indices) return fail("LIST move position is invalid")
        if (fromIndex == toIndex) return true
        if (!nativeResult(engine.nativeMoveListEntry(0, 0, 0, fromIndex, toIndex))) return false
        if (!renumberLiveList()) return false
        return persistLiveListEdits()
    }

    fun copyLiveListEntry(item: ListItem, insertIndex: Int): Boolean {
        val current = listItems(0, 0, 0)
        if (current.size >= 200) return fail("LIST is full")
        val at = insertIndex.coerceIn(0, current.size)
        if (!nativeResult(engine.nativeInsertListEntry(
                0, 0, 0, at, at + 1, item.name,
                item.combiBank, item.combiPage, item.combiSlot
            ))) return false
        if (!renumberLiveList()) return false
        return persistLiveListEdits()
    }

    /** Fixed user COMBI positions are 1..500. */
    fun writeCombiSlot(slot: Int, name: String): Boolean {
        if (slot !in 1..500) return fail("COMBI position must be 1..500")
        val finalName = name.trim().ifBlank { combiName() }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) return false
        if (!nativeResult(engine.nativeActivateCombi(0, 0, slot))) return false
        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        return nativeResult(engine.nativeSaveUserPerformanceState(userState.absolutePath))
    }

    fun firstFreeCombiSlot(): Int {
        for (slot in 1..500) {
            if (!engine.nativeCombiSlotOccupied(0, 0, slot)) return slot
        }
        return 500
    }

    /**
     * SILVETON_KORG_WRITE_V1: Korg-style WRITE of the current 7-UPPER COMBI to
     * LIST position [position] (0-based, as shown 1..200 on screen).
     * An occupied position is overwritten in place; a position after the end
     * becomes the next free one (the LIST has no gaps). The COMBI data goes to
     * the slot that position already owns, or to a free slot, so no other LIST
     * entry and no other BLEND is changed. Returns the written index or -1.
     */
    fun writeCombiToList(position: Int, name: String): Int {
        if (position !in 0 until 200) { fail("LIST position is invalid"); return -1 }
        val target = position
        if (!ensureListLength(target + 1)) { trimListTail(); return -1 }
        val items = listItems(0, 0, 0)
        val finalName = name.trim().ifBlank { combiName() }
        val old = items.getOrNull(target)?.takeUnless { isEmptyListItem(it) }
        fun usedElsewhere(slot: Int): Boolean = items.indices.any { k ->
            k != target && items[k].combiBank == 0 && items[k].combiPage == 0 && items[k].combiSlot == slot
        }
        val slot = if (old != null && old.combiBank == 0 && old.combiPage == 0 &&
            old.combiSlot in 1..500 && !usedElsewhere(old.combiSlot)) {
            old.combiSlot
        } else {
            (1..500).firstOrNull { !engine.nativeCombiSlotOccupied(0, 0, it) } ?: -1
        }
        if (slot !in 1..500) { fail("No free COMBI position (1..500)"); trimListTail(); return -1 }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) { trimListTail(); return -1 }
        if (!nativeResult(engine.nativeSetListEntry(0, 0, 0, target, target + 1, finalName, 0, 0, slot))) { trimListTail(); return -1 }
        if (!nativeResult(engine.nativeActivateListEntry(0, 0, 0, target))) return -1
        if (!persistLiveListEdits()) return -1
        return target
    }

    fun activateCombiAddress(bank: Int, page: Int, slot: Int): Boolean =
        nativeResult(engine.nativeActivateCombi(bank, page, slot))

    /** LIST stores the exact COMBI address instead of a duplicate blend. */
    fun addCombiToList(combiSlot: Int, name: String): Boolean {
        if (combiSlot !in 1..500) return fail("COMBI position must be 1..500")
        val current = listItems(0, 0, 0)
        if (current.size >= 200) return fail("LIST is full")
        val finalName = name.trim().ifBlank { combiName() }
        val nextNumber = (current.maxOfOrNull { it.number } ?: 0) + 1
        val changed = if (current.isEmpty()) {
            nativeResult(engine.nativeWriteList(
                0, 0, 0, "LIVE",
                intArrayOf(nextNumber),
                arrayOf(finalName),
                intArrayOf(0, 0, combiSlot)
            ))
        } else {
            nativeResult(engine.nativeInsertListEntry(
                0, 0, 0, current.size, nextNumber, finalName, 0, 0, combiSlot
            ))
        }
        if (!changed) return false
        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        return nativeResult(engine.nativeSaveUserPerformanceState(userState.absolutePath))
    }

    fun kscNames(path: File): List<String> = engine.nativeKscNames(path.absolutePath).toList()

    fun loadKscItem(path: File, index: Int): Boolean =
        nativeResult(engine.nativeLoadKscItem(path.absolutePath, index))

    /**
     * Materialize one ordinary standalone audio document. KMP/KSC should be
     * imported from a folder tree so their sibling KSF/KMP dependencies remain
     * beside them.
     */
    fun importAudioDocument(uri: Uri): Boolean {
        val local = media.materializeDocument(uri)
        return when (local.extension.lowercase()) {
            "wav", "wave", "aif", "aiff", "aifc" -> loadAudioFile(local)
            else -> fail("Unsupported standalone sample format: ${local.extension}")
        }
    }

    /** Copy a complete Korg folder first so relative KMP/KSF/KSC paths work. */
    fun importKorgTree(treeUri: Uri, folderName: String): File =
        media.materializeTree(treeUri, folderName)

    fun importSetTree(treeUri: Uri, setName: String): Boolean {
        val local = media.materializeSetTree(treeUri, setName)
        return loadSet(local)
    }

    /** Save atomically in app-owned storage. SetPackage performs the native validation. */
    fun saveInternalSet(setName: String): File? {
        val safe = media.safeSetDirectoryName(setName)
        val target = File(media.internalSetsDirectory(), safe)
        return if (nativeResult(engine.nativeSaveSet(target.absolutePath))) target else null
    }

    fun exportSet(source: File, destinationTree: Uri, folderName: String = source.name): Uri =
        media.exportSetDirectory(source, destinationTree, folderName)

    fun upperMeters(): List<Pair<Float, Float>> {
        val meters = engine.nativeMeters()
        return List(7) { index ->
            val base = 5 + index * 2
            meters.getOrElse(base) { 0f } to meters.getOrElse(base + 1) { 0f }
        }
    }

    fun audioStatus(): AudioStatus {
        val info = engine.nativeAudioInfo()
        val meters = engine.nativeMeters()
        return AudioStatus(
            sampleRate = info.getOrElse(0) { 0 },
            framesPerBurst = info.getOrElse(1) { 0 },
            bufferSizeFrames = info.getOrElse(2) { 0 },
            xrunCount = info.getOrElse(3) { 0 },
            sharingMode = info.getOrElse(4) { -1 },
            performanceMode = info.getOrElse(5) { -1 },
            deviceId = info.getOrElse(6) { -1 },
            estimatedLatencyMs = engine.nativeLatencyMs(),
            peakLeft = meters.getOrElse(0) { 0f },
            peakRight = meters.getOrElse(1) { 0f },
            limiterGainReductionDb = meters.getOrElse(2) { 0f },
            activeVoices = meters.getOrElse(3) { 0f }.toInt(),
            droppedRealtimeEvents = meters.getOrElse(4) { 0f }.toInt()
        )
    }

    fun combiName(): String = engine.nativeCombiName().ifBlank { "COMBI" }

    fun bankLoadError(): String = engine.nativeLastTextError()


    fun combiCatalog(): List<Pair<Int, String>> {
        val slots = engine.nativeCombiCatalogSlots()
        val names = engine.nativeCombiCatalogNames()
        val count = minOf(slots.size, names.size)
        return List(count) { index -> slots[index] to names[index] }
    }

    fun soundCatalog(): List<SoundItem> {
        val addresses = engine.nativeSoundCatalogAddresses()
        val names = engine.nativeSoundCatalogNames()
        val count = minOf(names.size, addresses.size / 3)
        return List(count) { index ->
            val base = index * 3
            SoundItem(addresses[base], addresses[base + 1], addresses[base + 2], names[index])
        }
    }

    fun upperState(upper: Int): UpperState? {
        val v = engine.nativeUpperState(upper)
        if (v.size < 33) return null
        return UpperState(
            enabled=v[0] >= 0.5f, solo=v[1] >= 0.5f, mute=v[2] >= 0.5f,
            levelDb=v[3], pan=v[4], keyLow=v[5].toInt(), keyHigh=v[6].toInt(),
            velocityLow=v[7].toInt(), velocityHigh=v[8].toInt(), voiceMode=v[9].toInt(),
            octave=v[10].toInt(), transpose=v[11].toInt(), fineCents=v[12],
            pitchBendRange=v[13].toInt(), portamentoSec=v[14],
            breath=v[15] >= 0.5f, expression=v[16] >= 0.5f, aftertouch=v[17] >= 0.5f, damper=v[18] >= 0.5f,
            midiChannel=v[19].toInt(), attackScale=v[20], decayScale=v[21], sustainScale=v[22], releaseScale=v[23],
            cutoffOctaves=v[24], resonanceOffset=v[25], lfoRateScale=v[26], lfoDepthScale=v[27],
            eqLowDb=v[28], eqMidDb=v[29], eqHighDb=v[30], fxSend1=v[31], fxSend2=v[32],
            soundName=engine.nativeUpperSoundName(upper).ifBlank { "EMPTY" },
            toneAttackMs=v.getOrElse(33) { 5f }, toneDecayMs=v.getOrElse(34) { 80f }, toneSustain=v.getOrElse(35) { 1f },
            toneReleaseMs=v.getOrElse(36) { 120f }, toneCutoffHz=v.getOrElse(37) { 20000f },
            toneResonance=v.getOrElse(38) { 0f },
            toneSet=(0 until 6).fold(0) { bits, k -> if (v.getOrElse(39 + k) { 0f } >= 0.5f) bits or (1 shl k) else bits }
        )
    }

    /** value < 0 clears the setting (PROGRAM default). param: see TONE_* constants. */
    fun setUpperTone(upper: Int, param: Int, value: Float): Boolean =
        nativeResult(engine.nativeSetUpperToneParam(upper, param, value))

    fun assignUpperSound(upper: Int, item: SoundItem): Boolean =
        nativeResult(engine.nativeAssignUpperSound(item.bank, item.page, item.slot, upper))

    fun clearUpperSound(upper: Int): Boolean = nativeResult(engine.nativeClearUpperSound(upper))

    fun updateUpperMain(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperMain(upper, state.enabled, state.solo, state.mute, state.levelDb, state.pan, state.keyLow, state.keyHigh, state.voiceMode)
    )

    fun updateUpperTune(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperTune(upper, state.octave, state.transpose, state.fineCents, state.pitchBendRange, state.portamentoSec)
    )

    fun updateUpperRange(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperRange(upper, state.velocityLow, state.velocityHigh)
    )

    fun updateUpperControl(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperControl(upper, state.breath, state.expression, state.aftertouch, state.damper, state.midiChannel)
    )

    fun updateUpperTone(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperTone(upper, state.attackScale, state.decayScale, state.sustainScale, state.releaseScale,
            state.cutoffOctaves, state.resonanceOffset, state.lfoRateScale, state.lfoDepthScale,
            state.eqLowDb, state.eqMidDb, state.eqHighDb)
    )

    fun updateUpperFx(upper: Int, state: UpperState): Boolean = nativeResult(
        engine.nativeUpdateUpperFxSends(upper, state.fxSend1, state.fxSend2)
    )

    fun combiFxState(): CombiFxState? {
        val v = engine.nativeCombiFxState()
        if (v.size < 15) return null
        fun fx(base: Int) = FxState(v[base].toInt(), v[base+1], v[base+2], v[base+3], v[base+4], v[base+5], v[base+6])
        return CombiFxState(fx(0), fx(7), v[14].toInt())
    }

    /** FX slot 1 = FX1, 2 = FX2 (SILVETON_FX_V2: the native engine counts them 0 and 1). */
    fun updateCombiFx(slot: Int, fx: FxState): Boolean {
        if (slot !in 1..2) return fail("Invalid COMBI FX slot")
        return nativeResult(engine.nativeUpdateCombiFx(slot - 1, fx.type, fx.wet, fx.timeMs, fx.feedback, fx.rateHz, fx.depth, fx.tone))
    }

    fun updateCombiFxRouting(routing: Int): Boolean = nativeResult(engine.nativeUpdateCombiFxRouting(routing))

    fun globalUiState(): GlobalUiState? {
        val v = engine.nativeGlobalState()
        if (v.size < 14) return null
        return GlobalUiState(v[0], v[1].toInt(), v[2], v[3], v[4], v[5], v[6] >= 0.5f, v[7], v[8].toInt(),
            v[9].toInt(), v[10].toInt(), v[11].toInt(), v[12] >= 0.5f, v[13] >= 0.5f)
    }

    /** Tablet volume keys: SILVETON's own MASTER level (same range as SETTINGS). */
    fun nudgeMasterGain(stepDb: Float): Float? {
        val g = globalUiState() ?: return null
        val next = (g.masterGainDb + stepDb).coerceIn(-60f, 18f)
        if (!updateGlobalUi(g.copy(masterGainDb = next))) return null
        return next
    }

    fun updateGlobalUi(state: GlobalUiState): Boolean = nativeResult(
        engine.nativeUpdateGlobal(state.masterTuneHz, state.transpose, state.masterGainDb, state.eqLowDb, state.eqMidDb, state.eqHighDb,
            state.limiterEnabled, state.limiterCeilingDb, state.velocityCurve, state.breathCc, state.expressionCc, state.damperCc,
            state.pitchBendEnabled, state.aftertouchEnabled)
    )

    private fun nativeResult(ok: Boolean): Boolean {
        if (ok) {
            statusChanged(null)
            return true
        }
        statusChanged(engine.nativeLastTextError().ifBlank { "Native sampler operation failed" })
        return false
    }

    private fun fail(message: String): Boolean {
        statusChanged(message)
        return false
    }

    override fun close() {
        if (!closed.compareAndSet(false, true)) return
        started.set(false)
        midi.close()
        runtime.close()
    }
}
