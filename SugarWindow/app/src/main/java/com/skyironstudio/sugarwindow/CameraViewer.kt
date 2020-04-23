package com.skyironstudio.sugarwindow

import androidx.appcompat.app.AppCompatActivity
import android.os.Bundle
import com.longdo.mjpegviewer.MjpegView
import kotlinx.android.synthetic.main.activity_camera_viewer.*

/**
 * Skeleton of an Android Things activity.
 *
 * Android Things peripheral APIs are accessible through the PeripheralManager
 * For example, the snippet below will open a GPIO pin and set it to HIGH:
 *
 * val manager = PeripheralManager.getInstance()
 * val gpio = manager.openGpio("BCM6").apply {
 *     setDirection(Gpio.DIRECTION_OUT_INITIALLY_LOW)
 * }
 * gpio.value = true
 *
 * You can find additional examples on GitHub: https://github.com/androidthings
 */
class CameraViewer : AppCompatActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_camera_viewer)

        mjpegView.setMode(MjpegView.MODE_BEST_FIT);
        mjpegView.setAdjustHeight(true);
        mjpegView.setUrl("http://192.168.10.178:8081");
        mjpegView.startStream();
    }
}
