#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
controller_path = root / "android/app/src/main/java/com/silveton/sampler/SamplerAppController.kt"
view_path = root / "android/app/src/main/java/com/silveton/sampler/SilvetonSamplerView.kt"
for p in (controller_path, view_path):
    if not p.is_file():
        raise SystemExit(f"missing LIST/UI source: {p}")


def match_brace(text: str, open_pos: int) -> int:
    depth = 0
    for i in range(open_pos, len(text)):
        ch = text[i]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return i
    return -1


def method_bounds(text: str, signature: str):
    start = text.find(signature)
    if start < 0:
        raise SystemExit(f"missing method: {signature}")
    line_end = text.find('\n', start)
    if line_end < 0:
        line_end = len(text)
    eq = text.find('=', start, line_end)
    brace = text.find('{', start, line_end + 1)
    if eq >= 0 and (brace < 0 or eq < brace):
        return start, line_end
    if brace < 0:
        brace = text.find('{', start)
    if brace < 0:
        raise SystemExit(f"method body missing: {signature}")
    close = match_brace(text, brace)
    if close < 0:
        raise SystemExit(f"method closing brace missing: {signature}")
    end = close + 1
    if end < len(text) and text[end] == '\n':
        end += 1
    return start, end


def ensure_import(text: str, anchor: str, statement: str) -> str:
    if statement in text:
        return text
    if anchor not in text:
        raise SystemExit(f"missing import anchor for {statement}")
    return text.replace(anchor, anchor + statement + "\n", 1)

# -----------------------------------------------------------------------------
# Controller: LIST item editing. LIST entries remain references to COMBI slots.
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
    "fun renameLiveListEntry(", "fun deleteLiveListEntry(", "fun moveLiveListEntry(",
    "fun copyLiveListEntry(", "engine.nativeSaveUserPerformanceState(userState.absolutePath)"
]:
    if token not in c:
        raise SystemExit(f"LIST controller contract missing: {token}")
controller_path.write_text(c, encoding="utf-8")

# -----------------------------------------------------------------------------
# View: long press uses the CURRENT touch engine. We wrap the existing
# onTouchEvent and delegate every event to it, so drag/immediate-touch behavior
# remains unchanged. No GestureDetector dependency.
# -----------------------------------------------------------------------------
v = view_path.read_text(encoding="utf-8")
v = ensure_import(v, "import android.app.AlertDialog\n", "import android.app.Dialog")
v = ensure_import(v, "import android.graphics.Color\n", "import android.graphics.drawable.GradientDrawable")
v = ensure_import(v, "import android.view.MotionEvent\n", "import android.view.Gravity")
v = ensure_import(v, "import android.view.Gravity\n", "import android.view.HapticFeedbackConstants")
v = ensure_import(v, "import android.view.HapticFeedbackConstants\n", "import android.view.ViewConfiguration")
v = ensure_import(v, "import android.view.ViewConfiguration\n", "import android.view.Window")
v = ensure_import(v, "import android.view.Window\n", "import android.view.WindowManager")
v = ensure_import(v, "import android.widget.EditText\n", "import android.widget.Button")

# Independent long-press hit map. Do not alter the existing Hit/drag structure.
if "private data class ListLongHit(" not in v:
    field_anchor = "    private var listPage = 0\n"
    if field_anchor not in v:
        raise SystemExit("listPage field anchor missing")
    fields = field_anchor + '''    private data class ListLongHit(val rect: RectF, val action: (Float, Float) -> Unit)
    private val listLongHits = ArrayList<ListLongHit>(24)
    private data class ListClipboard(val item: SamplerAppController.ListItem, val cut: Boolean)
    private var listClipboard: ListClipboard? = null
    private var listLongPressX = 0f
    private var listLongPressY = 0f
    private var listLongPressArmed = false
    private var listLongPressFired = false
    private val listLongPressSlop by lazy { ViewConfiguration.get(context).scaledTouchSlop.toFloat() }
    private val listLongPressRunnable = Runnable {
        if (!listLongPressArmed) return@Runnable
        val target = listLongHitAt(listLongPressX, listLongPressY) ?: return@Runnable
        listLongPressArmed = false
        listLongPressFired = true
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        target.action.invoke(listLongPressX, listLongPressY)
        invalidate()
    }
'''
    v = v.replace(field_anchor, fields, 1)

