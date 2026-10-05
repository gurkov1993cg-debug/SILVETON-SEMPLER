#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
manifest_path = root / "android/app/src/main/AndroidManifest.xml"
root_runtime_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonRootRuntime.kt"
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"

for p in (manifest_path, root_runtime_path, controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing bank access source: {p}")

# Android 11+ scoped storage blocks direct File/mmap access to arbitrary shared
# files unless all-files access is declared and granted. This dedicated rooted
# sampler grants the app-op from its already-approved root bootstrap.
manifest = manifest_path.read_text(encoding="utf-8")
perm = '    <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />\n'
if "android.permission.MANAGE_EXTERNAL_STORAGE" not in manifest:
    anchor = '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n'
    if anchor not in manifest:
        raise SystemExit("AndroidManifest root anchor not found")
    manifest = manifest.replace(anchor, anchor + perm, 1)
manifest_path.write_text(manifest, encoding="utf-8")

runtime = root_runtime_path.read_text(encoding="utf-8")

# IMPORTANT: do not trust the in-process 'active' flag as proof that Android has
# kept our cpuset/governor/ALSA/SELinux state. The OS may recreate nodes or
# restore scheduling constraints while this process stays alive. enter() is
# deliberately idempotent: saved original values are protected by STATE/active,
# while the live performance envelope must be reasserted on every start/resume/
# recovery call.
early_return = '        if (active) return@synchronized Result(true)\n\n'
if early_return in runtime:
    runtime = runtime.replace(
        early_return,
        '        // Reassert live sampler grants/tuning even when already active.\n\n',
        1,
    )
if 'if (active) return@synchronized Result(true)' in runtime:
    raise SystemExit("stale root active short-circuit still present")

if "MANAGE_EXTERNAL_STORAGE allow" not in runtime:
    anchor = '''            STATE=/data/local/tmp/silveton-root-state
            mkdir -p "${'$'}STATE"
'''
    addition = '''            STATE=/data/local/tmp/silveton-root-state
            # Dedicated rooted appliance: grant this package direct shared-storage access
            # so SILVETON_BANK.sv can be mmap'ed in place without copying multi-GB banks.
            cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true
            mkdir -p "${'$'}STATE"
'''
    if anchor not in runtime:
        raise SystemExit("SilvetonRootRuntime enter() STATE anchor not found")
    runtime = runtime.replace(anchor, addition, 1)

if "fun refreshBankStorageAccess(): Result" not in runtime:
    insert_at = runtime.find("    fun refreshAudioPermissions(): Result")
    if insert_at < 0:
        raise SystemExit("refreshAudioPermissions insertion anchor not found")
    method = '''    fun refreshBankStorageAccess(): Result {
        val script = """
            set -u
            cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || {
                echo "Could not grant all-files access"
                exit 61
            }
            exit 0
        """.trimIndent()
        return runSu(script, 5)
    }

'''
    runtime = runtime[:insert_at] + method + runtime[insert_at:]

for token in [
    "MANAGE_EXTERNAL_STORAGE allow",
    "fun refreshBankStorageAccess(): Result",
    "cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow",
    "Reassert live sampler grants/tuning even when already active.",
]:
    if token not in runtime:
        raise SystemExit(f"missing root bank/performance token: {token}")
root_runtime_path.write_text(runtime, encoding="utf-8")

controller = controller_path.read_text(encoding="utf-8")
old_start = controller.find("    private fun loadDefaultBankOnce() {")
if old_start < 0:
    raise SystemExit("loadDefaultBankOnce not found")
next_method = controller.find("\n    fun pauseAudio()", old_start)
if next_method < 0:
    raise SystemExit("loadDefaultBankOnce end anchor not found")

new_method = '''    private fun loadDefaultBankOnce() {
        if (defaultBankAttempted) return
        defaultBankAttempted = true

        // Android 15 scoped storage can make File.isFile return false even when
        // /storage/emulated/0/SILVETON_BANK.sv exists. Root grants the package
        // all-files access; the bank then stays in place and native code mmap's it.
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
            val rootDetail = if (access.ok) "all-files access granted" else access.message
            statusChanged("SILVETON_BANK.sv is not readable ($rootDetail). Expected /storage/emulated/0/SILVETON_BANK.sv")
            return
        }

        if (!engine.nativeLoadSvBank(selected.absolutePath)) {
            statusChanged(engine.nativeLastTextError().ifBlank { "SILVETON_BANK.sv load failed: ${selected.absolutePath}" })
            return
        }

        val soundCount = engine.nativeSoundCatalogNames().size
        if (soundCount <= 0) {
            statusChanged("SILVETON_BANK.sv loaded but contains no visible SOUND positions")
            return
        }
        statusChanged(null)
    }
'''
controller = controller[:old_start] + new_method + controller[next_method:]

if "fun bankLoadError(): String" not in controller:
    anchor = '    fun combiName(): String = engine.nativeCombiName().ifBlank { "COMBI" }\n'
    if anchor not in controller:
        raise SystemExit("controller combiName anchor not found for bank error accessor")
    controller = controller.replace(anchor, anchor + '\n    fun bankLoadError(): String = engine.nativeLastTextError()\n', 1)

for token in [
    "SilvetonRootRuntime.refreshBankStorageAccess()",
    'File("/storage/emulated/0/SILVETON_BANK.sv")',
    'File("/storage/emulated/0/Download/SILVETON_BANK.sv")',
    "engine.nativeSoundCatalogNames().size",
    "fun bankLoadError(): String",
]:
    if token not in controller:
        raise SystemExit(f"missing controller bank-load token: {token}")
controller_path.write_text(controller, encoding="utf-8")

# Do not silently show only EMPTY when bank loading failed. Surface native parser
# errors (including format/version/reference failures) in the SOUND picker.
view = view_path.read_text(encoding="utf-8")
start = view.find("    private fun showSoundPicker(upper:Int){")
if start < 0:
    raise SystemExit("showSoundPicker not found")
end = view.find("\n    private fun ", start + 10)
if end < 0:
    raise SystemExit("showSoundPicker end not found")
block = view[start:end]
if "BANK HAS NO SOUNDS" not in block:
    replacement = '''    private fun showSoundPicker(upper:Int){
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
'''
    view = view[:start] + replacement + view[end:]

for token in [
    'setTitle("BANK HAS NO SOUNDS")',
    "sampler.bankLoadError()",
    'labels.add("EMPTY")',
    '"%03d   %s".format(item.slot,item.name)',
]:
    if token not in view:
        raise SystemExit(f"missing SOUND picker diagnostic token: {token}")
view_path.write_text(view, encoding="utf-8")

print("PASS: Android 15 bank access + root performance envelope reassert + SOUND diagnostics installed")
