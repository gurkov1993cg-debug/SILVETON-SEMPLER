#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
bridge = root / 'android/app/src/main/cpp/SamplerAudioBridge.cpp'
engine_kt = root / 'android/app/src/main/java/com/silveton/sampler/NativeSamplerEngine.kt'
controller = root / 'android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt'
for p in (bridge, engine_kt, controller):
    if not p.is_file():
        raise SystemExit(f'missing BLENDS persistence source: {p}')

# ---------------------------------------------------------------------------
# Native lightweight user-performance state.
# It stores COMBI/LIST + fixed soundSlotId references only. Program pointers are
# cleared in the snapshot before SetPackage::save(), therefore the package graph
# contains no PROGRAM/MULTISAMPLE/SAMPLE audio and stays tiny.
# ---------------------------------------------------------------------------
s = bridge.read_text(encoding='utf-8')
if 'bool saveUserPerformanceState(const std::string& path)' not in s:
    anchor = '''    bool saveSet(const std::string& path) {
        syncCurrentStateToLibrary();
        const auto saved = SetPackage::save(path, library_.snapshot());
        if (!saved.success) { setTextError(saved.error); return false; }
        setTextError({});
        return true;
    }
'''
    if anchor not in s:
        raise SystemExit('saveSet anchor changed')
    methods = anchor + '''
    bool saveUserPerformanceState(const std::string& path) {
        syncCurrentStateToLibrary();
        auto data = library_.snapshot();
        data.samples.clear();
        data.multisamples.clear();
        data.programs.clear();
        for (auto& slot : data.combis) {
            for (auto& upper : slot.combi.uppers) upper.program.reset();
        }
        const auto saved = SetPackage::save(path, data);
        if (!saved.success) { setTextError(saved.error); return false; }
        setTextError({});
        return true;
    }

    bool loadUserPerformanceState(const std::string& path) {
        const auto loaded = SetPackage::load(path);
        if (!loaded.ok()) { setTextError(loaded.error); return false; }

        for (const auto& saved : loaded.data.combis) {
            Combi rebound = saved.combi;
            for (auto& upper : rebound.uppers) {
                if (upper.soundSlotId == kNoSoundSlot) {
                    upper.program.reset();
                    continue;
                }
                if (upper.soundSlotId > kFixedSoundSlotCount) {
                    upper.program.reset();
                    upper.enabled = false;
                    continue;
                }
                upper.program = library_.program(SlotAddress{0U, 0U, upper.soundSlotId});
                if (upper.program != nullptr) upper.name = upper.program->name;
                else upper.enabled = false;
            }
            if (!library_.writeCombi(saved.address, rebound)) {
                setTextError("Could not restore saved BLEND/COMBI");
                return false;
            }
        }
        for (const auto& saved : loaded.data.lists) {
            if (!library_.writeList(saved.address, saved.list)) {
                setTextError("Could not restore saved LIST");
                return false;
            }
        }
        setTextError({});
        return true;
    }
'''
    s = s.replace(anchor, methods, 1)

# JNI bridge. Insert next to the existing SET load function so the generated
# Kotlin declarations have a direct native implementation.
if 'nativeSaveUserPerformanceState' not in s:
    anchor = 'extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadSet(JNIEnv* env,jobject,jstring path){return sampler().loadSet(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}\n'
    if anchor not in s:
        raise SystemExit('nativeLoadSet JNI anchor changed')
    jni = anchor + '''extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeSaveUserPerformanceState(JNIEnv* env,jobject,jstring path){return sampler().saveUserPerformanceState(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jboolean JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeLoadUserPerformanceState(JNIEnv* env,jobject,jstring path){return sampler().loadUserPerformanceState(jstringToUtf8(env,path))?JNI_TRUE:JNI_FALSE;}
'''
    s = s.replace(anchor, jni, 1)

for token in [
    'bool saveUserPerformanceState(const std::string& path)',
    'data.samples.clear();',
    'data.multisamples.clear();',
    'data.programs.clear();',
    'upper.program.reset();',
    'bool loadUserPerformanceState(const std::string& path)',
    'nativeSaveUserPerformanceState',
    'nativeLoadUserPerformanceState',
]:
    if token not in s:
        raise SystemExit(f'native BLENDS persistence contract missing: {token}')
bridge.write_text(s, encoding='utf-8')