# Register/lookup helper next to the existing hit lookup.
if "private fun listLongHit(" not in v:
    hit_at = v.find("    private fun hitAt(")
    if hit_at < 0:
        raise SystemExit("hitAt helper missing")
    line_end = v.find('\n', hit_at)
    if line_end < 0:
        raise SystemExit("hitAt line end missing")
    helpers = '''
    private fun listLongHit(r: RectF, action: (Float, Float) -> Unit) {
        listLongHits.add(ListLongHit(RectF(r), action))
    }
    private fun listLongHitAt(x: Float, y: Float): ListLongHit? =
        listLongHits.asReversed().firstOrNull { it.rect.contains(x, y) }
'''
    v = v[:line_end + 1] + helpers + v[line_end + 1:]

# The long-hit map is rebuilt with the normal hit map every frame.
if "listLongHits.clear()" not in v:
    clear_anchor = "        hits.clear()\n"
    if clear_anchor not in v:
        raise SystemExit("hits.clear() anchor missing")
    v = v.replace(clear_anchor, clear_anchor + "        listLongHits.clear()\n", 1)

# Wrap whatever touch implementation is currently installed. This works with
# the immediate-touch/drag layer as well as the older expression-body handler.
if "private fun onTouchEventBase(event: MotionEvent): Boolean" not in v:
    sig = "    override fun onTouchEvent(event: MotionEvent): Boolean"
    start = v.find(sig)
    if start < 0:
        raise SystemExit("current onTouchEvent missing")
    s0, s1 = method_bounds(v, sig)
    original = v[s0:s1]
    base = original.replace("    override fun onTouchEvent(event: MotionEvent): Boolean",
                            "    private fun onTouchEventBase(event: MotionEvent): Boolean", 1)
    wrapper = r'''
    override fun onTouchEvent(event: MotionEvent): Boolean {
        when (event.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                listLongPressX = event.x
                listLongPressY = event.y
                listLongPressFired = false
                listLongPressArmed = listLongHitAt(event.x, event.y) != null
                removeCallbacks(listLongPressRunnable)
                if (listLongPressArmed) {
                    postDelayed(listLongPressRunnable, ViewConfiguration.getLongPressTimeout().toLong())
                }
            }
            MotionEvent.ACTION_MOVE -> {
                if (listLongPressArmed) {
                    val dx = event.x - listLongPressX
                    val dy = event.y - listLongPressY
                    if (dx * dx + dy * dy > listLongPressSlop * listLongPressSlop) {
                        listLongPressArmed = false
                        removeCallbacks(listLongPressRunnable)
                    }
                }
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                listLongPressArmed = false
                removeCallbacks(listLongPressRunnable)
                if (listLongPressFired) {
                    val cancel = MotionEvent.obtain(event)
                    cancel.action = MotionEvent.ACTION_CANCEL
                    onTouchEventBase(cancel)
                    cancel.recycle()
                    listLongPressFired = false
                    return true
                }
            }
        }
        return onTouchEventBase(event)
    }
'''
    v = v[:s0] + base.rstrip() + "\n" + wrapper + v[s1:]

# Add a long-press target to each populated LIST cell without changing its
# existing short-tap/drag callback.
if "showListContextMenu(idx, item)" not in v:
    d0, d1 = method_bounds(v, "    private fun drawList(c: Canvas, r: RectF)")
    block = v[d0:d1]
    act = block.find("sampler.activateListEntry(0,0,0,idx)")
    if act < 0:
        raise SystemExit("LIST activation anchor changed")
    item_start = block.rfind("if (item != null) {", 0, act)
    if item_start < 0:
        raise SystemExit("LIST item block missing")
    open_brace = block.find('{', item_start)
    close_brace = match_brace(block, open_brace)
    if close_brace < 0:
        raise SystemExit("LIST item block closing brace missing")
    insert = '''                listLongHit(rr) { _, _ -> showListContextMenu(idx, item) }\n'''
    block = block[:close_brace] + insert + block[close_brace:]
    v = v[:d0] + block + v[d1:]

# Context actions.
if "private fun showListContextMenu(" not in v:
    at = v.find("    private fun drawEffects(c: Canvas, r: RectF) {")
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
            if (source < 0) { listClipboard = null; return }
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

# Professional parameter editor page.
if "private fun showEditorPage(" not in v:
    at = v.find("    private fun showTwoInts(")
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
        inputType = if (numeric)
            InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED
        else InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
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

    private fun showEditorPage(title: String, subtitle: String, body: LinearLayout, applyLabel: String = "APPLY", onApply: () -> Unit) {
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
            text = title; setTextColor(Color.WHITE); textSize = 25f; setTypeface(typeface, Typeface.BOLD)
        })
        root.addView(TextView(context).apply {
            text = subtitle; setTextColor(Color.rgb(135, 151, 168)); textSize = 13f
            setPadding(0, uiPx(4f), 0, uiPx(14f))
        })
        root.addView(body, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        val buttons = LinearLayout(context).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.END; setPadding(0, uiPx(20f), 0, 0) }
        val cancel = Button(context).apply { text = "CANCEL"; isAllCaps = false }
        val apply = Button(context).apply { text = applyLabel; isAllCaps = false }
        buttons.addView(cancel); buttons.addView(apply); root.addView(buttons)
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
        val edit = editorField(current, false); body.addView(edit)
        showEditorPage(title, "Edit the name and confirm.", body) {
            val value = edit.text.toString().trim(); if (value.isNotEmpty()) done(value); invalidate()
        }
    }

