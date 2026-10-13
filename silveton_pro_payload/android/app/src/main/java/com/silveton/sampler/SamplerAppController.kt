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
        val TERCA_CELLS = arrayOf("MAJ ↑", "MAJ ↓", "MIN ↑", "MIN ↓", "PHR ↑", "HIJAZ ↑")
        val NOTE_NAMES = arrayOf("C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B")
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
        val droppedRealtimeEvents: Int,
        val cpuLoadPercent: Float = 0f   // SILVETON_CPU_METER_V1
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

    // SILVETON_PORTA_NEAR_V1: glide time for neighbouring keys (ms, whole sampler, 0..400).
    private var portaNearValue = portaPrefs.getInt("porta_near", 90).coerceIn(0, 400)
    fun portaNear(): Int = portaNearValue
    fun setPortaNear(value: Int, save: Boolean = true) {
        portaNearValue = value.coerceIn(0, 400)
        engine.nativeSetPortaNear(portaNearValue)
        if (save) portaPrefs.edit().putInt("porta_near", portaNearValue).apply()
    }

    // SILVETON_KEY_SCAN_V1: KEY RANGE of an Upper from a real key.
    private var keyScanUpper = -1
    private var lastKeySerial = -1
    fun keyScanUpper(): Int = keyScanUpper
    fun startKeyScan(upper: Int) { keyScanUpper = upper; engine.nativeStartScan(3) }
    fun cancelKeyScan() { keyScanUpper = -1; engine.nativeStartScan(0) }

    // ---------------------------------------------------------------- TERCA / Q (quarter tone)
    // SILVETON_TERCA_V1 / SILVETON_SCALE_V1: every Upper has a T key (plays the TERCA of
    // the SILVETON processor) and a Q key (follows the quarter-tone scale). SCAN sets the
    // tonality for T and the notes for Q. Kept between sessions.
    data class TercaState(val root: Int, val cell: Int)

    private var tercaUppers: Int = portaPrefs.getInt("terca_uppers", 0).toInt() and 0x7F
    private var scaleUppers: Int = portaPrefs.getInt("scale_uppers", 0).toInt() and 0x7F
    private var tercaRoot: Int = portaPrefs.getInt("terca_root", -1).toInt()
    private var tercaCell: Int = portaPrefs.getInt("terca_cell", -1).toInt()
    private var scaleMask: Int = portaPrefs.getInt("scale_mask", 0).toInt() and 0xFFF
    // SILVETON_TQ_LIST_V1: one T and one Q key on LIST switch TERCA / SCALE on and off
    // for the Uppers chosen with T / Q in COMBI (the choice itself is kept).
    private var tercaMaster: Boolean = portaPrefs.getBoolean("terca_master", true)
    private var scaleMaster: Boolean = portaPrefs.getBoolean("scale_master", true)
    private var lastTercaSerial = -1
    private var lastScaleSerial = -1

    fun tercaState() = TercaState(tercaRoot, tercaCell)
    /** COMBI T key lit: this Upper is chosen for TERCA and TERCA is on. */
    fun tercaOn(upper: Int): Boolean = tercaMaster && (tercaUppers shr upper) and 1 != 0
    fun quarterOn(upper: Int): Boolean = scaleMaster && (scaleUppers shr upper) and 1 != 0
    /** Chosen in COMBI but switched off on LIST (shown dim). */
    fun tercaChosen(upper: Int): Boolean = (tercaUppers shr upper) and 1 != 0
    fun quarterChosen(upper: Int): Boolean = (scaleUppers shr upper) and 1 != 0
    fun quarterNotes(): Int = scaleMask
    fun tercaMasterOn(): Boolean = tercaMaster && tercaUppers != 0
    fun scaleMasterOn(): Boolean = scaleMaster && scaleUppers != 0
    fun hasTercaUppers(): Boolean = tercaUppers != 0
    fun hasScaleUppers(): Boolean = scaleUppers != 0

    /** COMBI T key: choose / un-choose the Upper. A chosen Upper while TERCA is off on LIST = TERCA on again. */
    fun toggleTerca(upper: Int) {
        val chosen = (tercaUppers shr upper) and 1 != 0
        if (chosen && !tercaMaster) tercaMaster = true
        else { tercaUppers = tercaUppers xor (1 shl upper); if (!chosen) tercaMaster = true }
        saveTercaScale(); applyTerca()
    }
    fun toggleQuarter(upper: Int) {
        val chosen = (scaleUppers shr upper) and 1 != 0
        if (chosen && !scaleMaster) scaleMaster = true
        else { scaleUppers = scaleUppers xor (1 shl upper); if (!chosen) scaleMaster = true }
        saveTercaScale(); applyScaleUppers()
    }
    /** LIST T key. */
    fun toggleTercaMaster() { tercaMaster = !tercaMaster; saveTercaScale(); applyTerca() }
    /** LIST Q key. */
    fun toggleScaleMaster() { scaleMaster = !scaleMaster; saveTercaScale(); applyScaleUppers() }
    private fun applyScaleUppers() = engine.nativeSetScaleUppers(if (scaleMaster) scaleUppers else 0)

    /** 0 = no scan, 1 = TERCA scan running, 2 = Q scan running. Also picks up finished scans. */
    fun pollScan(): Int {
        val v = engine.nativeScanInfo()
        if (v.size < 6) return 0
        if (lastTercaSerial < 0) lastTercaSerial = v[3]
        if (lastScaleSerial < 0) lastScaleSerial = v[5]
        if (v[3] != lastTercaSerial) {
            lastTercaSerial = v[3]
            if (v[1] in 0..11 && v[2] in 0..5) { tercaRoot = v[1]; tercaCell = v[2]; saveTercaScale(); applyTerca() }
        }
        if (v[5] != lastScaleSerial) {
            lastScaleSerial = v[5]
            if (v[4] != 0) { scaleMask = v[4] and 0xFFF; saveTercaScale(); applyScale() }
        }
        if (v.size >= 8) {
            if (lastKeySerial < 0) lastKeySerial = v[7]
            if (v[7] != lastKeySerial) {
                lastKeySerial = v[7]
                val upper = keyScanUpper
                keyScanUpper = -1
                val key = v[6]
                val s = if (upper in 0..6) upperState(upper) else null
                if (s != null && key in 0..127) {
                    // the Upper plays up to the pressed key
                    val low = if (s.keyLow > key) 0 else s.keyLow
                    updateUpperMain(upper, s.copy(keyLow = low, keyHigh = key))
                }
            }
        }
        if (v[0] != 3 && keyScanUpper >= 0) keyScanUpper = -1
        return v[0]
    }

    fun startTercaScan() = engine.nativeStartScan(1)
    fun startScaleScan() = engine.nativeStartScan(2)
    fun cancelScan() = engine.nativeStartScan(0)

    private fun saveTercaScale() {
        portaPrefs.edit().putInt("terca_uppers", tercaUppers).putInt("scale_uppers", scaleUppers)
            .putInt("terca_root", tercaRoot).putInt("terca_cell", tercaCell).putInt("scale_mask", scaleMask)
            .putBoolean("terca_master", tercaMaster).putBoolean("scale_master", scaleMaster).apply()
    }

    private fun applyTerca() {
        val valid = tercaRoot in 0..11 && tercaCell in 0..5
        engine.nativeSetTerca(if (valid && tercaMaster) tercaUppers else 0, tercaRoot.coerceAtLeast(0), tercaCell.coerceAtLeast(0))
    }

    /** Q = quarter tone down (-50 cents) on the notes taken by SCAN. */
    fun scaleCents(): FloatArray = FloatArray(12) { pc -> if ((scaleMask and (1 shl pc)) != 0) -50f else 0f }

    private fun applyScale() {
        applyScaleUppers()
        val adv = globalAdvanced() ?: return
        // Offsets are given per real note (scaleRoot 0).
        updateGlobalAdvanced(adv.copy(scaleRoot = 0, scaleCents = scaleCents()))
    }

    // ---------------------------------------------------------------- full MIDI / advanced GLOBAL
    // SILVETON_MIDI_FULL_V1
    data class GlobalAdvanced(
        val scaleRoot: Int, val scaleCents: FloatArray, val midiInMask: Int,
        val filterNotes: Boolean, val filterCc: Boolean, val filterBend: Boolean, val filterAftertouch: Boolean,
        val breathMin: Float, val breathMax: Float, val breathCurve: Float,
        val expressionMin: Float, val expressionMax: Float, val expressionCurve: Float,
        val damperReversed: Boolean
    )

    fun globalAdvanced(): GlobalAdvanced? {
        val v = engine.nativeGlobalState()
        if (v.size < 39) return null
        return GlobalAdvanced(v[14].toInt(), FloatArray(12) { v[15 + it] }, v[27].toInt(),
            v[28] >= 0.5f, v[29] >= 0.5f, v[30] >= 0.5f, v[31] >= 0.5f,
            v[32], v[33], v[34], v[35], v[36], v[37], v[38] >= 0.5f)
    }

    fun updateGlobalAdvanced(a: GlobalAdvanced): Boolean = nativeResult(
        engine.nativeUpdateGlobalAdvanced(a.scaleRoot, a.scaleCents, a.midiInMask, a.filterNotes, a.filterCc,
            a.filterBend, a.filterAftertouch, a.breathMin, a.breathMax, a.breathCurve,
            a.expressionMin, a.expressionMax, a.expressionCurve, a.damperReversed)
    )

    /** Global transpose step (LIST quick keys). */
    fun stepGlobalTranspose(step: Int): Boolean {
        val g = globalUiState() ?: return false
        return updateGlobalUi(g.copy(transpose = if (step == 0) 0 else (g.transpose + step).coerceIn(-12, 12)))
    }

    /** Start low-latency output first, then accept MIDI. */
    fun start(): Boolean {
        if (closed.get()) return false
        engine.nativeSetPortaBalance(portaBalanceValue)
        engine.nativeSetPortaNear(portaNearValue)
        applyTerca()
        applyScaleUppers()
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

    /** SOUNDs of SILVETON_BANK.sv only (the built-in waves are not a bank). SILVETON_BANK_LOAD_FIX_V1 */
    private fun bankSoundCount(): Int {
        // SILVETON_BOW_V1: the bowed instruments (slots 9101..) are always in the catalog, so only slots 1..500 count
        val a = engine.nativeSoundCatalogAddresses()
        var n = 0
        var i = 2
        while (i < a.size) { if (a[i] in 1..500) n++; i += 3 }
        return n
    }

    private fun loadDefaultBankOnce() {
        if (defaultBankAttempted) return
        // The native sampler survives an Activity restart; keep its bank and BLENDS.
        // SILVETON_BANK_LOAD_FIX_V1: the built-in waves ("~ SAW" ...) are always in the catalog,
        // so only real bank SOUNDs count as "a bank is loaded".
        if (bankSoundCount() > 0) {
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
        bankPath = selected.absolutePath
        restoreUserStateIfMissing()
        val userState = userStateFile
        if (setPresent(userState) && !engine.nativeLoadUserPerformanceState(userState.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "BLENDS/LIST user state load failed" })
            return
        }
        if (setPresent(userState) && !setPresent(backupFile)) backupUserState()   // first copy for LIST/BLENDS made before this version
        val soundCount = bankSoundCount()
        if (soundCount <= 0) {
            statusChanged("SILVETON_BANK.sv loaded but contains no visible SOUND positions")
            return
        }
        defaultBankAttempted = true
        applyScale()
        applyTerca()
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
        return saveUserState()
    }

    // SILVETON_LIST_BACKUP_V1 / SILVETON_SETS_FIX_V1 → replaced by SILVETON_SET_FILE_V1:
    // the LIST and BLENDS are ONE file next to the bank -
    // <folder of SILVETON_BANK.sv>/SILVETON_BLENDS.SVSET (the previous version stays as
    // .BAK next to it). A silent safety copy goes to the app's own folder; at start a
    // missing file is restored from the safety copy or from the old locations.
    private fun bankDir(): File = bankFile()?.parentFile ?: appContext.filesDir
    private val userStateFile: File get() = File(bankDir(), "SILVETON_BLENDS.SVSET")
    private val privateStateFile: File get() = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
    private val oldBackupDir: File = File("/storage/emulated/0/SILVETON")
    private val backupFile: File get() = privateStateFile

    private fun saveUserState(): Boolean {
        val dst = userStateFile
        try {   // keep the previous version next to it as .BAK (one WRITE can always be undone)
            if (setPresent(dst)) copySet(dst, File(dst.parentFile, "SILVETON_BLENDS.SVSET.BAK"))
        } catch (_: Exception) {
        }
        val ok = nativeResult(engine.nativeSaveUserPerformanceState(dst.absolutePath))
        if (ok) backupUserState()
        return ok
    }

    // SILVETON_SET_DIR_FIX_V1: a SET (.SVSET) is a FOLDER (manifest.svs + audio/), not one file.
    // SAVE AS, LOAD, the backup and the restore all copy it as a folder.
    private fun setPresent(f: File): Boolean =
        (f.isDirectory && File(f, "manifest.svs").isFile) || (f.isFile && f.length() > 0L)

    private fun copySet(src: File, dst: File): Boolean = try {
        if (dst.exists()) dst.deleteRecursively()
        if (src.isDirectory) src.copyRecursively(dst, overwrite = true) else src.copyTo(dst, overwrite = true).isFile
    } catch (_: Exception) { false }

    private fun backupUserState() {
        try {
            val src = userStateFile
            if (!setPresent(src)) return
            copySet(src, privateStateFile)   // silent safety copy (survives a lost SD card / moved bank)
        } catch (_: Exception) {
        }
    }

    // SILVETON_BANK_INFO_V1: INFO in SETTINGS - what is in SILVETON_BANK.sv, and
    // DELETE of whole SOUNDs (with the samples only they use) from the bank file.
    data class BankSound(val slot: Int, val name: String, val samples: Int, val bytes: Long)
    data class BankInfo(val path: String, val sounds: Int, val samples: Int, val audioBytes: Long, val fileBytes: Long, val list: List<BankSound>)
    @Volatile private var bankPath: String? = null

    private fun bankFile(): File? {
        bankPath?.let { p -> val f = File(p); if (f.isFile) return f }
        val external: File? = appContext.getExternalFilesDir(null)?.let { d -> File(d, "SILVETON_BANK.sv") }
        val candidates: List<File> = listOfNotNull<File>(
            File("/storage/emulated/0/SILVETON_BANK.sv"), File("/storage/emulated/0/Download/SILVETON_BANK.sv"),
            external, File(appContext.filesDir, "SILVETON_BANK.sv"))
        return candidates.firstOrNull { f: File -> f.isFile && f.canRead() }
    }

    fun bankInfo(): BankInfo? {
        val f = bankFile() ?: return null
        val lines = engine.nativeBankInfo(f.absolutePath).split('\n').filter { it.isNotBlank() }
        val head = lines.firstOrNull()?.split('\t') ?: return null
        if (head.size < 5 || head[0] != "TOTAL") return null
        val list = lines.drop(1).mapNotNull { l ->
            val c = l.split('\t')
            if (c.size < 4) null else BankSound(c[0].toIntOrNull() ?: return@mapNotNull null, c[1], c[2].toIntOrNull() ?: 0, c[3].toLongOrNull() ?: 0L)
        }.sortedBy { it.slot }
        return BankInfo(f.absolutePath, head[1].toIntOrNull() ?: 0, head[2].toIntOrNull() ?: 0, head[3].toLongOrNull() ?: 0L, head[4].toLongOrNull() ?: 0L, list)
    }

    /** Deletes the SOUNDs from the bank file and loads the bank again. Null = done, else the reason. */
    fun deleteBankSounds(slots: List<Int>): String? {
        val f = bankFile() ?: return "SILVETON_BANK.sv не е намерен"
        val err = engine.nativeRemoveBankSounds(f.absolutePath, slots.toIntArray())
        if (err.isNotEmpty()) return err
        if (!engine.nativeLoadSvBank(f.absolutePath)) return engine.nativeLastTextError().ifBlank { "Банката не се зареди отново" }
        applyScale()
        applyTerca()
        saveUserState()
        return null
    }

    // SILVETON_SET_SAVE_AS_V1: SAVE AS / LOAD of the whole set: every LIST position with
    // its BLEND, all COMBI settings, GLOBAL settings and the T / Q choice.
    // SILVETON_SET_FILE_V1: one file per set, next to the bank:
    // <folder of SILVETON_BANK.sv>/SILVETON_SETS/<name>.SVSET (+ <name>.TQ).
    private val setsDir: File get() = File(bankDir(), "SILVETON_SETS")
    private fun setsDirs(): List<File> = listOf(setsDir)
    fun setsFolders(): String = setsDir.absolutePath

    // SILVETON_LIST_V2: favourite LIST positions

    // ---------------------------------------------------------------- SILVETON_PARAMS_V1
    // Generic engine settings (ids: SamplerAudioBridge.cpp). Values come back as float arrays.
    fun setUpperParam(upper: Int, id: Int, value: Float): Boolean = nativeResult(engine.nativeSetUpperParam(upper, id, value))
    fun upperParams(upper: Int): FloatArray = engine.nativeUpperParams(upper)
    fun setLayerParam(upper: Int, layer: Int, id: Int, value: Float): Boolean = nativeResult(engine.nativeSetLayerParam(upper, layer, id, value))
    fun setLayerSound(upper: Int, layer: Int, slot: Int): Boolean = nativeResult(engine.nativeSetLayerSound(upper, layer, slot))
    fun layerParams(upper: Int, layer: Int): FloatArray = engine.nativeLayerParams(upper, layer)
    fun layerName(upper: Int, layer: Int): String = engine.nativeLayerName(upper, layer)
    fun setFxParam(unit: Int, id: Int, value: Float): Boolean = nativeResult(engine.nativeSetFxParam(unit, id, value))
    fun fxParams(unit: Int): FloatArray = engine.nativeFxParams(unit)
    // Sound picker memory: favourite SOUND slots and the last picked ones (SILVETON_PICKER_V1)
    fun soundFavourites(): Set<Int> = portaPrefs.getString("sound_favs", "")!!.split(',').mapNotNull { it.toIntOrNull() }.toSet()
    fun saveSoundFavourites(favs: Set<Int>) { portaPrefs.edit().putString("sound_favs", favs.sorted().joinToString(",")).apply() }
    fun recentSounds(): List<Int> = portaPrefs.getString("sound_recent", "")!!.split(',').mapNotNull { it.toIntOrNull() }
    fun rememberRecentSound(slot: Int) {
        val list = (listOf(slot) + recentSounds().filter { it != slot }).take(40)
        portaPrefs.edit().putString("sound_recent", list.joinToString(",")).apply()
    }
    fun analyzerFrame(upper: Int, afterEq: Boolean): FloatArray = engine.nativeAnalyzerFrame(upper, afterEq)
    fun setSoundSynthParam(upper: Int, sound: Int, id: Int, value: Float): Boolean = nativeResult(engine.nativeSetSoundSynthParam(upper, sound, id, value))
    fun soundSynthParams(upper: Int, sound: Int): FloatArray = engine.nativeSoundSynthParams(upper, sound)

    // SILVETON_THEME_V1: the look of the screen (false = СВЕТЛА, true = ТЪМНА), kept between starts
    fun themeDark(): Boolean = portaPrefs.getBoolean("theme_dark", false)
    fun setThemeDark(dark: Boolean) { portaPrefs.edit().putBoolean("theme_dark", dark).apply() }

    fun listFavourites(): Set<Int> = portaPrefs.getString("list_favs", "")!!.split(',').mapNotNull { it.toIntOrNull() }.toSet()
    fun saveListFavourites(favs: Set<Int>) { portaPrefs.edit().putString("list_favs", favs.sorted().joinToString(",")).apply() }

    private fun setFileName(name: String): String =
        name.trim().replace(Regex("[^A-Za-z0-9 _\\-\\u0400-\\u04FF]"), "_").take(40).trim().ifBlank { "SET" }

    fun savedSets(): List<String> = try {
        val all = ArrayList<File>()
        for (dir in setsDirs()) {
            val files: Array<File> = dir.listFiles { f: File -> f.name.endsWith(".SVSET") && setPresent(f) } ?: emptyArray()   // SILVETON_SET_DIR_FIX_V1
            all.addAll(files)
        }
        all.sortedByDescending { f: File -> f.lastModified() }.map { f: File -> f.name.removeSuffix(".SVSET") }.distinct()
    } catch (_: Exception) { emptyList() }

    private fun setFile(name: String): File? {
        val base = setFileName(name) + ".SVSET"
        return setsDirs().map { d: File -> File(d, base) }.firstOrNull { f: File -> setPresent(f) }
    }

    fun setExists(name: String): Boolean = setFile(name) != null

    /** Returns null when saved, else a short reason. */
    fun saveSetAs(name: String): String? {
        if (!saveUserState()) return engine.nativeLastTextError().ifBlank { "Записът не успя" }
        val base = setFileName(name)
        val tq = "terca_uppers=$tercaUppers\nscale_uppers=$scaleUppers\nterca_root=$tercaRoot\nterca_cell=$tercaCell\n" +
            "scale_mask=$scaleMask\nterca_master=$tercaMaster\nscale_master=$scaleMaster\n"
        var saved = 0
        var lastError: String? = null
        for (dir in setsDirs()) {
            try {
                if (!dir.isDirectory && !dir.mkdirs()) { lastError = "Няма достъп до ${dir.absolutePath}"; continue }
                if (!copySet(userStateFile, File(dir, "$base.SVSET"))) { lastError = "Няма достъп до ${dir.absolutePath}"; continue }   // SILVETON_SET_DIR_FIX_V1
                File(dir, "$base.TQ").writeText(tq)
                saved++
            } catch (e: Exception) { lastError = e.message ?: "Записът не успя" }
        }
        return if (saved > 0) null else (lastError ?: "Записът не успя")
    }

    /** Returns null when loaded, else a short reason. The current LIST / BLENDS are replaced. */
    fun loadSavedSet(name: String): String? {
        val file = setFile(name) ?: return "Файлът го няма"
        if (!nativeResult(engine.nativeReplaceUserPerformanceState(file.absolutePath)))
            return engine.nativeLastTextError().ifBlank { "Зареждането не успя" }
        try {
            val tq = File(file.parentFile, setFileName(name) + ".TQ")
            if (tq.isFile) {
                val m = tq.readLines().mapNotNull { l -> l.split('=', limit = 2).takeIf { it.size == 2 }?.let { it[0] to it[1] } }.toMap()
                m["terca_uppers"]?.toIntOrNull()?.let { tercaUppers = it and 0x7F }
                m["scale_uppers"]?.toIntOrNull()?.let { scaleUppers = it and 0x7F }
                m["terca_root"]?.toIntOrNull()?.let { tercaRoot = it }
                m["terca_cell"]?.toIntOrNull()?.let { tercaCell = it }
                m["scale_mask"]?.toIntOrNull()?.let { scaleMask = it and 0xFFF }
                m["terca_master"]?.let { tercaMaster = it == "true" }
                m["scale_master"]?.let { scaleMaster = it == "true" }
                saveTercaScale()
            }
        } catch (_: Exception) {
        }
        applyScale()
        applyTerca()
        saveUserState()
        return null
    }

    // A fresh install (no LIST/BLENDS of its own) takes the copy back automatically.
    private fun restoreUserStateIfMissing() {
        try {
            val dst = userStateFile
            if (setPresent(dst)) return
            // the safety copy, then the places older versions used
            val src = listOf(
                privateStateFile, File(dst.parentFile, "SILVETON_BLENDS.SVSET.BAK"),
                File(oldBackupDir, "SILVETON_USER_STATE.SVSET"), File(oldBackupDir, "SILVETON_USER_STATE.SVSET.BAK"))
                .firstOrNull { setPresent(it) } ?: return
            copySet(src, dst)   // SILVETON_SET_FILE_V1
        } catch (_: Exception) {
        }
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
        return saveUserState()
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
        return saveUserState()
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
            droppedRealtimeEvents = meters.getOrElse(4) { 0f }.toInt(),
            cpuLoadPercent = meters.getOrElse(5 + 7 * 2) { 0f }
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
        return CombiFxState(fx(0), fx(7), v[28].toInt())   // SILVETON_FX_V3: 4 slots x 7 values, then the routing
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
