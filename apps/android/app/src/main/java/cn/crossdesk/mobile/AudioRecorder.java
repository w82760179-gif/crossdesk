package cn.crossdesk.mobile;

import android.annotation.SuppressLint;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.media.audiofx.AcousticEchoCanceler;
import android.media.audiofx.NoiseSuppressor;
import android.os.Process;
import java.util.function.Consumer;

/** Captures 48 kHz mono 16-bit PCM in exact 10 ms (960-byte) frames for MiniRTC Opus uplink. */
final class AudioRecorder implements AutoCloseable {
    private static final int SAMPLE_RATE = 48000;
    private static final int FRAME_BYTES = 960; // 480 samples * 2 bytes = 10 ms
    private final Consumer<byte[]> onFrame;
    private final Runnable onFailure;
    private volatile boolean closed;
    private volatile AudioRecord activeRecord;
    private final Thread thread = new Thread(this::record, "CrossDesk mic");

    AudioRecorder(Consumer<byte[]> onFrame, Runnable onFailure) {
        this.onFrame = onFrame;
        this.onFailure = onFailure;
        thread.start();
    }

    @SuppressLint("MissingPermission")
    private void record() {
        Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
        AudioRecord recorder = null;
        AcousticEchoCanceler aec = null;
        NoiseSuppressor ns = null;
        try {
            int minBuf = AudioRecord.getMinBufferSize(
                    SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            if (minBuf <= 0) throw new IllegalStateException("AudioRecord unsupported");
            int bufSize = Math.max(minBuf * 2, FRAME_BYTES * 8);
            recorder = new AudioRecord(
                    MediaRecorder.AudioSource.VOICE_COMMUNICATION,
                    SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT,
                    bufSize);
            if (recorder.getState() != AudioRecord.STATE_INITIALIZED) {
                throw new IllegalStateException("AudioRecord init failed");
            }
            activeRecord = recorder;
            int sessionId = recorder.getAudioSessionId();
            if (AcousticEchoCanceler.isAvailable()) {
                aec = AcousticEchoCanceler.create(sessionId);
                if (aec != null) aec.setEnabled(true);
            }
            if (NoiseSuppressor.isAvailable()) {
                ns = NoiseSuppressor.create(sessionId);
                if (ns != null) ns.setEnabled(true);
            }
            recorder.startRecording();
            byte[] frame = new byte[FRAME_BYTES];
            while (!closed) {
                int offset = 0;
                while (!closed && offset < FRAME_BYTES) {
                    int n = recorder.read(frame, offset, FRAME_BYTES - offset);
                    if (n <= 0) {
                        if (!closed) throw new IllegalStateException("AudioRecord read error: " + n);
                        break;
                    }
                    offset += n;
                }
                if (!closed && offset == FRAME_BYTES) {
                    onFrame.accept(frame);
                }
            }
        } catch (SecurityException | IllegalArgumentException | IllegalStateException error) {
            if (!closed && onFailure != null) onFailure.run();
        } finally {
            activeRecord = null;
            if (ns != null) { try { ns.release(); } catch (Exception ignored) { } }
            if (aec != null) { try { aec.release(); } catch (Exception ignored) { } }
            if (recorder != null) {
                try { recorder.stop(); } catch (Exception ignored) { }
                try { recorder.release(); } catch (Exception ignored) { }
            }
        }
    }

    @Override public void close() {
        if (closed) return;
        closed = true;
        AudioRecord rec = activeRecord;
        if (rec != null) {
            try { rec.stop(); } catch (Exception ignored) { }
        }
        thread.interrupt();
        if (Thread.currentThread() != thread) {
            try { thread.join(350); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); }
        }
    }
}
