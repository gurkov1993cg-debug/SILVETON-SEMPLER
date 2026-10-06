package com.silveton.sampler

import android.os.Handler
import android.os.HandlerThread
import android.os.Process

/** Lifecycle owner for the rooted Direct-ALSA sampler stream. */
class SamplerRuntime(
    val engine: NativeSamplerEngine = NativeSamplerEngine(),
    private val statusChanged: (String?) -> Unit = {}
) : AutoCloseable {
    private val recoveryThread = HandlerThread(
        "SilvetonAudioRecovery",
        Process.THREAD_PRIORITY_BACKGROUND
    ).apply { start() }
    private val handler = Handler(recoveryThread.looper)
    private var running = false
    private var closed = false

    private fun promoteThreadsAsync() {
        Thread({
            try { Thread.sleep(80L) } catch (_: InterruptedException) { return@Thread }
            if (!closed) SilvetonRootRuntime.promoteRealtimeThreads()
        }, "SilvetonRtPromote").apply {
            priority = Thread.NORM_PRIORITY - 1
            start()
        }
    }

    private val recoveryPoll = object : Runnable {
        override fun run() {
            if (closed) return
            if (engine.nativeAudioNeedsRecovery()) {
                val root = SilvetonRootRuntime.enter()
                if (!root.ok) {
                    statusChanged(root.message)
                } else {
                    val perms = SilvetonRootRuntime.refreshAudioPermissions()
                    if (!perms.ok) {
                        statusChanged(perms.message)
                        handler.postDelayed(this, 250L)
                        return
                    }
                    val error = engine.nativeLastAudioError()
                    if (!engine.nativeAudioRecover()) {
                        statusChanged("Audio recovery failed: $error ${engine.nativeLastTextError()}")
                    } else if (!engine.nativeAudioStart()) {
                        statusChanged(engine.nativeLastTextError().ifBlank { "Audio restart failed (${engine.nativeLastAudioError()})" })
                    } else {
                        promoteThreadsAsync()
                        statusChanged(null)
                    }
                }
            }
            handler.postDelayed(this, 250L)
        }
    }

    fun start(): Boolean {
        if (closed) return false
        val root = SilvetonRootRuntime.enter()
        if (!root.ok) {
            statusChanged(root.message)
            return false
        }
        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()
        if (!audioPermissions.ok) statusChanged(audioPermissions.message)
        val midiPermissions = SilvetonRootRuntime.refreshMidiPermissions()
        if (!midiPermissions.ok) statusChanged(midiPermissions.message)
        if (!running) {
            running = true
            handler.post(recoveryPoll)
        }
        if (!engine.nativeAudioOpen()) {
            statusChanged(withRootNote(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" }, audioPermissions))
            // Keep root/SELinux grants while the Activity is alive: MIDI is
            // independent and audio can be retried without tearing permissions down.
            return false
        }
        if (!engine.nativeAudioStart()) {
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })
            return false
        }
        promoteThreadsAsync()
        statusChanged(null)
        return true
    }

    fun suspendAudio() {
        if (closed) return
        // Keep the root performance envelope active while the sampler Activity
        // is alive. This avoids CPU/IRQ/governor churn on transient focus loss.
        engine.nativeAudioStop()
    }

    fun resumeAudio(): Boolean {
        if (closed) return false
        val root = SilvetonRootRuntime.enter()
        if (!root.ok) {
            statusChanged(root.message)
            return false
        }
        val audioPermissions = SilvetonRootRuntime.refreshAudioPermissions()
        if (!audioPermissions.ok) statusChanged(audioPermissions.message)
        val midiPermissions = SilvetonRootRuntime.refreshMidiPermissions()
        if (!midiPermissions.ok) statusChanged(midiPermissions.message)
        if (engine.nativeAudioNeedsRecovery() && !engine.nativeAudioRecover()) {
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio recovery failed (${engine.nativeLastAudioError()})" })
            return false
        }
        if (!engine.nativeAudioOpen()) {
            statusChanged(withRootNote(engine.nativeLastTextError().ifBlank { "Audio reopen failed (${engine.nativeLastAudioError()})" }, audioPermissions))
            return false
        }
        val ok = engine.nativeAudioStart()
        if (!ok) {
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio start failed (${engine.nativeLastAudioError()})" })
        } else {
            promoteThreadsAsync()
            statusChanged(null)
        }
        return ok
    }

    /** Keeps the root mixer/permission problem visible next to the audio error. */
    private fun withRootNote(message: String, root: SilvetonRootRuntime.Result): String =
        if (root.ok || root.message.isBlank()) message else "$message | ROOT: ${root.message.lines().lastOrNull { it.isNotBlank() } ?: root.message}"

    override fun close() {
        if (closed) return
        closed = true
        running = false
        handler.removeCallbacks(recoveryPoll)
        recoveryThread.quitSafely()
        engine.nativePanic()
        engine.nativeAudioStop()
        engine.nativeAudioClose()
        SilvetonRootRuntime.leave()
    }
}
