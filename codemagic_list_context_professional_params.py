#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"

for p in (controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing LIST/UI source: {p}")

# -----------------------------------------------------------------------------
# Controller: complete LIST-entry management with persistence.
# These operations edit only LIST references; COMBI remains the single source of
# truth. Every mutation is persisted to SILVETON_USER_STATE.SVSET.
# -----------------------------------------------------------------------------
c = controller_path.read_text(encoding="utf-8")
if "fun renameLiveListEntry(" not in c:
    anchor = '''    fun activateListEntry(bank: Int, page: Int, slot: Int, index: Int): Boolean =
        nativeResult(engine.nativeActivateListEntry(bank, page, slot, index))
'''
    if anchor not in c:
        raise SystemExit("controller activateListEntry anchor changed")
    methods = anchor + '''

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
        val item = listItems(0, 0, 0).getOrNull(index) ?: return fail("LIST entry is not available")
        val finalName = name.trim()
        if (finalName.isEmpty()) return fail("LIST name cannot be empty")
        if (!nativeResult(engine.nativeSetListEntry(
                0, 0, 0, index, item.number, finalName,
                item.combiBank, item.combiPage, item.combiSlot
            ))) return false
        return persistLiveListEdits()
    }

    fun deleteLiveListEntry(index: Int): Boolean {
        val items = listItems(0, 0, 0)
        if (index !in items.indices) return fail("LIST entry is not available")
        if (!nativeResult(engine.nativeRemoveListEntry(0, 0, 0, index))) return false
        if (!renumberLiveList()) return false
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
'''
    c = c.replace(anchor, methods, 1)

for token in [
    "fun renameLiveListEntry(",
    "fun deleteLiveListEntry(",
    "fun moveLiveListEntry(",
    "fun copyLiveListEntry(",
    "engine.nativeRemoveListEntry(0, 0, 0, index)",
    "engine.nativeMoveListEntry(0, 0, 0, fromIndex, toIndex)",
    "engine.nativeInsertListEntry(",
    "engine.nativeSaveUserPerformanceState(userState.absolutePath)",
]:
    if token not in c:
        raise SystemExit(f"LIST controller contract missing: {token}")
controller_path.write_text(c, encoding="utf-8")

# -----------------------------------------------------------------------------
# View: long-press context menu + professional parameter editor pages.
# -----------------------------------------------------------------------------
v = view_path.read_text(encoding="utf-8")

# Imports needed by the custom editor surface.
imports = [
    ("import android.app.AlertDialog\n", "import android.app.AlertDialog\nimport android.app.Dialog\n"),
    ("import android.graphics.Color\n", "import android.graphics.Color\nimport android.graphics.drawable.GradientDrawable\n"),
    ("import android.view.GestureDetector\n", "import android.view.GestureDetector\nimport android.view.Gravity\nimport android.view.Window\nimport android.view.WindowManager\n"),
    ("import android.widget.EditText\n", "import android.widget.Button\nimport android.widget.EditText\n"),
]
for old, new in imports:
    if new.splitlines()[-1] not in v:
        if old not in v:
            raise SystemExit(f"UI import anchor missing: {old.strip()}")
        v = v.replace(old, new, 1)

# Hit targets gain a genuine long-press action. Double-tap reset remains intact.
old_hit = '''    private data class Hit(
        val rect: RectF,
        val tap: (Float, Float) -> Unit,
        val reset: (() -> Unit)? = null
    )'''
new_hit = '''    private data class Hit(
        val rect: RectF,
        val tap: (Float, Float) -> Unit,
        val reset: (() -> Unit)? = null,
        val longPress: ((Float, Float) -> Unit)? = null
    )'''
if old_hit in v:
    v = v.replace(old_hit, new_hit, 1)
elif "val longPress: ((Float, Float) -> Unit)?" not in v:
    raise SystemExit("Hit structure anchor changed")

# Clipboard state for right-click-style LIST workflow.
if "private data class ListClipboard(" not in v:
    field_anchor = "    private var listPage = 0\n"
    if field_anchor not in v:
        raise SystemExit("listPage field anchor missing")
    fields = field_anchor + '''    private data class ListClipboard(val item: SamplerAppController.ListItem, val cut: Boolean)
    private var listClipboard: ListClipboard? = null
'''
    v = v.replace(field_anchor, fields, 1)

# GestureDetector long-press dispatch.
if "override fun onLongPress(e: MotionEvent)" not in v:
    anchor = '''        override fun onDoubleTap(e: MotionEvent): Boolean {
            val hit = hitAt(e.x, e.y)
            if (hit?.reset != null) hit.reset.invoke() else hit?.tap?.invoke(e.x, e.y)
            invalidate()
            return true
        }
'''
    if anchor not in v:
        raise SystemExit("gesture detector anchor changed")
    replacement = anchor + '''        override fun onLongPress(e: MotionEvent) {
            hitAt(e.x, e.y)?.longPress?.invoke(e.x, e.y)
            invalidate()
        }
'''
    v = v.replace(anchor, replacement, 1)

# Hit registration accepts optional longPress while preserving all existing call sites.
old_hit_fn = '    private fun hit(r:RectF,tap:(Float,Float)->Unit,reset:(()->Unit)?=null){hits.add(Hit(RectF(r),tap,reset))}\n'
new_hit_fn = '    private fun hit(r:RectF,tap:(Float,Float)->Unit,reset:(()->Unit)?=null,longPress:((Float,Float)->Unit)?=null){hits.add(Hit(RectF(r),tap,reset,longPress))}\n'
if old_hit_fn in v:
    v = v.replace(old_hit_fn, new_hit_fn, 1)
elif "longPress:((Float,Float)->Unit)?=null" not in v:
    raise SystemExit("hit() registration anchor changed")

# Every populated LIST cell: short tap plays, long press opens context menu.
old_row_hit = '''                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) refreshAllUppers()
                })'''
new_row_hit = '''                hit(rr, { _, _ ->
                    if (sampler.activateListEntry(0,0,0,idx)) refreshAllUppers()
                }, longPress = { _, _ -> showListContextMenu(idx, item) })'''
if old_row_hit in v:
    v = v.replace(old_row_hit, new_row_hit, 1)
elif "showListContextMenu(idx, item)" not in v:
    raise SystemExit("LIST row hit anchor changed")

# Insert context-menu workflow before effects page.
if "private fun showListContextMenu(" not in v:
    anchor = "    private fun drawEffects(c: Canvas, r: RectF) {\n"
    at = v.find(anchor)
    if at < 0:
        raise SystemExit("drawEffects insertion anchor missing")
    methods = r'''    private fun editListCombi(item: SamplerAppController.ListItem) {
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
            if (source < 0) {
                listClipboard = null
                return
            }
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

'''
    v = v[:at] + methods + v[at:]

# Professional reusable parameter-page shell.
if "private fun showEditorPage(" not in v:
    anchor = "    private fun showTwoInts("
    at = v.find(anchor)
    if at < 0:
        raise SystemExit("showTwoInts insertion anchor missing")
    helpers = r'''    private fun uiPx(dp: Float): Int = (dp * resources.displayMetrics.density + 0.5f).toInt()

    private fun editorLabel(textValue: String): TextView = TextView(context).apply {
        text = textValue
        setTextColor(Color.rgb(158, 170, 184))
        textSize = 12f
        setPadding(0, uiPx(10f), 0, uiPx(6f))
    }

    private fun editorField(value: String, numeric: Boolean = true): EditText = EditText(context).apply {
        if (numeric) inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
        else inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
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

    private fun showEditorPage(
        title: String,
        subtitle: String,
        body: LinearLayout,
        applyLabel: String = "APPLY",
        onApply: () -> Unit
    ) {
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
            text = title
            setTextColor(Color.WHITE)
            textSize = 25f
            setTypeface(typeface, Typeface.BOLD)
        })
        root.addView(TextView(context).apply {
            text = subtitle
            setTextColor(Color.rgb(135, 151, 168))
            textSize = 13f
            setPadding(0, uiPx(4f), 0, uiPx(14f))
        })
        root.addView(body, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        val buttons = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.END
            setPadding(0, uiPx(20f), 0, 0)
        }
        val cancel = Button(context).apply { text = "CANCEL"; isAllCaps = false }
        val apply = Button(context).apply { text = applyLabel; isAllCaps = false }
        buttons.addView(cancel)
        buttons.addView(apply)
        root.addView(buttons)
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
        val edit = editorField(current, false)
        body.addView(edit)
        showEditorPage(title, "Edit the name and confirm.", body) {
            val value = edit.text.toString().trim()
            if (value.isNotEmpty()) done(value)
            invalidate()
        }
    }

'''
    v = v[:at] + helpers + v[at:]

# Replace the old small two-field AlertDialog with the professional page.
def method_bounds(text, signature):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing method {signature}")
    candidates = []
    for token in ("\n    private fun ", "\n    fun ", "\n    override fun "):
        pos = text.find(token, start + len(signature))
        if pos >= 0:
            candidates.append(pos)
    return start, min(candidates) if candidates else len(text)

s0, s1 = method_bounds(v, "    private fun showTwoInts(")
v = v[:s0] + r'''    private fun showTwoInts(title:String,l1:String,v1:Int,l2:String,v2:Int,minV:Int,maxV:Int,done:(Int,Int)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel(l1))
        val a = editorField(v1.toString())
        body.addView(a)
        body.addView(editorLabel(l2))
        val b = editorField(v2.toString())
        body.addView(b)
        showEditorPage(title, "Allowed range: $minV … $maxV", body) {
            val x = a.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v1
            val y = b.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v2
            done(x,y)
            invalidate()
        }
    }

''' + v[s1:]

# Replace showNumber with a page that has range, large value and real +/- step controls.
s0, s1 = method_bounds(v, "    private fun showNumber(")
v = v[:s0] + r'''    private fun showNumber(title:String,current:Float,minV:Float,maxV:Float,decimals:Int,done:(Float)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("VALUE"))
        fun formatValue(value: Float): String = if (decimals == 0) value.toInt().toString() else "% .${decimals}f".format(value).trim()
        val edit = editorField(formatValue(current))
        body.addView(edit)
        body.addView(editorLabel("RANGE   ${formatValue(minV)}  …  ${formatValue(maxV)}"))
        val step = when(decimals){ 0 -> 1f; 1 -> 0.1f; else -> 0.01f }
        val adjust = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER }
        val minus = Button(context).apply { text = "− ${formatValue(step)}"; isAllCaps = false }
        val plus = Button(context).apply { text = "+ ${formatValue(step)}"; isAllCaps = false }
        fun adjustBy(delta: Float) {
            val now = edit.text.toString().replace(',','.').toFloatOrNull() ?: current
            val next = (now + delta).coerceIn(minV,maxV)
            edit.setText(formatValue(next)); edit.setSelection(edit.text.length)
        }
        minus.setOnClickListener { adjustBy(-step) }
        plus.setOnClickListener { adjustBy(step) }
        adjust.addView(minus)
        adjust.addView(plus)
        body.addView(adjust)
        showEditorPage(title, "Precise parameter entry", body) {
            val value = edit.text.toString().replace(',','.').toFloatOrNull()?.coerceIn(minV,maxV) ?: current
            done(value)
            invalidate()
        }
    }

''' + v[s1:]

# Upgrade WRITE COMBI from a small generic popup to the same professional page.
if "private fun showWriteCombi(" in v:
    s0, s1 = method_bounds(v, "    private fun showWriteCombi(")
    replacement = r'''    private fun showWriteCombi(addToList:Boolean){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("COMBI NAME"))
        val name = editorField(sampler.combiName(), false)
        body.addView(name)
        body.addView(editorLabel("COMBI POSITION   1 … 500"))
        val defaultSlot = editingCombiSlot ?: sampler.firstFreeCombiSlot()
        val slot = editorField(defaultSlot.toString())
        body.addView(slot)
        val title = if(addToList) "WRITE COMBI + ADD TO LIST" else "WRITE COMBI"
        val subtitle = if(addToList) "Save the complete 7-UPPER COMBI, then add a LIST reference." else "Save the complete 7-UPPER COMBI to a fixed position."
        showEditorPage(title, subtitle, body, if(addToList) "WRITE + ADD" else "WRITE") {
            val position = slot.text.toString().toIntOrNull()?.coerceIn(1,500) ?: defaultSlot
            val finalName = name.text.toString().trim().ifBlank { sampler.combiName() }
            if(sampler.writeCombiSlot(position,finalName)){
                editingCombiSlot = position
                refreshAllUppers()
                refreshFx()
                if(addToList && sampler.addCombiToList(position,finalName)){
                    val size = sampler.listItems(0,0,0).size
                    listPage = ((size-1).coerceAtLeast(0)/10).coerceIn(0,19)
                    page = Page.LIST
                }
            }
            invalidate()
        }
    }

'''
    v = v[:s0] + replacement + v[s1:]

for token in [
    "override fun onLongPress(e: MotionEvent)",
    "showListContextMenu(idx, item)",
    'actions.add("COPY")',
    'actions.add("CUT")',
    'actions.add("MOVE")',
    'actions.add("RENAME")',
    'actions.add("DELETE")',
    'actions.add("PASTE BEFORE")',
    "sampler.renameLiveListEntry(index, newName)",
    "sampler.deleteLiveListEntry(index)",
    "sampler.moveLiveListEntry(",
    "sampler.copyLiveListEntry(clip.item, insertion)",
    "private fun showEditorPage(",
    "Precise parameter entry",
    "WRITE COMBI + ADD TO LIST",
]:
    if token not in v:
        raise SystemExit(f"professional LIST/UI contract missing: {token}")

view_path.write_text(v, encoding="utf-8")
print("PASS: long-press LIST context menu + persistent copy/cut/move/rename/delete + professional parameter pages installed")
