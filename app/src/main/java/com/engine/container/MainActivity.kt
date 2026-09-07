package com.engine.container

import android.annotation.SuppressLint
import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.widget.ProgressBar
import android.widget.TextView
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileOutputStream
import java.io.InputStream
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val TAG = "EngineContainer"
        private const val ROOTFS_DIR_NAME = "rootfs"
        private const val ROOTFS_ASSET_NAME = "rootfs_a13.zip"

        init {
            System.loadLibrary("engine")
        }
    }

    // ========================================================================
    // Native JNI Engine Interface
    // ========================================================================
    private external fun nativeOnSurfaceCreated(surface: Surface)
    private external fun nativeOnSurfaceChanged(width: Int, height: Int)
    private external fun nativeOnSurfaceDestroyed()
    private external fun startContainer(rootfsPath: String, hookSoPath: String): Int
    private external fun stopContainer()
    private external fun isContainerRunning(): Boolean
    private external fun sendTouchEvent(
        action: Int,
        pointerId: Int,
        x: Float,
        y: Float,
        pressure: Float,
        timestamp: Long
    ): Boolean
    private external fun nativeInitInputServer(socketName: String): Boolean

    // Frame streamer JNI
    private external fun nativeStartFrameStreamer(): Boolean
    private external fun nativeStopFrameStreamer()
    private external fun nativeGetFrameStreamerFps(): Float
    private external fun nativeIsFrameStreamerActive(): Boolean

    // View References
    private lateinit var surfaceView: SurfaceView
    private lateinit var statusIndicator: View
    private lateinit var textRuntimeState: TextView
    private lateinit var textDisplayStats: TextView
    private lateinit var textRootfsState: TextView
    private lateinit var extractionProgress: ProgressBar

    // FPS polling
    private val uiHandler = Handler(Looper.getMainLooper())
    private val fpsPoller = object : Runnable {
        override fun run() {
            val fps = nativeGetFrameStreamerFps()
            val active = nativeIsFrameStreamerActive()
            val fpsStr = if (active) "%.1f fps | LIVE".format(fps) else "connecting..."
            textDisplayStats.text = "DISPLAY: $fpsStr"
            uiHandler.postDelayed(this, 1000)
        }
    }

    // Orchestration State Flags
    private val isSurfaceReady = AtomicBoolean(false)
    private val isRootfsReady = AtomicBoolean(false)
    private val isStartingContainer = AtomicBoolean(false)

    // Background executor for I/O and process orchestration
    private val backgroundExecutor = Executors.newSingleThreadExecutor()

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        applyImmersiveFullscreen()
        initViews()

        // Bind SurfaceHolder callbacks to route ANativeWindow into C++ NDK
        surfaceView.holder.addCallback(this)

        // Capture direct touch events on the guest viewport
        surfaceView.setOnTouchListener { _, event ->
            handleMotionEvent(event)
            true
        }

        // Start FPS polling
        uiHandler.postDelayed(fpsPoller, 2000)

        // Verify or unpack rootfs in background thread
        backgroundExecutor.execute {
            extractRootfsIfNeeded()
        }
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        handleMotionEvent(event)
        return true
    }

    private fun handleMotionEvent(event: MotionEvent) {
        val actionMasked = event.actionMasked
        val pointerIndex = event.actionIndex
        val timestamp = event.eventTime

        when (actionMasked) {
            MotionEvent.ACTION_DOWN,
            MotionEvent.ACTION_UP,
            MotionEvent.ACTION_POINTER_DOWN,
            MotionEvent.ACTION_POINTER_UP,
            MotionEvent.ACTION_CANCEL -> {
                val pointerId = event.getPointerId(pointerIndex)
                val x = event.getX(pointerIndex)
                val y = event.getY(pointerIndex)
                val pressure = event.getPressure(pointerIndex)
                sendTouchEvent(actionMasked, pointerId, x, y, pressure, timestamp)
            }

            MotionEvent.ACTION_MOVE -> {
                val pointerCount = event.pointerCount
                for (i in 0 until pointerCount) {
                    val pointerId = event.getPointerId(i)
                    val x = event.getX(i)
                    val y = event.getY(i)
                    val pressure = event.getPressure(i)
                    sendTouchEvent(actionMasked, pointerId, x, y, pressure, timestamp)
                }
            }
        }
    }

    private fun applyImmersiveFullscreen() {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.R) {
            window.insetsController?.let { controller ->
                controller.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                controller.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
            )
        }
    }

    private fun initViews() {
        surfaceView = findViewById(R.id.guestSurfaceView)
        statusIndicator = findViewById(R.id.statusIndicator)
        textRuntimeState = findViewById(R.id.textRuntimeState)
        textDisplayStats = findViewById(R.id.textDisplayStats)
        textRootfsState = findViewById(R.id.textRootfsState)
        extractionProgress = findViewById(R.id.extractionProgress)
    }

    // ========================================================================
    // SurfaceHolder.Callback Implementation
    // ========================================================================

    override fun surfaceCreated(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceCreated: Passing Surface to native C++ engine")
        nativeOnSurfaceCreated(holder.surface)
        isSurfaceReady.set(true)

        runOnUiThread {
            statusIndicator.setBackgroundColor(getColor(R.color.container_status_amber))
            textRuntimeState.text = "ENGINE: SURFACE READY | AWAITING BOOT"
        }

        checkAndBootContainer()
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        Log.i(TAG, "surfaceChanged: Resolution=$width x $height, Format=$format")
        nativeOnSurfaceChanged(width, height)

        runOnUiThread {
            textDisplayStats.text = "RES: ${width}x${height} (RGBA_8888)"
        }
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        Log.i(TAG, "surfaceDestroyed: Releasing native ANativeWindow reference")
        isSurfaceReady.set(false)
        nativeOnSurfaceDestroyed()

        runOnUiThread {
            statusIndicator.setBackgroundColor(getColor(R.color.container_status_red))
            textRuntimeState.text = "ENGINE: DISPLAY SUSPENDED"
            textDisplayStats.text = "RES: DISCONNECTED"
        }
    }

    // ========================================================================
    // Boot Orchestrator (Tahap 8)
    // ========================================================================

    private fun checkAndBootContainer() {
        if (!isSurfaceReady.get() || !isRootfsReady.get()) {
            return
        }

        if (isContainerRunning() || !isStartingContainer.compareAndSet(false, true)) {
            return
        }

        backgroundExecutor.execute {
            val rootfsDir = File(filesDir, ROOTFS_DIR_NAME)
            val primaryAbi = android.os.Build.SUPPORTED_ABIS.firstOrNull() ?: "arm64-v8a"
            val arm64HookTarget = File(filesDir, "libzygote_shim.so")
            try {
                val apk = java.util.zip.ZipFile(File(applicationInfo.sourceDir))
                
                // 1. Extract libzygote_shim.so for host primary ABI
                val hookEntry = apk.getEntry("lib/$primaryAbi/libzygote_shim.so")
                    ?: apk.getEntry("lib/arm64-v8a/libzygote_shim.so")
                if (hookEntry != null) {
                    apk.getInputStream(hookEntry).use { input ->
                        arm64HookTarget.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    arm64HookTarget.setReadable(true, false)
                    arm64HookTarget.setExecutable(true, false)
                    Log.i(TAG, "Extracted $primaryAbi libzygote_shim.so: ${arm64HookTarget.length()} bytes")
                }

                // 2. Extract libc++_shared.so into guest system/lib64
                val libDir = File(rootfsDir, "system/lib64")
                libDir.mkdirs()
                val libcTarget = File(libDir, "libc++_shared.so")
                val libcEntry = apk.getEntry("lib/arm64-v8a/libc++_shared.so")
                if (libcEntry != null && (!libcTarget.exists() || libcTarget.length() == 0L)) {
                    apk.getInputStream(libcEntry).use { input ->
                        libcTarget.outputStream().use { output ->
                            input.copyTo(output)
                        }
                    }
                    libcTarget.setReadable(true, false)
                    libcTarget.setExecutable(true, false)
                    Log.i(TAG, "Provisioned guest libc++_shared.so (${libcTarget.length()} bytes)")
                }
                apk.close()
            } catch (e: Exception) {
                Log.w(TAG, "Failed extracting arm64 native libs: ${e.message}")
            }

            val primaryHookFile = File(applicationInfo.nativeLibraryDir, "libzygote_shim.so")
            val hookSoPath = if (arm64HookTarget.exists() && arm64HookTarget.length() > 0L) {
                arm64HookTarget.absolutePath
            } else if (primaryHookFile.exists()) {
                primaryHookFile.absolutePath
            } else {
                ""
            }

            Log.i(TAG, "Initiating Boot Orchestration on rootfs: ${rootfsDir.absolutePath}")
            Log.i(TAG, "Hook library path: $hookSoPath")

            runOnUiThread {
                statusIndicator.setBackgroundColor(getColor(R.color.container_status_amber))
                textRuntimeState.text = "ENGINE: BOOTING AOSP ZYGOTE..."
            }

            val zygotePid = startContainer(rootfsDir.absolutePath, hookSoPath)

            runOnUiThread {
                if (zygotePid > 0) {
                    statusIndicator.setBackgroundColor(getColor(R.color.container_status_green))
                    textRuntimeState.text = "ENGINE: RUNNING | ZYGOTE PID: $zygotePid | ARM64"
                    textRootfsState.text = "ROOTFS: ACTIVE (AOSP 13 / TQ3A)"
                    Log.i(TAG, "Container runtime successfully started. Zygote PID=$zygotePid")
                } else {
                    statusIndicator.setBackgroundColor(getColor(R.color.container_status_red))
                    textRuntimeState.text = "ENGINE: BOOT FAILED (ERROR: $zygotePid)"
                    Log.e(TAG, "Container runtime boot failed with return code: $zygotePid")
                    isStartingContainer.set(false)
                }
            }

            // Launch display bridge in guest after boot
            if (zygotePid > 0) {
                launchDisplayBridge()
            }
        }
    }

    // ========================================================================
    // Rootfs Unpack & Permission Provisioning
    // ========================================================================

    private fun extractRootfsIfNeeded() {
        val rootfsDir = File(filesDir, ROOTFS_DIR_NAME)
        val testBinary = File(rootfsDir, "system/bin/app_process64")

        if (testBinary.exists() && testBinary.length() > 0) {
            Log.i(TAG, "Rootfs already present at: ${rootfsDir.absolutePath}")
            isRootfsReady.set(true)
            runOnUiThread {
                textRootfsState.text = "ROOTFS: READY (${rootfsDir.name}/system/bin/app_process64)"
                extractionProgress.visibility = View.GONE
            }
            checkAndBootContainer()
            return
        }

        runOnUiThread {
            textRootfsState.text = "ROOTFS: EXTRACTING SYSTEM ARTIFACTS..."
            extractionProgress.visibility = View.VISIBLE
            extractionProgress.isIndeterminate = true
        }

        var inputStream: InputStream? = null
        var apkZip: java.util.zip.ZipFile? = null
        try {
            // Direct streaming from APK container to bypass AssetManager memory constraints
            try {
                val apkFile = File(applicationInfo.sourceDir)
                if (apkFile.exists()) {
                    val zip = java.util.zip.ZipFile(apkFile)
                    val entry = zip.getEntry("assets/$ROOTFS_ASSET_NAME")
                    if (entry != null) {
                        Log.i(TAG, "Streaming rootfs directly from APK container: ${entry.name} (${entry.size} bytes)")
                        apkZip = zip
                        inputStream = zip.getInputStream(entry)
                    }
                }
            } catch (e: Exception) {
                Log.w(TAG, "Direct APK stream unavailable, falling back to assets.open: ${e.message}")
            }

            if (inputStream == null) {
                val assetList = assets.list("") ?: emptyArray()
                if (assetList.contains(ROOTFS_ASSET_NAME)) {
                    Log.i(TAG, "Unpacking rootfs from assets/$ROOTFS_ASSET_NAME")
                    inputStream = assets.open(ROOTFS_ASSET_NAME)
                } else {
                    val stagingArchive = File(filesDir, ROOTFS_ASSET_NAME)
                    if (stagingArchive.exists()) {
                        Log.i(TAG, "Unpacking rootfs from staged file: ${stagingArchive.absolutePath}")
                        inputStream = stagingArchive.inputStream()
                    } else {
                        Log.w(TAG, "No rootfs archive found in assets or filesDir.")
                        runOnUiThread {
                            textRootfsState.text = "ROOTFS: AWAITING IMAGE DROP ($ROOTFS_ASSET_NAME)"
                            extractionProgress.visibility = View.GONE
                        }
                        createEssentialDirectories(rootfsDir)
                        return
                    }
                }
            }

            unpackZipArchive(inputStream, rootfsDir)
            createEssentialDirectories(rootfsDir)

            isRootfsReady.set(true)
            runOnUiThread {
                textRootfsState.text = "ROOTFS: READY (${rootfsDir.absolutePath})"
                extractionProgress.visibility = View.GONE
            }
            Log.i(TAG, "Rootfs successfully provisioned.")
            checkAndBootContainer()

        } catch (e: Exception) {
            Log.e(TAG, "Error unpacking rootfs archive", e)
            runOnUiThread {
                textRootfsState.text = "ROOTFS: EXTRACTION FAILED (${e.localizedMessage})"
                extractionProgress.visibility = View.GONE
            }
        } finally {
            try {
                inputStream?.close()
            } catch (_: Exception) {}
            try {
                apkZip?.close()
            } catch (_: Exception) {}
        }
    }

    private fun unpackZipArchive(rawInput: InputStream, targetDir: File) {
        targetDir.mkdirs()
        val canonicalDest = targetDir.canonicalPath
        val buffer = ByteArray(65536)

        ZipInputStream(BufferedInputStream(rawInput)).use { zipIn ->
            var entry: ZipEntry? = zipIn.nextEntry
            var extractedCount = 0

            while (entry != null) {
                val outFile = File(targetDir, entry.name)
                val canonicalOut = outFile.canonicalPath

                if (!canonicalOut.startsWith(canonicalDest)) {
                    throw SecurityException("Zip Slip path violation: ${entry.name}")
                }

                if (entry.isDirectory) {
                    outFile.mkdirs()
                } else {
                    outFile.parentFile?.mkdirs()
                    BufferedOutputStream(FileOutputStream(outFile)).use { outStream ->
                        var bytesRead: Int
                        while (zipIn.read(buffer).also { bytesRead = it } != -1) {
                            outStream.write(buffer, 0, bytesRead)
                        }
                    }

                    if (entry.name.contains("/bin/") || entry.name.endsWith(".so")) {
                        outFile.setExecutable(true, false)
                        outFile.setReadable(true, false)
                    }
                }

                extractedCount++
                if (extractedCount % 200 == 0) {
                    Log.d(TAG, "Extracted $extractedCount files...")
                }
                zipIn.closeEntry()
                entry = zipIn.nextEntry
            }
            Log.i(TAG, "Completed zip unpacking. Total entries: $extractedCount")
        }
    }

    private fun createEssentialDirectories(rootfsDir: File) {
        val dirs = listOf("data", "dev", "proc", "sys", "apex", "vendor", "system/bin", "system/lib64")
        for (dir in dirs) {
            val d = File(rootfsDir, dir)
            if (!d.exists()) {
                d.mkdirs()
            }
            d.setReadable(true, false)
            d.setWritable(true, false)
            d.setExecutable(true, false)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        uiHandler.removeCallbacks(fpsPoller)
        nativeStopFrameStreamer()
        stopContainer()
        backgroundExecutor.shutdown()
    }

    // ========================================================================
    // Display Bridge Launcher
    // ========================================================================

    private fun launchDisplayBridge() {
        backgroundExecutor.execute {
            try {
                // Copy display_bridge.sh to /data/local/tmp via adb (or directly)
                val bridgeScript = File(filesDir, "display_bridge.sh")
                assets.list("")?.contains("display_bridge.sh").let { hasAsset ->
                    if (hasAsset == true) {
                        assets.open("display_bridge.sh").use { src ->
                            bridgeScript.outputStream().use { dst -> src.copyTo(dst) }
                        }
                        bridgeScript.setExecutable(true, false)
                        Log.i(TAG, "display_bridge.sh extracted to ${bridgeScript.absolutePath}")
                    }
                }

                // Wait for sys.boot_completed in guest
                var booted = false
                repeat(60) {
                    if (!booted) {
                        val proc = Runtime.getRuntime().exec(arrayOf("getprop", "sys.boot_completed"))
                        val result = proc.inputStream.bufferedReader().readLine()?.trim()
                        if (result == "1") {
                            booted = true
                            Log.i(TAG, "sys.boot_completed=1 detected, launching display bridge")
                        } else {
                            Thread.sleep(2000)
                        }
                    }
                }

                if (booted && bridgeScript.exists()) {
                    val pb = ProcessBuilder("/system/bin/sh", bridgeScript.absolutePath)
                    pb.redirectErrorStream(true)
                    pb.start()
                    Log.i(TAG, "Display bridge launched successfully")
                    runOnUiThread {
                        textRuntimeState.text = "ENGINE: RUNNING | DISPLAY BRIDGE ACTIVE"
                    }
                } else {
                    // Fallback: manual start via adb
                    Log.w(TAG, "Display bridge not auto-started. Run manually: adb shell sh /data/local/tmp/display_bridge.sh")
                }
                } catch (e: Exception) {
                Log.e(TAG, "Failed to launch display bridge", e)
            }
        }
    }
}
