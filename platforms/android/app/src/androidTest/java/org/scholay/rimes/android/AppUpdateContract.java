package org.scholay.rimes.android;
import org.json.JSONObject;
import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.accessibility.AccessibilityNodeInfo;
import android.widget.Button;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.concurrent.atomic.AtomicBoolean;
final class AppUpdateContract {
    static void run(Instrumentation instrumentation) throws Exception {
        validateManifest();
        UpdateManifestContract.run(instrumentation);
        systemConfirmation(instrumentation);
        AppUpdateLifecycleContract.run(instrumentation);
    }

    static void validateManifest() throws Exception {
        JSONObject asset = new JSONObject().put("version", "1.2.3").put("url", "https://github.com/scholay/xsime/releases/download/v1.2.3/RIMES-Android-1.2.3.apk").put("sha256", new String(new char[64]).replace('\0', 'a')).put("size", 100);
        AppUpdateActivity.validateAsset(asset);
        for (String url : new String[]{"http://github.com/scholay/xsime/releases/download/v1.2.3/RIMES-Android-1.2.3.apk", "https://github.com/attacker/xsime/releases/download/v1.2.3/RIMES-Android-1.2.3.apk", "https://github.com/scholay/xsime/releases/download/v1.2.2/RIMES-Android-1.2.2.apk"}) {
            JSONObject wrong = new JSONObject(asset.toString()).put("url", url);
            boolean rejected = false; try { AppUpdateActivity.validateAsset(wrong); } catch (Exception expected) { rejected = true; }
            if (!rejected) throw new AssertionError("Untrusted update accepted");
        }
        for (long size : new long[]{0, -1, 1024L * 1024 * 1024}) {
            boolean rejected = false; try { AppUpdateActivity.validateAsset(new JSONObject(asset.toString()).put("size", size)); } catch (Exception expected) { rejected = true; }
            if (!rejected) throw new AssertionError("Unsafe package size accepted");
        }
    }

