package cn.crossdesk.mobile;

import java.util.concurrent.Executor;

/** Keep the newest position between ordered RTC commands. Buttons and keys are barriers. */
final class OrderedPointerQueue {
    interface Sender { void move(float x, float y); }
    private final Executor executor;
    private final Sender sender;
    private Move pending;

    OrderedPointerQueue(Executor executor, Sender sender) {
        this.executor = executor;
        this.sender = sender;
    }

    synchronized void move(float x, float y) {
        if (pending == null) {
            pending = new Move(x, y);
            executor.execute(pending);
        } else {
            pending.x = x;
            pending.y = y;
        }
    }

    synchronized void execute(Runnable command) {
        pending = null;
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
}
