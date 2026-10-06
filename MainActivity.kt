package com.silveton.sampler

import android.content.pm.ActivityInfo
import android.os.Bundle
import android.view.KeyEvent
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.util.concurrent.Executors

/** Android lifecycle host for the functional SILVETON SAMPLER touchscreen UI. */
class MainActivity : AppCompatActivity() {
    private lateinit var sampler: SamplerAppController
    private lateinit var samplerView: SilvetonSamplerView
    private val audioLifecycle = Executors.newSingleThreadExecutor { runnable ->
        Thread(runnable, "SilvetonAudioLifecycle").apply { priority = Thread.NORM_PRIORITY + 1 }
    }
    @Volatile private var created = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE
        window.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        hideSystemUi()

        sampler = SamplerAppController(
            this,
            statusChanged = { warning ->
                if (::samplerView.isInitialized) runOnUiThread { if (created) samplerView.setWarning(warning) }
            },
            midiConnectionChanged = { count ->
                if (::samplerView.isInitialized) runOnUiThread { if (created) samplerView.setMidiConnections(count) }
            }
        )
        samplerView = SilvetonSamplerView(this, sampler)
        setContentView(samplerView)
        created = true
    }

    override fun onStart() {
        super.onStart()
        if (created) audioLifecycle.execute { sampler.start() }
    }

    override fun onResume() {
        super.onResume()
        hideSystemUi()
        if (created) audioLifecycle.execute { sampler.resumeAudio() }
    }

    override fun onPause() {
        if (created) audioLifecycle.execute { sampler.pauseAudio() }
        super.onPause()
    }

    override fun onDestroy() {
        if (created) audioLifecycle.execute { sampler.close() }
        audioLifecycle.shutdown()
        super.onDestroy()
    }

    // Tablet volume keys change SILVETON's own MASTER level. They are kept away
    // from Android: Android's volume change and its click sound went through the
    // tablet's own audio system and broke SILVETON's direct sound.
    private var volumeToast: Toast? = null

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val step = when (keyCode) {
            KeyEvent.KEYCODE_VOLUME_UP -> 2f
            KeyEvent.KEYCODE_VOLUME_DOWN -> -2f
            KeyEvent.KEYCODE_VOLUME_MUTE -> 0f
            else -> return super.onKeyDown(keyCode, event)
        }
        if (created && step != 0f) {
            val db = sampler.nudgeMasterGain(step)
            if (db != null) {
                samplerView.globalChangedOutside()
                volumeToast?.cancel()
                val text = "MASTER " + (if (db > 0f) "+" else "") + String.format(java.util.Locale.US, "%.0f dB", db)
                volumeToast = Toast.makeText(this, text, Toast.LENGTH_SHORT).also { it.show() }
            }
        }
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean = when (keyCode) {
        KeyEvent.KEYCODE_VOLUME_UP, KeyEvent.KEYCODE_VOLUME_DOWN, KeyEvent.KEYCODE_VOLUME_MUTE -> true
        else -> super.onKeyUp(keyCode, event)
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        if (hasFocus) hideSystemUi()
    }

    private fun hideSystemUi() {
        WindowCompat.setDecorFitsSystemWindows(window, false)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }
    }
}
