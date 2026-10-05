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
    private var owned = false
    private val instanceId = nextInstance.incrementAndGet()

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

    fun start(): Boolean = synchronized(lifecycleLock) { startLocked() }

    private fun startLocked(): Boolean {
        if (closed) return false
        if (!owned) { owned = true; SilvetonRootRuntime.acquire() }
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
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio open failed (${engine.nativeLastAudioError()})" })
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

    fun suspendAudio() = synchronized(lifecycleLock) {
        if (closed) return@synchronized
        // Keep the root performance envelope active while the sampler Activity
        // is alive. This avoids CPU/IRQ/governor churn on transient focus loss.
        engine.nativeAudioStop()
    }

    fun resumeAudio(): Boolean = synchronized(lifecycleLock) {
        var ok = resumeOnce()
        // A quick leave and re-enter can find the PCM still busy for a moment.
        var attempt = 0
        while (!ok && !closed && attempt < 4) {
            attempt++
            try { Thread.sleep(150L) } catch (_: InterruptedException) { break }
            engine.nativeAudioStop()
            engine.nativeAudioClose()
            ok = resumeOnce()
        }
        ok
    }

    private fun resumeOnce(): Boolean {
        if (closed) return false
        if (!owned) { owned = true; SilvetonRootRuntime.acquire() }
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
            statusChanged(engine.nativeLastTextError().ifBlank { "Audio reopen failed (${engine.nativeLastAudioError()})" })
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

    override fun close() = synchronized(lifecycleLock) {
        if (closed) return@synchronized
        closed = true
        running = false
        handler.removeCallbacks(recoveryPoll)
        recoveryThread.quitSafely()
        // The native sampler is shared by the whole process. If a newer
        // Activity already took over, closing now would silence it.
        if (instanceId == nextInstance.get()) {
            engine.nativePanic()
            engine.nativeAudioStop()
            engine.nativeAudioClose()
        }
        if (owned) { owned = false; SilvetonRootRuntime.release() }
        Unit
    }

    private companion object {
        val lifecycleLock = Any()
        val nextInstance = java.util.concurrent.atomic.AtomicInteger(0)
    }
}