    private static boolean controlsDisabled(View view) {
        if (view instanceof Button) {
            String label = ((Button) view).getText().toString();
            if ((label.contains("Download") || label.contains("Install")) && view.isEnabled()) return false;
        }
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) if (!controlsDisabled(group.getChildAt(i))) return false;
        }
        return true;
    }
    private static void awaitText(Instrumentation instrumentation, Activity activity, String expected) throws Exception {
        long deadline = SystemClock.uptimeMillis() + 15000;
        while (SystemClock.uptimeMillis() < deadline) {
            // Installation crosses an app boundary. Assert what the user can
            // actually see, not a retained Activity's potentially hidden view.
            AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
            if (root != null && activity.getPackageName().contentEquals(root.getPackageName() == null ? "" : root.getPackageName())) {
                for (AccessibilityNodeInfo node : root.findAccessibilityNodeInfosByText(expected)) {
                    if (node.isVisibleToUser() && node.getText() != null && node.getText().toString().contains(expected)) return;
                }
            }
            SystemClock.sleep(100);
        }
        throw new AssertionError("Missing updater state: " + expected + "; activity destroyed=" + activity.isDestroyed()
                + "; pendingSession=" + activity.getSharedPreferences("application-update", Context.MODE_PRIVATE).getInt("pendingSession", -1)
                + "; failureCode=" + activity.getSharedPreferences("application-update", Context.MODE_PRIVATE).getInt("failureCode", -1));
    }
    private static void systemConfirmation(Instrumentation instrumentation) throws Exception {
        Context context = instrumentation.getTargetContext();
        if (!BuildConfig.DEBUG) throw new AssertionError("The installer fixture requires a disposable debug app");
        context.getSharedPreferences("application-update", Context.MODE_PRIVATE).edit().clear().commit();
        Activity activity = instrumentation.startActivitySync(new Intent(context, AppUpdateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        int sessionId = -1;
        try {
            instrumentation.waitForIdleSync();
            awaitText(instrumentation, activity, "Current: " + BuildConfig.VERSION_NAME);
            AtomicBoolean disabled = new AtomicBoolean();
            instrumentation.runOnMainSync(() -> disabled.set(controlsDisabled(activity.getWindow().getDecorView())));
            if (!disabled.get()) throw new AssertionError("Unverified packages must not be downloadable/installable");
            JSONObject pending = new JSONObject().put("version", "99.0.0");
            context.getSharedPreferences("application-update", Context.MODE_PRIVATE).edit()
                    .putString("candidate", pending.toString())
                    .putLong("pendingCode", (long) BuildConfig.VERSION_CODE + 1).commit();
            // Only this disposable CI app receives the install app-op. Exercise
            // Android's real confirmation callback, then cancel before replacement.
            try (android.os.ParcelFileDescriptor command = instrumentation.getUiAutomation().executeShellCommand(
                    "appops set " + context.getPackageName() + " REQUEST_INSTALL_PACKAGES allow");
                 InputStream output = new android.os.ParcelFileDescriptor.AutoCloseInputStream(command)) {
                byte[] drain = new byte[1024]; while (output.read(drain) != -1) { }
            }
            PackageInstaller.SessionParams params = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(context.getPackageName());
            if (android.os.Build.VERSION.SDK_INT >= 31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED);
            sessionId = installer.createSession(params);
            context.getSharedPreferences("application-update", Context.MODE_PRIVATE).edit().putInt("pendingSession", sessionId).commit();
            try (PackageInstaller.Session session = installer.openSession(sessionId);
                 InputStream input = new FileInputStream(context.getApplicationInfo().sourceDir)) {
                try (OutputStream output = session.openWrite("base.apk", 0, -1)) {
                    byte[] bytes = new byte[65536]; int n;
                    while ((n = input.read(bytes)) != -1) output.write(bytes, 0, n);
                    session.fsync(output);
                }
                session.commit(AppUpdateActivity.installationCallback(activity, sessionId).getIntentSender());
            }
            boolean cancelled = false;
            long deadline = SystemClock.uptimeMillis() + 30000;
            while (!cancelled && SystemClock.uptimeMillis() < deadline) {
                AccessibilityNodeInfo root = instrumentation.getUiAutomation().getRootInActiveWindow();
                if (root != null && root.getPackageName() != null &&
                        root.getPackageName().toString().contains("packageinstaller")) {
                    for (AccessibilityNodeInfo node : root.findAccessibilityNodeInfosByText("Cancel")) {
                        if (node.isClickable() && node.performAction(AccessibilityNodeInfo.ACTION_CLICK)) { cancelled = true; break; }
                    }
                }
                if (!cancelled) SystemClock.sleep(100);
            }
            if (!cancelled) throw new AssertionError("Android's install confirmation did not appear");
            awaitText(instrumentation, activity, "Installation did not finish");
            instrumentation.runOnMainSync(activity::finish);
            instrumentation.waitForIdleSync();
            Activity retryPage = instrumentation.startActivitySync(new Intent(context, AppUpdateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            try { awaitText(instrumentation, retryPage, "Installation did not finish"); }
            finally { instrumentation.runOnMainSync(retryPage::finish); }
            instrumentation.waitForIdleSync();
            // Completion is based on the running build, independently of a
            // successful callback or a browser/installer having been opened.
            context.getSharedPreferences("application-update", Context.MODE_PRIVATE).edit()
                    .putLong("pendingCode", BuildConfig.VERSION_CODE).commit();
            Activity reopened = instrumentation.startActivitySync(new Intent(context, AppUpdateActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            try { awaitText(instrumentation, reopened, "Updated to " + BuildConfig.VERSION_NAME); }
            finally { instrumentation.runOnMainSync(reopened::finish); }
        } finally {
            if (sessionId >= 0) try { installer.abandonSession(sessionId); } catch (Exception ignored) { }
            instrumentation.runOnMainSync(activity::finish);
            context.getSharedPreferences("application-update", Context.MODE_PRIVATE).edit().clear().commit();
        }
    }
}
