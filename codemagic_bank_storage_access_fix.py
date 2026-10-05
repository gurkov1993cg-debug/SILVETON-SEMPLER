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

# Android 11+ shared-storage access for direct mmap of SILVETON_BANK.sv.
manifest = manifest_path.read_text(encoding="utf-8")
perm = '    <uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE" />\n'
if "android.permission.MANAGE_EXTERNAL_STORAGE" not in manifest:
    anchor = '<manifest xmlns:android="http://schemas.android.com/apk/res/android">\n'
    if anchor not in manifest:
        raise SystemExit("AndroidManifest root anchor not found")
    manifest = manifest.replace(anchor, anchor + perm, 1)
manifest_path.write_text(manifest, encoding="utf-8")

runtime = root_runtime_path.read_text(encoding="utf-8")

# Never trust only the cached active flag: Android may recreate ALSA nodes or
# restore scheduler state while this process remains alive.
early = '        if (active) return@synchronized Result(true)\n\n'
if early in runtime:
    runtime = runtime.replace(early, '        // Reassert live sampler grants/tuning even when already active.\n\n', 1)
if 'if (active) return@synchronized Result(true)' in runtime:
    raise SystemExit("stale root active short-circuit remains")

if "MANAGE_EXTERNAL_STORAGE allow" not in runtime:
    anchor = '''            STATE=/data/local/tmp/silveton-root-state
            mkdir -p "${'$'}STATE"
'''
    addition = '''            STATE=/data/local/tmp/silveton-root-state
            cmd appops set com.silveton.sampler MANAGE_EXTERNAL_STORAGE allow >/dev/null 2>&1 || true
            mkdir -p "${'$'}STATE"
'''
    if anchor not in runtime:
        raise SystemExit("SilvetonRootRuntime STATE anchor not found")
    runtime = runtime.replace(anchor, addition, 1)

if "fun refreshBankStorageAccess(): Result" not in runtime:
    at = runtime.find("    fun refreshAudioPermissions(): Result")
    if at < 0:
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
    runtime = runtime[:at] + method + runtime[at:]

root_runtime_path.write_text(runtime, encoding="utf-8")

# Load bank from the known appliance locations. The file stays in place and the
# native runtime mmap's it directly.
controller = controller_path.read_text(encoding="utf-8")
start = controller.find("    private fun loadDefaultBankOnce() {")
end = controller.find("\n    fun pauseAudio()", start)
if start < 0 or end < 0:
    raise SystemExit("loadDefaultBankOnce boundaries not found")

new_method = '''    private fun loadDefaultBankOnce() {
        if (defaultBankAttempted) return
        defaultBankAttempted = true

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
        val soundCount = engine.nativeSoundCatalogNames().size
        if (soundCount <= 0) {
            statusChanged("SILVETON_BANK.sv loaded but contains no visible SOUND positions")
            return
        }
        statusChanged(null)
    }
'''
controller = controller[:start] + new_method + controller[end:]

if "fun bankLoadError(): String" not in controller:
    anchor = '    fun combiName(): String = engine.nativeCombiName().ifBlank { "COMBI" }\n'
    if anchor not in controller:
        raise SystemExit("controller combiName anchor not found")
    controller = controller.replace(anchor, anchor + '\n    fun bankLoadError(): String = engine.nativeLastTextError()\n', 1)
controller_path.write_text(controller, encoding="utf-8")

# Expose actual native bank errors instead of silently presenting an empty list.
view = view_path.read_text(encoding="utf-8")
start = view.find("    private fun showSoundPicker(upper:Int){")
end = view.find("\n    private fun ", start + 10)
if start < 0 or end < 0:
    raise SystemExit("showSoundPicker boundaries not found")
if "BANK HAS NO SOUNDS" not in view[start:end]:
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
view_path.write_text(view, encoding="utf-8")

# FINAL audio guard. It runs after every bank/UI/runtime rewrite. It may repair
# the harmless active short-circuit, but it deliberately refuses any build that
# disables global SELinux, thermal protection or speaker-protection mechanisms.
guard = Path(__file__).with_name("codemagic_final_audio_guard.py")
if not guard.is_file():
    raise SystemExit(f"missing final audio guard: {guard}")
exec(compile(guard.read_text(encoding="utf-8"), str(guard), "exec"), {
    "__name__": "__main__",
    "__file__": str(guard),
})

final_root = root_runtime_path.read_text(encoding="utf-8")
final_controller = controller_path.read_text(encoding="utf-8")
final_view = view_path.read_text(encoding="utf-8")
for token in [
    "MANAGE_EXTERNAL_STORAGE allow",
    "fun refreshBankStorageAccess(): Result",
]:
    if token not in final_root:
        raise SystemExit(f"missing final bank/root token: {token}")
for token in [
    "SilvetonRootRuntime.refreshBankStorageAccess()",
    'File("/storage/emulated/0/SILVETON_BANK.sv")',
    "engine.nativeSoundCatalogNames().size",
    "fun bankLoadError(): String",
]:
    if token not in final_controller:
        raise SystemExit(f"missing final bank loader token: {token}")
for token in ['setTitle("BANK HAS NO SOUNDS")', "sampler.bankLoadError()"]:
    if token not in final_view:
        raise SystemExit(f"missing final SOUND diagnostic token: {token}")

print("PASS: bank access + final Direct ALSA guard applied without disabling physical protections")