'''
    v = v[:at] + helpers + v[at:]

# Replace the old compact numeric dialogs.
s0, s1 = method_bounds(v, "    private fun showTwoInts(")
v = v[:s0] + r'''    private fun showTwoInts(title:String,l1:String,v1:Int,l2:String,v2:Int,minV:Int,maxV:Int,done:(Int,Int)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel(l1)); val a = editorField(v1.toString()); body.addView(a)
        body.addView(editorLabel(l2)); val b = editorField(v2.toString()); body.addView(b)
        showEditorPage(title, "Allowed range: $minV … $maxV", body) {
            val x = a.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v1
            val y = b.text.toString().toIntOrNull()?.coerceIn(minV,maxV) ?: v2
            done(x,y); invalidate()
        }
    }

''' + v[s1:]

s0, s1 = method_bounds(v, "    private fun showNumber(")
v = v[:s0] + r'''    private fun showNumber(title:String,current:Float,minV:Float,maxV:Float,decimals:Int,done:(Float)->Unit){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("VALUE"))
        fun formatValue(value: Float): String = if (decimals == 0) value.toInt().toString() else "% .${decimals}f".format(value).trim()
        val edit = editorField(formatValue(current)); body.addView(edit)
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
        minus.setOnClickListener { adjustBy(-step) }; plus.setOnClickListener { adjustBy(step) }
        adjust.addView(minus); adjust.addView(plus); body.addView(adjust)
        showEditorPage(title, "Precise parameter entry", body) {
            val value = edit.text.toString().replace(',','.').toFloatOrNull()?.coerceIn(minV,maxV) ?: current
            done(value); invalidate()
        }
    }

''' + v[s1:]

# WRITE COMBI uses the same editor page.
if "private fun showWriteCombi(" in v:
    s0, s1 = method_bounds(v, "    private fun showWriteCombi(")
    v = v[:s0] + r'''    private fun showWriteCombi(addToList:Boolean){
        val body = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }
        body.addView(editorLabel("COMBI NAME")); val name = editorField(sampler.combiName(), false); body.addView(name)
        body.addView(editorLabel("COMBI POSITION   1 … 500"))
        val defaultSlot = editingCombiSlot ?: sampler.firstFreeCombiSlot()
        val slot = editorField(defaultSlot.toString()); body.addView(slot)
        val title = if(addToList) "WRITE COMBI + ADD TO LIST" else "WRITE COMBI"
        val subtitle = if(addToList) "Save the complete 7-UPPER COMBI, then add a LIST reference." else "Save the complete 7-UPPER COMBI to a fixed position."
        showEditorPage(title, subtitle, body, if(addToList) "WRITE + ADD" else "WRITE") {
            val position = slot.text.toString().toIntOrNull()?.coerceIn(1,500) ?: defaultSlot
            val finalName = name.text.toString().trim().ifBlank { sampler.combiName() }
            if(sampler.writeCombiSlot(position,finalName)){
                editingCombiSlot = position; refreshAllUppers(); refreshFx()
                if(addToList && sampler.addCombiToList(position,finalName)){
                    val size = sampler.listItems(0,0,0).size
                    listPage = ((size-1).coerceAtLeast(0)/10).coerceIn(0,19); page = Page.LIST
                }
            }
            invalidate()
        }
    }

''' + v[s1:]

for token in [
    "private data class ListLongHit(", "private fun onTouchEventBase(event: MotionEvent): Boolean",
    "ViewConfiguration.getLongPressTimeout()", "listLongHit(rr)", "showListContextMenu(idx, item)",
    'actions.add("COPY")', 'actions.add("CUT")', 'actions.add("MOVE")', 'actions.add("RENAME")',
    'actions.add("DELETE")', 'actions.add("PASTE BEFORE")', "private fun showEditorPage(",
    "Precise parameter entry", "WRITE COMBI + ADD TO LIST"
]:
    if token not in v:
        raise SystemExit(f"professional LIST/UI contract missing: {token}")

view_path.write_text(v, encoding="utf-8")
print("PASS: current-touch long-press LIST menu + persistent copy/cut/move/rename/delete + professional parameter pages installed")
