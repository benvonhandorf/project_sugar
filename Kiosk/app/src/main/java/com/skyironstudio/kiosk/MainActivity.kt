package com.skyironstudio.kiosk

import android.os.Bundle
import android.util.Log
import android.view.ViewGroup
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.viewinterop.AndroidView
import androidx.media3.common.MediaItem
import androidx.media3.common.PlaybackException
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.ui.PlayerView
import com.skyironstudio.kiosk.ui.theme.KioskTheme
import java.net.URL

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        setContent {
            KioskTheme {
                // A surface container using the 'background' color from the theme
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background
                ) {
                    CameraStreams()
                }
            }
        }
    }
}

@Immutable
@Stable
data class CameraState(val exoPlayer: ExoPlayer, val streams: List<String>) {

}

@Composable
fun CameraStreams() {
    val context = LocalContext.current

    val streams = listOf("rtsp://picam01:8554/camera0", "rtsp://razorcrest:8554/camera0")
    val mediaItems = streams.map { MediaItem.fromUri(it) }

    val exoPlayer = remember {
        ExoPlayer.Builder(context)
            .build().apply {
                this.prepare()
                this.playWhenReady = true
                this.setMediaItems(mediaItems)
            }
    }

    val state = CameraState(exoPlayer, streams)

    CameraStream(exoPlayer, state.streams[0])
}

@Composable
fun CameraStream(exoPlayer: ExoPlayer, streamURL: String, modifier: Modifier = Modifier) {
    val context = LocalContext.current

    val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) {
            super.onEvents(player, events)
            for (i in 0 until events.size()) {
                val event = events.get(i)
                Log.i("ExoPlayerEvents", "$i - $event")
            }
        }

        override fun onPlayerError(error: PlaybackException) {
            super.onPlayerError(error)

            Log.i("ExoPlayerError", "$error")
        }

        override fun onPlayerErrorChanged(error: PlaybackException?) {
            super.onPlayerErrorChanged(error)

            Log.i("ExoPlayerErrorCng", "$error")
        }

        override fun onPlaybackStateChanged(playbackState: Int) {
            super.onPlaybackStateChanged(playbackState)

            Log.i("ExoPlayerPlaybackCng", "$playbackState")
        }

        override fun onIsPlayingChanged(isPlaying: Boolean) {
            super.onIsPlayingChanged(isPlaying)

            Log.i("ExoPlayerPlayCng", isPlaying.toString())
        }
    }

    Box(
        modifier = modifier
            .testTag("VideoPlayerParent")
    ) {
        DisposableEffect(key1 = true) {
            exoPlayer.addListener(listener)

            onDispose {

            }
        }
        AndroidView(
            modifier = modifier
                .testTag("VideoPlayer"),
            factory = {
                PlayerView(context).apply {
                    player = exoPlayer
                    useController = false
                    layoutParams = FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT
                    )
                }
            })
    }
}

@Composable
fun Greeting(name: String, modifier: Modifier = Modifier) {
    Text(
        text = "Hello $name!",
        modifier = modifier
    )
}

@Preview(showBackground = true)
@Composable
fun GreetingPreview() {
    KioskTheme {
        Greeting("Android")
    }
}