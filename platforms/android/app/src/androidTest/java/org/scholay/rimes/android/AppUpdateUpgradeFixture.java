package org.scholay.rimes.android;

import android.app.Instrumentation;
import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.ParcelFileDescriptor;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.concurrent.atomic.AtomicReference;

/** Test APK only: stages an actual newer debug APK before an external host drives installation. */
final class AppUpdateUpgradeFixture {
    static String prepare(Instrumentation instrumentation,String path) throws Exception {
        Context context=instrumentation.getTargetContext();
        if (!BuildConfig.DEBUG || !context.getPackageName().equals("org.scholay.rimes.android.debug"))
            throw new AssertionError("Upgrade fixture is limited to the disposable debug app");
        if (path==null || !path.matches("/data/local/tmp/[A-Za-z0-9_.-]+\\.apk")) throw new AssertionError("Expected a test APK in /data/local/tmp");
        AtomicReference<AppUpdateManager> updater=new AtomicReference<>();
        instrumentation.runOnMainSync(() -> updater.set(AppUpdateManager.get(context)));
        File staged=updater.get().packageFile();
        if (!staged.getParentFile().isDirectory() && !staged.getParentFile().mkdirs()) throw new AssertionError("Cannot prepare updater directory");
        try (ParcelFileDescriptor source=instrumentation.getUiAutomation().executeShellCommand("cat "+path);
             InputStream input=new ParcelFileDescriptor.AutoCloseInputStream(source); FileOutputStream output=new FileOutputStream(staged)) {
            byte[] buffer=new byte[65536]; int n; long count=0;
            while ((n=input.read(buffer))!=-1) { count+=n; if (count>512L*1024*1024) throw new AssertionError("Oversized fixture"); output.write(buffer,0,n); }
            output.getFD().sync();
        }
        PackageInfo target=context.getPackageManager().getPackageArchiveInfo(staged.getPath(),0);
        if (target==null) throw new AssertionError("Cannot read staged APK");
        long previous=AppUpdateManager.versionCode(context.getPackageManager().getPackageInfo(context.getPackageName(),0));
        JSONObject asset=new JSONObject().put("version",target.versionName).put("build",AppUpdateManager.versionCode(target))
                .put("minimumOS",26).put("size",staged.length()).put("sha256",hash(staged))
                .put("url","https://github.com/scholay/xsime/releases/download/v"+target.versionName+"/RIMES-Android-"+target.versionName+".apk");
        updater.get().verifyPackage(staged,asset);
        new KeyboardSettings(context);
        if (!context.getSharedPreferences(KeyboardSettings.PREFERENCES_NAME,Context.MODE_PRIVATE).edit()
                .putInt(KeyboardSettings.KEY_PORTRAIT_HEIGHT,117).putBoolean(KeyboardSettings.KEY_WIDE_SPACE,true)
                .putBoolean(KeyboardSettings.KEY_LEARNING,false).commit()) throw new AssertionError("Could not seed settings");
        File marker=new File(context.getFilesDir(),"update-fixture-token");
        File dictionary=new File(context.getNoBackupFilesDir(),"rime-user/.update-fixture-data");
        if (!dictionary.getParentFile().isDirectory() && !dictionary.getParentFile().mkdirs()) throw new AssertionError("Cannot seed dictionary fixture");
        Files.write(marker.toPath(),"XSIME upgrade retention fixture\n".getBytes(StandardCharsets.UTF_8));
        Files.write(dictionary.toPath(),"synthetic dictionary retention bytes\n".getBytes(StandardCharsets.UTF_8));
        JSONObject retained=new JSONObject().put("files/update-fixture-token",hash(marker))
                .put("no_backup/rime-user/.update-fixture-data",hash(dictionary));
        JSONObject state=new JSONObject().put("previousCode",previous).put("expectedCode",AppUpdateManager.versionCode(target))
                .put("expectedVersion",target.versionName).put("retained",retained);
        Files.write(new File(context.getFilesDir(),"update-fixture-state.json").toPath(),state.toString().getBytes(StandardCharsets.UTF_8));
        if (!context.getSharedPreferences(AppUpdateManager.PREFS,Context.MODE_PRIVATE).edit().clear()
                .putString("debugUpdateFixture",asset.toString()).commit()) throw new AssertionError("Could not seed fixture");
        return "PASS update upgrade fixture prepared; previousCode="+previous+" expectedCode="+AppUpdateManager.versionCode(target)+"\n";
    }
    private static String hash(File file) throws Exception {
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try (InputStream input=new FileInputStream(file)) { byte[] buffer=new byte[65536]; int n; while ((n=input.read(buffer))!=-1) digest.update(buffer,0,n); }
        return AppUpdateManager.hex(digest.digest());
    }
}
