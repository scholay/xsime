package org.scholay.rimes.android;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.SystemClock;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class AppUpdateLifecycleContract {
    static void run(Instrumentation instrumentation) throws Exception {
        AtomicReference<AppUpdateManager> manager=new AtomicReference<>();
        instrumentation.runOnMainSync(() -> manager.set(AppUpdateManager.get(instrumentation.getTargetContext())));
        Activity first=instrumentation.startActivitySync(new Intent(instrumentation.getTargetContext(),AppUpdateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        CountDownLatch running=new CountDownLatch(1),release=new CountDownLatch(1);
        AtomicInteger parallel=new AtomicInteger(),peak=new AtomicInteger();
        AtomicReference<AppUpdateManager.State> latest=new AtomicReference<>();
        AppUpdateManager.Observer observer=latest::set;
        instrumentation.runOnMainSync(() -> {
            manager.get().observe(observer);
            manager.get().start("Lifecycle fixture",true,operation -> {
                peak.accumulateAndGet(parallel.incrementAndGet(),Math::max); running.countDown();
                try { while (!release.await(20,TimeUnit.MILLISECONDS)) operation.check(); operation.check(); return "Lifecycle fixture complete"; }
                finally { parallel.decrementAndGet(); }
            });
        });
        if (!running.await(5,TimeUnit.SECONDS)) throw new AssertionError("Updater worker did not start");
        try {
            // An Activity replacement must leave the same operation and progress owner alive.
            instrumentation.runOnMainSync(first::finish); instrumentation.waitForIdleSync();
            Activity second=instrumentation.startActivitySync(new Intent(instrumentation.getTargetContext(),AppUpdateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            try {
                instrumentation.waitForIdleSync();
                if (!latest.get().busy || !latest.get().canCancel) throw new AssertionError("Activity recreation lost the active transfer");
                instrumentation.runOnMainSync(() -> {
                    manager.get().start("Must not overlap",true,operation -> { peak.accumulateAndGet(parallel.incrementAndGet(),Math::max); parallel.decrementAndGet(); return "overlap"; });
                    manager.get().cancel();
                });
                awaitIdle(latest);
                if (!latest.get().message.contains("Cancelled") || peak.get()!=1) throw new AssertionError("Cancellation/serialization failed");
                instrumentation.runOnMainSync(() -> manager.get().start("Retry",true,operation -> "Retry complete"));
                awaitIdle(latest);
                if (!latest.get().message.equals("Retry complete")) throw new AssertionError("Retry did not complete after cancellation");
            } finally { instrumentation.runOnMainSync(second::finish); }
        } finally {
            release.countDown();
            instrumentation.runOnMainSync(() -> { manager.get().cancel(); manager.get().removeObserver(observer); first.finish(); });
        }
    }
    private static void awaitIdle(AtomicReference<AppUpdateManager.State> latest) {
        long deadline=SystemClock.uptimeMillis()+5000;
        while (latest.get().busy && SystemClock.uptimeMillis()<deadline) SystemClock.sleep(20);
        if (latest.get().busy) throw new AssertionError("Updater never became idle");
    }
}
