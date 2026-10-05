#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"

for p in (controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing workflow source: {p}")

controller = controller_path.read_text(encoding="utf-8")

# Expose the already-existing native COMBI/LIST backend to the tablet UI.
if "fun writeCombiSlot(" not in controller:
    anchor = '''    fun activateListEntry(bank: Int, page: Int, slot: Int, index: Int): Boolean =
        nativeResult(engine.nativeActivateListEntry(bank, page, slot, index))
'''
    replacement = anchor + '''

    /** Fixed user COMBI positions are 1..500. Write then reactivate so a renamed
     *  COMBI immediately becomes the current editable state. */
    fun writeCombiSlot(slot: Int, name: String): Boolean {
        if (slot !in 1..500) return fail("COMBI position must be 1..500")
        val finalName = name.trim().ifBlank { combiName() }
        if (!nativeResult(engine.nativeWriteCombi(0, 0, slot, finalName))) return false
        return nativeResult(engine.nativeActivateCombi(0, 0, slot))
    }

    fun firstFreeCombiSlot(): Int {
        for (slot in 1..500) if (!engine.nativeCombiSlotOccupied(0, 0, slot)) return slot
        return 500
    }

    fun activateCombiAddress(bank: Int, page: Int, slot: Int): Boolean =
        nativeResult(engine.nativeActivateCombi(bank, page, slot))

    /** Append one saved COMBI to the live LIST. LIST stores the COMBI address,
     *  so later overwriting that COMBI automatically updates what the LIST opens. */
    fun addCombiToList(combiSlot: Int, name: String): Boolean {
        if (combiSlot !in 1..500) return fail("COMBI position must be 1..500")
        val current = listItems(0, 0, 0)
        if (current.size >= 200) return fail("LIST is full")
        val count = current.size + 1
        val numbers = IntArray(count)
        val names = Array(count) { "" }
        val addresses = IntArray(count * 3)
        current.forEachIndexed { index, item ->
            numbers[index] = item.number
            names[index] = item.name
            val base = index * 3
            addresses[base] = item.combiBank
            addresses[base + 1] = item.combiPage
            addresses[base + 2] = item.combiSlot
        }
        val index = current.size
        numbers[index] = index + 1
        names[index] = name.trim().ifBlank { combiName() }
        val base = index * 3
        addresses[base] = 0
        addresses[base + 1] = 0
        addresses[base + 2] = combiSlot
        return nativeResult(engine.nativeWriteList(0, 0, 0, "LIVE", numbers, names, addresses))
    }
'''
    if anchor not in controller:
        raise SystemExit("controller LIST activation anchor not found")
    controller = controller.replace(anchor, replacement, 1)

for token in [
    "fun writeCombiSlot(",
    "fun firstFreeCombiSlot()",
    "fun activateCombiAddress(",
    "fun addCombiToList(",
    "engine.nativeWriteCombi(0, 0, slot, finalName)",
    "engine.nativeWriteList(0, 0, 0, \"LIVE\"",
]:
    if token not in controller:
        raise SystemExit(f"missing controller COMBI/LIST workflow: {token}")
controller_path.write_text(controller, encoding="utf-8")

view = view_path.read_text(encoding="utf-8")

# Remember which fixed COMBI position is being edited. When LIST -> EDIT COMBI
# is used, WRITE COMBI defaults to exactly that position.
if "private var editingCombiSlot: Int? = null" not in view:
    anchor = "    private var selectedUpper = 0\n"
    if anchor not in view:
        raise SystemExit("selectedUpper field anchor not found")
    view = view.replace(anchor, anchor + "    private var editingCombiSlot: Int? = null\n", 1)

# Make WRITE COMBI and ADD TO LIST real, visible controls in the COMBI header.
if '"WRITE COMBI"' not in view:
    anchor = '''        val combiName = sampler.combiName()
        text(c, combiName, r.left + pad, r.top + titleH * 0.70f, 31f.dp, C_TEXT, bold = true)
'''
    addition = anchor + '''
        val writeR = RectF(r.right - 500f.dp, r.top + 15f.dp, r.right - 360f.dp, r.top + 57f.dp)
        button(c, writeR, "WRITE COMBI", true)
        hit(writeR, { _, _ -> showWriteCombi(false) })

        val addListR = RectF(r.right - 350f.dp, r.top + 15f.dp, r.right - 195f.dp, r.top + 57f.dp)
        button(c, addListR, "ADD TO LIST", true)
        hit(addListR, { _, _ -> showWriteCombi(true) })
'''
    if anchor not in view:
        raise SystemExit("COMBI title anchor not found")
    view = view.replace(anchor, addition, 1)

# Make LIST -> exact source COMBI editing visible without hiding it in gestures.
if '"EDIT COMBI"' not in view:
    anchor = '''        val currentName = activeItem?.name ?: "Силветон"
        text(c, currentName, left+95f*sx, nowBottom-17f*sy, 42f*ss, C_TEXT)
'''
    addition = anchor + '''
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
'''
    if anchor not in view:
        raise SystemExit("LIST current-name anchor not found")
    view = view.replace(anchor, addition, 1)

# Visible WRITE dialog: name + fixed COMBI position 1..500. ADD TO LIST writes
# the COMBI first, then appends a LIST entry that references that exact position.
if "private fun showWriteCombi(" not in view:
    anchor = "    private fun showSoundPicker(upper:Int){\n"
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
                refreshAllUppers(); refreshFx()
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
    if anchor not in view:
        raise SystemExit("showSoundPicker insertion anchor not found")
    view = view.replace(anchor, method + anchor, 1)

# When a LIST row itself is selected, remember its source COMBI position too.
old_hit = '''                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) refreshAllUppers()
                })
'''
new_hit = '''                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) {
                        editingCombiSlot = item.combiSlot
                        refreshAllUppers()
                        refreshFx()
                    }
                })
'''
if old_hit in view:
    view = view.replace(old_hit, new_hit, 1)
elif "editingCombiSlot = item.combiSlot" not in view:
    raise SystemExit("LIST row activation anchor not found")

for token in [
    '"WRITE COMBI"',
    '"ADD TO LIST"',
    '"EDIT COMBI"',
    "private fun showWriteCombi(",
    "sampler.writeCombiSlot(position,finalName)",
    "sampler.addCombiToList(position,finalName)",
    "sampler.activateCombiAddress(activeItem.combiBank, activeItem.combiPage, activeItem.combiSlot)",
]:
    if token not in view:
        raise SystemExit(f"missing visible COMBI/LIST workflow: {token}")

view_path.write_text(view, encoding="utf-8")
print("PASS: visible WRITE COMBI / ADD TO LIST / EDIT COMBI workflow installed")
