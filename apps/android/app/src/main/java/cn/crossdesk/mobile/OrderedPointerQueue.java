package cn.crossdesk.mobile;

import java.util.concurrent.Executor;

/** Keep the newest position between ordered RTC commands. Buttons and keys are barriers. */
final class OrderedPointerQueue {
    interface Sender { void move(float x, float y); }
    interface TouchSender { void touch(int[] ids, int[] phases, float[] xs, float[] ys, int count); }
    private final Executor executor;
    private final Sender sender;
    private final TouchSender touchSender;
    private Move pending;
    private TouchMove pendingTouch;

    OrderedPointerQueue(Executor executor, Sender sender) {
        this(executor, sender, null);
    }

    OrderedPointerQueue(Executor executor, Sender sender, TouchSender touchSender) {
        this.executor = executor;
        this.sender = sender;
        this.touchSender = touchSender;
    }

    synchronized void move(float x, float y) {
        pendingTouch = null;
        if (pending == null) {
            pending = new Move(x, y);
            executor.execute(pending);
        } else {
            pending.x = x;
            pending.y = y;
        }
    }

    synchronized void touchMove(int[] ids, int[] phases, float[] xs, float[] ys, int count) {
        if (touchSender == null) return;
        pending = null;
        if (pendingTouch == null) {
            pendingTouch = new TouchMove(ids, phases, xs, ys, count);
            executor.execute(pendingTouch);
        } else {
            pendingTouch.ids = ids;
            pendingTouch.phases = phases;
            pendingTouch.xs = xs;
            pendingTouch.ys = ys;
            pendingTouch.count = count;
        }
    }

    synchronized void execute(Runnable command) {
        pending = null;
        pendingTouch = null;
        executor.execute(command);
    }

    private final class Move implements Runnable {
        float x, y;
        Move(float x, float y) { this.x = x; this.y = y; }
        public void run() {
            float sendX, sendY;
            synchronized (OrderedPointerQueue.this) {
                if (pending == this) pending = null;
                sendX = x;
                sendY = y;
            }
            sender.move(sendX, sendY);
        }
    }

    private final class TouchMove implements Runnable {
        int[] ids, phases;
        float[] xs, ys;
        int count;
        TouchMove(int[] ids, int[] phases, float[] xs, float[] ys, int count) {
            this.ids = ids; this.phases = phases; this.xs = xs; this.ys = ys; this.count = count;
        }
        public void run() {
            int[] sendIds, sendPhases;
            float[] sendXs, sendYs;
            int sendCount;
            synchronized (OrderedPointerQueue.this) {
                if (pendingTouch == this) pendingTouch = null;
                sendIds = ids; sendPhases = phases; sendXs = xs; sendYs = ys; sendCount = count;
            }
            touchSender.touch(sendIds, sendPhases, sendXs, sendYs, sendCount);
        }
    }
}
