package com.silveton.sampler

class NativeSamplerEngine {
    init {
        System.loadLibrary("silveton_jni")
        nativeSamplerInit()
    }

    external fun nativeSamplerInit()
    external fun nativeSetUpperToneParam(upper: Int, param: Int, value: Float): Boolean
    external fun nativeAudioOpen(): Boolean
    external fun nativeAudioStart(): Boolean
    external fun nativeAudioStop()
    external fun nativeAudioClose()
    external fun nativeAudioRecover(): Boolean
    external fun nativeAudioNeedsRecovery(): Boolean
    /** Keeps SILVETON on the headphone jack only (no-op while audio is stopped). */
    external fun nativeEnforceOutputRoute(): Boolean
    /** PORTA BALANCE of the SILVETON keyboard processor (3..40, default 11). */
    external fun nativeSetPortaBalance(balance: Int)
    /** SILVETON_SCAN_V1: 1 = TERCA scan, 2 = SCALE scan, 0 = cancel. */
    external fun nativeStartScan(mode: Int)
    /** [scanMode, tercaRoot, tercaCell, tercaSerial, scaleMask, scaleSerial]. */
    external fun nativeScanInfo(): IntArray
    /** SILVETON_TERCA_V1: TERCA on the Uppers in [upperMask] (bit 0 = Upper 1, 0 = off). */
    external fun nativeSetTerca(upperMask: Int, root: Int, cell: Int)
    /** SILVETON_SCALE_V1: Uppers whose Q key is on follow the quarter-tone scale. */
    external fun nativeSetScaleUppers(upperMask: Int)
    external fun nativeLoadAudio(path: String): Boolean
    external fun nativeLoadKmp(path: String): Boolean
    external fun nativeImportKmpToLibrary(path: String): IntArray
    external fun nativeImportKscToLibrary(path: String): IntArray
    external fun nativeMultisampleIds(): IntArray
    external fun nativeMultisampleNames(): Array<String>
    external fun nativeKscNames(path: String): Array<String>
    external fun nativeLoadKscItem(path: String, index: Int): Boolean
    external fun nativeLoadSvBank(path: String): Boolean
    external fun nativeLoadSet(path: String): Boolean
    external fun nativeSaveUserPerformanceState(path: String): Boolean
    external fun nativeLoadUserPerformanceState(path: String): Boolean
    external fun nativeReplaceUserPerformanceState(path: String): Boolean
    external fun nativeSaveSet(path: String): Boolean
    external fun nativeSaveBank(path: String, bank: Int): Boolean
    external fun nativeLoadBank(path: String, targetBank: Int, overwrite: Boolean): Boolean
    external fun nativeSetSetName(name: String): Boolean
    external fun nativeWriteProgram(bank: Int, page: Int, slot: Int, upper: Int, name: String): Boolean
    external fun nativeUpdateProgramBasic(bank: Int, page: Int, slot: Int, mono: Boolean, legato: Boolean, portamentoSec: Float): Boolean
    external fun nativeAddProgramLayer(bank: Int, page: Int, slot: Int, multisampleId: Int, name: String): Boolean
    external fun nativeRemoveProgramLayer(bank: Int, page: Int, slot: Int, layer: Int): Boolean
    external fun nativeMoveProgramLayer(bank: Int, page: Int, slot: Int, fromLayer: Int, toLayer: Int): Boolean
    external fun nativeAssignProgramMultisample(bank: Int, page: Int, slot: Int, layer: Int, multisampleId: Int): Boolean
    external fun nativeUpdateProgramLayerBasic(
        bank: Int, page: Int, slot: Int, layer: Int, enabled: Boolean,
        keyLow: Int, keyHigh: Int, velocityLow: Int, velocityHigh: Int,
        velocityFadeIn: Int, velocityFadeOut: Int, transposeSemitones: Int,
        tuneCents: Float, levelDb: Float, pan: Float, delaySec: Float, damperEnabled: Boolean
    ): Boolean
    external fun nativeUpdateProgramAmpEnvelope(
        bank: Int, page: Int, slot: Int, layer: Int, attack: Float, decay: Float, sustain: Float, release: Float
    ): Boolean
    external fun nativeUpdateProgramPitch(
        bank: Int, page: Int, slot: Int, layer: Int, amountCents: Float,
        attack: Float, decay: Float, sustain: Float, release: Float
    ): Boolean
    external fun nativeUpdateProgramFilter(
        bank: Int, page: Int, slot: Int, layer: Int, mode: Int, cutoffHz: Float, resonance: Float,
        keyTrack: Float, velocityAmount: Float, envelopeAmount: Float, lfoAmount: Float,
        attack: Float, decay: Float, sustain: Float, release: Float
    ): Boolean
    external fun nativeUpdateProgramLfo(
        bank: Int, page: Int, slot: Int, layer: Int, lfoIndex: Int, wave: Int, frequencyHz: Float,
        delaySec: Float, fadeSec: Float, keySync: Boolean, pitchCents: Float, filterOctaves: Float, ampDepth: Float
    ): Boolean
    external fun nativeWriteSample(id: Int, upper: Int, layer: Int, zone: Int, name: String): Boolean
    external fun nativeUpdateSampleMetadata(id: Int, name: String, rootKey: Int, fineTuneCents: Float): Boolean
    external fun nativeTrimSample(id: Int, startFrame: Long, endFrame: Long): Boolean
    external fun nativeNormalizeSample(id: Int, targetPeakDb: Float): Boolean
    external fun nativeReverseSample(id: Int): Boolean
    external fun nativeFadeSample(id: Int, fadeInFrames: Long, fadeOutFrames: Long): Boolean
    external fun nativeSetSampleLoop(id: Int, startFrame: Long, endFrame: Long, crossfadeFrames: Int): Boolean
    external fun nativeClearSampleLoop(id: Int): Boolean
    external fun nativeSampleNearestZeroCrossing(id: Int, aroundFrame: Long, radius: Long): Long
    external fun nativeWriteMultisample(id: Int, upper: Int, layer: Int, name: String): Boolean
    external fun nativeRenameMultisample(id: Int, name: String): Boolean
    external fun nativeAddMultisampleZone(
        id: Int, sampleId: Int, insertIndex: Int, keyLow: Int, keyHigh: Int, velocityLow: Int, velocityHigh: Int,
        rootKey: Int, transposeSemitones: Int, tuneCents: Float, gainDb: Float, pan: Float,
        oneShot: Boolean, reverse: Boolean, nonTranspose: Boolean
    ): Boolean
    external fun nativeUpdateMultisampleZone(
        id: Int, zoneIndex: Int, sampleId: Int, keyLow: Int, keyHigh: Int, velocityLow: Int, velocityHigh: Int,
        rootKey: Int, transposeSemitones: Int, tuneCents: Float, gainDb: Float, pan: Float,
        oneShot: Boolean, reverse: Boolean, nonTranspose: Boolean
    ): Boolean
    external fun nativeRemoveMultisampleZone(id: Int, zoneIndex: Int): Boolean
    external fun nativeMoveMultisampleZone(id: Int, fromIndex: Int, toIndex: Int): Boolean
    external fun nativeWriteCombi(bank: Int, page: Int, slot: Int, name: String): Boolean
    external fun nativeUpdateUpperMain(upper: Int, enabled: Boolean, solo: Boolean, mute: Boolean, levelDb: Float, pan: Float, keyLow: Int, keyHigh: Int, voiceMode: Int): Boolean
    external fun nativeUpdateUpperTune(upper: Int, octave: Int, transpose: Int, fineCents: Float, pitchBendRange: Int, portamentoSec: Float): Boolean
    external fun nativeUpdateUpperRange(upper: Int, velocityLow: Int, velocityHigh: Int): Boolean
    external fun nativeUpdateUpperControl(upper: Int, breath: Boolean, expression: Boolean, aftertouch: Boolean, damper: Boolean, midiChannel: Int): Boolean
    external fun nativeUpdateUpperTone(upper: Int, attackScale: Float, decayScale: Float, sustainScale: Float, releaseScale: Float, cutoffOctaves: Float, resonanceOffset: Float, lfoRateScale: Float, lfoDepthScale: Float, eqLowDb: Float, eqMidDb: Float, eqHighDb: Float): Boolean
    external fun nativeUpperState(upper: Int): FloatArray
    external fun nativeUpperSoundName(upper: Int): String
    external fun nativeCombiName(): String
    external fun nativeCombiCatalogSlots(): IntArray
    external fun nativeCombiCatalogNames(): Array<String>
    external fun nativeSoundCatalogAddresses(): IntArray
    external fun nativeSoundCatalogNames(): Array<String>
    external fun nativeAssignUpperSound(bank: Int, page: Int, slot: Int, upper: Int): Boolean
    external fun nativeClearUpperSound(upper: Int): Boolean
    external fun nativeUpdateUpperFxSends(upper: Int, send1: Float, send2: Float): Boolean
    external fun nativeUpdateUpperControllerRoute(
        upper: Int, route: Int, source: Int, destination: Int, cc: Int, amount: Float, bipolar: Boolean
    ): Boolean
    external fun nativeUpdateCombiFx(
        slot: Int, type: Int, wet: Float, timeMs: Float, feedback: Float, rateHz: Float, depth: Float, tone: Float
    ): Boolean
    external fun nativeUpdateCombiFxRouting(routing: Int): Boolean
    external fun nativeCombiFxState(): FloatArray
    external fun nativeGlobalState(): FloatArray
    external fun nativeWriteGlobal(): Boolean
    external fun nativeUpdateGlobal(
        masterTuneHz: Float, transposeSemitones: Int, masterGainDb: Float,
        eqLowDb: Float, eqMidDb: Float, eqHighDb: Float, limiterEnabled: Boolean,
        limiterCeilingDb: Float, velocityCurve: Int, breathCc: Int, expressionCc: Int,
        damperCc: Int, pitchBendEnabled: Boolean, aftertouchEnabled: Boolean
    ): Boolean
    external fun nativeUpdateGlobalAdvanced(
        scaleRoot: Int,
        scaleOffsetsCents: FloatArray,
        midiInputChannelMask: Int,
        filterNotes: Boolean,
        filterControlChange: Boolean,
        filterPitchBend: Boolean,
        filterAftertouch: Boolean,
        breathMin: Float,
        breathMax: Float,
        breathCurve: Float,
        expressionMin: Float,
        expressionMax: Float,
        expressionCurve: Float,
        damperPolarityReversed: Boolean
    ): Boolean
    external fun nativeWriteList(
        bank: Int, page: Int, slot: Int, listName: String,
        numbers: IntArray, names: Array<String>, combiAddresses: IntArray
    ): Boolean
    external fun nativeSetListEntry(listBank: Int, listPage: Int, listSlot: Int, index: Int, number: Int, name: String, combiBank: Int, combiPage: Int, combiSlot: Int): Boolean
    external fun nativeInsertListEntry(listBank: Int, listPage: Int, listSlot: Int, index: Int, number: Int, name: String, combiBank: Int, combiPage: Int, combiSlot: Int): Boolean
    external fun nativeRemoveListEntry(listBank: Int, listPage: Int, listSlot: Int, index: Int): Boolean
    external fun nativeMoveListEntry(listBank: Int, listPage: Int, listSlot: Int, fromIndex: Int, toIndex: Int): Boolean
    external fun nativeMoveList(fromBank: Int, fromPage: Int, fromSlot: Int, toBank: Int, toPage: Int, toSlot: Int, overwrite: Boolean): Boolean
    external fun nativeActivateListEntry(listBank: Int, listPage: Int, listSlot: Int, index: Int): Boolean
    external fun nativeActiveListEntry(listBank: Int, listPage: Int, listSlot: Int): Int
    external fun nativeSetActiveListEntry(listBank: Int, listPage: Int, listSlot: Int, index: Int): Boolean
    external fun nativeListEntryNumbers(listBank: Int, listPage: Int, listSlot: Int): IntArray
    external fun nativeListEntryNames(listBank: Int, listPage: Int, listSlot: Int): Array<String>
    external fun nativeListEntryCombiAddresses(listBank: Int, listPage: Int, listSlot: Int): IntArray
    external fun nativeActivateProgram(bank: Int, page: Int, slot: Int, upper: Int): Boolean
    external fun nativeActivateCombi(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeClearProgram(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeClearCombi(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeClearList(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeClearMultisample(id: Int): Boolean
    external fun nativeProgramSlotOccupied(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeCombiSlotOccupied(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeListSlotOccupied(bank: Int, page: Int, slot: Int): Boolean
    external fun nativeRenameProgram(bank: Int, page: Int, slot: Int, name: String): Boolean
    external fun nativeRenameCombi(bank: Int, page: Int, slot: Int, name: String): Boolean
    external fun nativeRenameList(bank: Int, page: Int, slot: Int, name: String): Boolean
    external fun nativeCopyProgram(fromBank: Int, fromPage: Int, fromSlot: Int, toBank: Int, toPage: Int, toSlot: Int, overwrite: Boolean): Boolean
    external fun nativeCopyCombi(fromBank: Int, fromPage: Int, fromSlot: Int, toBank: Int, toPage: Int, toSlot: Int, overwrite: Boolean): Boolean
    external fun nativeCopyList(fromBank: Int, fromPage: Int, fromSlot: Int, toBank: Int, toPage: Int, toSlot: Int, overwrite: Boolean): Boolean
    external fun nativeMidi(status: Int, data1: Int, data2: Int): Boolean
    external fun nativeMidiDeviceCount(): Int
    external fun nativeMidiLastError(): Int
    external fun nativeMidiActivitySerial(): Long
    external fun nativeMidiLastMessage(): IntArray
    external fun nativeAudioStarted(): Boolean
    external fun nativeTestInstrumentEnabled(): Boolean
    external fun nativeTestNoteOn(note: Int, velocity: Int): Boolean
    external fun nativeTestNoteOff(note: Int)
    external fun nativePanic(): Boolean
    /** peakL, peakR, limiter GR dB, active voices, dropped RT events, then 7 x (upperL, upperR) */
    external fun nativeMeters(): FloatArray
    /** sampleRate, framesPerBurst, bufferSizeFrames, xrunCount */
    external fun nativeAudioInfo(): IntArray
    external fun nativeLatencyMs(): Double
    external fun nativeLastAudioError(): Int
    external fun nativeLastTextError(): String
}
