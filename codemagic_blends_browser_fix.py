#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
cpp_path = root / "android/app/src/main/cpp/SamplerAudioBridge.cpp"
native_path = root / "android/app/src/main/java/com/silveton/sampler/NativeSamplerEngine.kt"
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"

for p in (cpp_path, native_path, controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing BLENDS source: {p}")

# ---- Native catalog: expose the real occupied COMBI slots 1..500 and names. ----
cpp = cpp_path.read_text(encoding="utf-8")
if "combiCatalogSlots() const" not in cpp:
    anchor = '    std::string combiName() const { return state_ == nullptr ? std::string{} : state_->combi.name; }\n'
    methods = r'''

    std::vector<int> combiCatalogSlots() const {
        std::vector<int> out;
        out.reserve(500U);
        for (int slot = 1; slot <= 500; ++slot) {
            SlotAddress address{};
            address.bank = 0;
            address.page = 0;
            address.slot = static_cast<std::uint16_t>(slot);
            if (library_.combi(address).has_value()) out.push_back(slot);
        }
        return out;
    }

    std::vector<std::string> combiCatalogNames() const {
        std::vector<std::string> out;
        out.reserve(500U);
        for (int slot = 1; slot <= 500; ++slot) {
            SlotAddress address{};
            address.bank = 0;
            address.page = 0;
            address.slot = static_cast<std::uint16_t>(slot);
            const auto combi = library_.combi(address);
            if (combi.has_value()) out.push_back(combi->name);
        }
        return out;
    }
'''
    if anchor not in cpp:
        raise SystemExit("native combiName anchor not found")
    cpp = cpp.replace(anchor, anchor + methods, 1)

if "nativeCombiCatalogSlots" not in cpp:
    anchor = 'extern "C" JNIEXPORT jstring JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiName(JNIEnv* env,jobject){const auto s=sampler().combiName();return env->NewStringUTF(s.c_str());}\n'
    jni = (
        'extern "C" JNIEXPORT jintArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiCatalogSlots(JNIEnv* env,jobject){return intVectorArray(env,sampler().combiCatalogSlots());}\n'
        'extern "C" JNIEXPORT jobjectArray JNICALL Java_com_silveton_sampler_NativeSamplerEngine_nativeCombiCatalogNames(JNIEnv* env,jobject){return stringVectorArray(env,sampler().combiCatalogNames());}\n'
    )
    if anchor not in cpp:
        raise SystemExit("nativeCombiName JNI anchor not found")
    cpp = cpp.replace(anchor, anchor + jni, 1)

for token in ["combiCatalogSlots() const", "combiCatalogNames() const", "nativeCombiCatalogSlots", "nativeCombiCatalogNames"]:
    if token not in cpp:
        raise SystemExit(f"missing native BLENDS catalog token: {token}")
cpp_path.write_text(cpp, encoding="utf-8")

# ---- Kotlin JNI declarations. ----
native = native_path.read_text(encoding="utf-8")
if "nativeCombiCatalogSlots" not in native:
    anchor = "    external fun nativeCombiName(): String\n"
    addition = (
        "    external fun nativeCombiCatalogSlots(): IntArray\n"
        "    external fun nativeCombiCatalogNames(): Array<String>\n"
    )
    if anchor not in native:
        raise SystemExit("NativeSamplerEngine combiName declaration anchor not found")
    native = native.replace(anchor, anchor + addition, 1)
native_path.write_text(native, encoding="utf-8")

# ---- Controller catalog: one pair per real occupied COMBI. ----
controller = controller_path.read_text(encoding="utf-8")
if "fun combiCatalog(): List<Pair<Int, String>>" not in controller:
    anchor = '    fun combiName(): String = engine.nativeCombiName().ifBlank { "COMBI" }\n'
    addition = r'''

    fun combiCatalog(): List<Pair<Int, String>> {
        val slots = engine.nativeCombiCatalogSlots()
        val names = engine.nativeCombiCatalogNames()
        val count = minOf(slots.size, names.size)
        return List(count) { index -> slots[index] to names[index] }
    }
'''
    if anchor not in controller:
        raise SystemExit("controller combiName anchor not found")
    controller = controller.replace(anchor, anchor + addition, 1)
if "engine.nativeCombiCatalogSlots()" not in controller or "engine.nativeCombiCatalogNames()" not in controller:
    raise SystemExit("controller BLENDS catalog is incomplete")
controller_path.write_text(controller, encoding="utf-8")

# ---- Visible COMBI browser. ----
def method_bounds(text: str, signature: str):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing method: {signature}")
    candidates = []
    for token in ("\n    private fun ", "\n    override fun ", "\n    fun "):
        pos = text.find(token, start + len(signature))
        if pos >= 0:
            candidates.append(pos)
    end = min(candidates) if candidates else len(text)
    return start, end


def insert_before_method_end(text: str, signature: str, code: str, marker: str) -> str:
    if marker in text:
        return text
    start, end = method_bounds(text, signature)
    block = text[start:end]
    close = block.rfind("\n    }")
    if close < 0:
        raise SystemExit(f"cannot locate closing brace for {signature}")
    block = block[:close] + "\n" + code.rstrip() + "\n" + block[close:]
    return text[:start] + block + text[end:]

view = view_path.read_text(encoding="utf-8")
blends_button = r'''        val blendsR = RectF(r.right - 665f.dp, r.top + 15f.dp, r.right - 510f.dp, r.top + 57f.dp)
        button(c, blendsR, "BLENDS", true)
        hit(blendsR, { _, _ -> showBlendPicker() })'''
view = insert_before_method_end(
    view,
    "    private fun drawCombi(c: Canvas, r: RectF)",
    blends_button,
    'button(c, blendsR, "BLENDS", true)'
)

if "private fun showBlendPicker()" not in view:
    insert_at = view.find("    private fun showWriteCombi(")
    if insert_at < 0:
        insert_at = view.find("    private fun showSoundPicker")
    if insert_at < 0:
        raise SystemExit("BLENDS picker insertion boundary not found")
    picker = r'''    private fun showBlendPicker(){
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

'''
    view = view[:insert_at] + picker + view[insert_at:]

for token in [
    'button(c, blendsR, "BLENDS", true)',
    "showBlendPicker()",
    "sampler.combiCatalog()",
    'setTitle("BLENDS  1–500")',
    "sampler.activateCombiAddress(0,0,item.first)",
    "editingCombiSlot = item.first",
]:
    if token not in view:
        raise SystemExit(f"missing visible BLENDS browser token: {token}")
view_path.write_text(view, encoding="utf-8")

print("PASS: real saved COMBI/BLENDS 1..500 browser installed and wired")