# ---------------------------------------------------------------------------
# Kotlin native declarations.
# ---------------------------------------------------------------------------
k = engine_kt.read_text(encoding='utf-8')
if 'external fun nativeSaveUserPerformanceState(' not in k:
    anchor = '    external fun nativeLoadSet(path: String): Boolean\n'
    if anchor not in k:
        raise SystemExit('NativeSamplerEngine nativeLoadSet declaration anchor changed')
    decl = anchor + '''    external fun nativeSaveUserPerformanceState(path: String): Boolean
    external fun nativeLoadUserPerformanceState(path: String): Boolean
'''
    k = k.replace(anchor, decl, 1)
engine_kt.write_text(k, encoding='utf-8')

# ---------------------------------------------------------------------------
# Controller: load saved performance objects after SOUND bank; save after every
# visible WRITE COMBI / ADD TO LIST operation.
# ---------------------------------------------------------------------------
c = controller.read_text(encoding='utf-8')

# loadDefaultBankOnce is already normalized by the bank-storage patch before this
# script runs. Load the tiny user-state package only after nativeLoadSvBank, so
# soundSlotId references can be rebound to the fresh bank programs.
load0 = c.find('    private fun loadDefaultBankOnce() {')
load1 = c.find('\n    fun pauseAudio()', load0)
if load0 < 0 or load1 < 0:
    raise SystemExit('loadDefaultBankOnce boundaries changed')
block = c[load0:load1]
if 'nativeLoadUserPerformanceState' not in block:
    anchor = '''        if (!engine.nativeLoadSvBank(selected.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "SILVETON_BANK.sv load failed: ${selected.absolutePath}" })
            return
        }
'''
    if anchor not in block:
        raise SystemExit('bank-load success anchor changed')
    addition = anchor + '''        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        if (userState.exists() && !engine.nativeLoadUserPerformanceState(userState.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "BLENDS/LIST user state load failed" })
            return
        }
'''
    block = block.replace(anchor, addition, 1)
    c = c[:load0] + block + c[load1:]

# WRITE COMBI: persist after the slot is successfully activated.
old_write = '''    fun writeCombiSlot(slot: Int, name: String): Boolean {
        if (slot !in 1..500) return fail("COMBI position must be 1..500")
        val finalName = name.trim().ifBlank { combiName() }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) return false
        return nativeResult(engine.nativeActivateCombi(0, 0, slot))
    }
'''
new_write = '''    fun writeCombiSlot(slot: Int, name: String): Boolean {
        if (slot !in 1..500) return fail("COMBI position must be 1..500")
        val finalName = name.trim().ifBlank { combiName() }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) return false
        if (!nativeResult(engine.nativeActivateCombi(0, 0, slot))) return false
        val userState = File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")
        return nativeResult(engine.nativeSaveUserPerformanceState(userState.absolutePath))
    }
'''
if old_write in c:
    c = c.replace(old_write, new_write, 1)
elif 'nativeSaveUserPerformanceState' not in c[c.find('    fun writeCombiSlot('):c.find('    fun firstFreeCombiSlot()', c.find('    fun writeCombiSlot('))]:
    raise SystemExit('writeCombiSlot persistence anchor changed')

# ADD TO LIST: save the list after the native mutation succeeds.
old_list = '''    fun addCombiToList(combiSlot: Int, name: String): Boolean {
        if (combiSlot !in 1..500) return fail("COMBI position must be 1..500")
        val current = listItems(0, 0, 0)
        if (current.size >= 200) return fail("LIST is full")
        val finalName = name.trim().ifBlank { combiName() }
        val nextNumber = (current.maxOfOrNull { it.number } ?: 0) + 1
        return if (current.isEmpty()) {
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
    }
'''
new_list = '''    fun addCombiToList(combiSlot: Int, name: String): Boolean {
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
'''
if old_list in c:
    c = c.replace(old_list, new_list, 1)
elif c.count('nativeSaveUserPerformanceState') < 2:
    raise SystemExit('addCombiToList persistence anchor changed')

for token in [
    'File(appContext.filesDir, "SILVETON_USER_STATE.SVSET")',
    'engine.nativeLoadUserPerformanceState(userState.absolutePath)',
    'engine.nativeSaveUserPerformanceState(userState.absolutePath)',
]:
    if token not in c:
        raise SystemExit(f'controller BLENDS persistence contract missing: {token}')
controller.write_text(c, encoding='utf-8')

print('PASS: BLENDS/COMBI + LIST persist across app restarts without copying bank sample audio')
