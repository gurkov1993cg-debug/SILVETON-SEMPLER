#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"

for p in (controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing workflow source: {p}")


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


# -----------------------------------------------------------------------------
# Controller: expose the native COMBI/LIST backend already present in the engine.
# Insert by function boundary instead of exact formatting so visual patches cannot
# break this workflow.
# -----------------------------------------------------------------------------
controller = controller_path.read_text(encoding="utf-8")
if "fun writeCombiSlot(" not in controller:
    insert_at = controller.find("    fun kscNames(")
    if insert_at < 0:
        raise SystemExit("controller insertion boundary not found")
    methods = '''    /** Fixed user COMBI positions are 1..500. */
    fun writeCombiSlot(slot: Int, name: String): Boolean {
        if (slot !in 1..500) return fail("COMBI position must be 1..500")
        val finalName = name.trim().ifBlank { combiName() }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) return false
        return nativeResult(engine.nativeActivateCombi(0, 0, slot))
    }

    fun firstFreeCombiSlot(): Int {
        for (slot in 1..500) {
            if (!engine.nativeCombiSlotOccupied(0, 0, slot)) return slot
        }
        return 500
    }

    fun activateCombiAddress(bank: Int, page: Int, slot: Int): Boolean =
        nativeResult(engine.nativeActivateCombi(bank, page, slot))

    /** LIST stores the exact COMBI address. Rewriting that COMBI later therefore
     * updates what the same LIST position opens without duplicating the blend. */
    fun addCombiToList(combiSlot: Int, name: String): Boolean {
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
    controller = controller[:insert_at] + methods + controller[insert_at:]

for token in [
    "fun writeCombiSlot(",
    "fun firstFreeCombiSlot()",
    "fun activateCombiAddress(",
    "fun addCombiToList(",
    "engine.nativeWriteCombi(0, 0, slot, finalName)",
    "engine.nativeActivateCombi(0, 0, slot)",
    "engine.nativeInsertListEntry(",
]:
    if token not in controller:
        raise SystemExit(f"missing controller COMBI/LIST workflow: {token}")
controller_path.write_text(controller, encoding="utf-8")


# -----------------------------------------------------------------------------
# View state.
# -----------------------------------------------------------------------------
view = view_path.read_text(encoding="utf-8")
if "private var editingCombiSlot: Int? = null" not in view:
    anchor = "    private var selectedUpper = 0"
    pos = view.find(anchor)
    if pos < 0:
        raise SystemExit("selectedUpper field not found")
    line_end = view.find("\n", pos)
    if line_end < 0:
        raise SystemExit("selectedUpper field line end not found")
    view = view[:line_end + 1] + "    private var editingCombiSlot: Int? = null\n" + view[line_end + 1:]


# -----------------------------------------------------------------------------
# COMBI: draw the buttons LAST in drawCombi(), so they stay visible even if an
# earlier skin draws other header content. Their hit zones are also registered
# last and therefore win over anything underneath them.
# -----------------------------------------------------------------------------
combi_buttons = '''        val writeR = RectF(r.right - 500f.dp, r.top + 15f.dp, r.right - 360f.dp, r.top + 57f.dp)
        button(c, writeR, "WRITE COMBI", true)
        hit(writeR, { _, _ -> showWriteCombi(false) })

        val addListR = RectF(r.right - 350f.dp, r.top + 15f.dp, r.right - 195f.dp, r.top + 57f.dp)
        button(c, addListR, "ADD TO LIST", true)
        hit(addListR, { _, _ -> showWriteCombi(true) })'''
view = insert_before_method_end(
    view,
    "    private fun drawCombi(c: Canvas, r: RectF)",
    combi_buttons,
    '"WRITE COMBI"'
)


# -----------------------------------------------------------------------------
# LIST: visible EDIT COMBI for the active LIST item. This jumps to the exact
# stored COMBI address, remembers its fixed position, and opens COMBI for editing.
# -----------------------------------------------------------------------------
list_edit = '''        if (activeItem != null) {
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
        }'''
view = insert_before_method_end(
    view,
    "    private fun drawList(c: Canvas, r: RectF)",
    list_edit,
    '"EDIT COMBI"'
)


# -----------------------------------------------------------------------------
# WRITE dialog: editable name + exact COMBI destination 1..500. ADD TO LIST first
# writes the COMBI, then adds a LIST reference to that exact slot.
# -----------------------------------------------------------------------------
if "private fun showWriteCombi(" not in view:
    insert_at = view.find("    private fun showSoundPicker")
    if insert_at < 0:
        raise SystemExit("showSoundPicker method boundary not found")
    method = '''    private fun showWriteCombi(addToList:Boolean){
        val box=LinearLayout(context).apply{orientation=LinearLayout.VERTICAL;setPadding(24,8,24,0)}
        box.addView(TextView(context).apply{text="NAME";setTextColor(Color.LTGRAY)})
        val name=EditText(context).apply{
            setText(sampler.combiName())
            setSelection(text.length)
            setTextColor(Color.WHITE)
        }
        box.addView(name)
        box.addView(TextView(context).apply{text="COMBI POSITION 1–500";setTextColor(Color.LTGRAY)})
        val defaultSlot=editingCombiSlot ?: sampler.firstFreeCombiSlot()
        val slot=EditText(context).apply{
            inputType=InputType.TYPE_CLASS_NUMBER
            setText(defaultSlot.toString())
            setSelection(text.length)
            setTextColor(Color.WHITE)
        }
        box.addView(slot)
        val title=if(addToList) "WRITE COMBI + ADD TO LIST" else "WRITE COMBI"
        AlertDialog.Builder(context).setTitle(title).setView(box).setPositiveButton("OK"){_,_->
            val position=slot.text.toString().toIntOrNull()?.coerceIn(1,500) ?: defaultSlot
            val finalName=name.text.toString().trim().ifBlank{sampler.combiName()}
            if(sampler.writeCombiSlot(position,finalName)){
                editingCombiSlot=position
                refreshAllUppers()
                refreshFx()
                if(addToList && sampler.addCombiToList(position,finalName)){
                    val size=sampler.listItems(0,0,0).size
                    listPage=((size-1).coerceAtLeast(0)/10).coerceIn(0,19)
                    page=Page.LIST
                }
            }
            invalidate()
        }.setNegativeButton("CANCEL",null).show()
    }

'''
    view = view[:insert_at] + method + view[insert_at:]


# Build-time contract: these are visible draw calls plus real native actions, not
# dead labels. Refuse the build if any part of the workflow disappears.
for token in [
    'button(c, writeR, "WRITE COMBI", true)',
    'button(c, addListR, "ADD TO LIST", true)',
    'button(c, editR, "EDIT COMBI", true)',
    "private fun showWriteCombi(",
    "sampler.writeCombiSlot(position,finalName)",
    "sampler.addCombiToList(position,finalName)",
    "sampler.activateCombiAddress(activeItem.combiBank, activeItem.combiPage, activeItem.combiSlot)",
    "editingCombiSlot = activeItem.combiSlot",
]:
    if token not in view:
        raise SystemExit(f"missing visible COMBI/LIST workflow: {token}")

view_path.write_text(view, encoding="utf-8")
print("PASS: robust visible WRITE COMBI / ADD TO LIST / EDIT COMBI workflow installed")
