package org.scholay.rimes.android;

import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.Handler;
import android.os.Looper;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** One application-owned queue. Activities observe it; rotating never cancels an update. */
final class AppUpdateManager {
    static final String PREFS = "application-update";
    private static final String FEED = "https://github.com/scholay/xsime/releases/latest/download/update-manifest.json";
    private static AppUpdateManager instance;
    static synchronized AppUpdateManager get(Context context) {
        if (instance == null) instance = new AppUpdateManager(context.getApplicationContext());
        return instance;
    }
    interface Observer { void changed(State state); }
    static final class State {
        final String message;
        final int progress;
        final boolean busy, canDownload, canInstall, canCancel;
        State(String message, int progress, boolean busy, boolean download, boolean install, boolean cancel) {
            this.message=message; this.progress=progress; this.busy=busy;
            canDownload=download; canInstall=install; canCancel=cancel;
        }
    }
    interface Work { String run(Operation operation) throws Exception; }
    static final class Operation {
        final boolean cancellable;
        volatile boolean cancelled;
        volatile HttpURLConnection connection;
        boolean installationTerminal;
        Operation(boolean cancellable) { this.cancellable=cancellable; }
        void check() { if (cancelled) throw new CancellationException("Cancelled"); }
        void cancel() { cancelled=true; HttpURLConnection current=connection; if (current!=null) try { current.disconnect(); } catch (RuntimeException ignored) { } }
        void connected(HttpURLConnection value) { connection=value; if (cancelled) { value.disconnect(); check(); } }
    }
    private final Context context;
    private final SharedPreferences preferences;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final Set<Observer> observers = new HashSet<>();
    private Operation active;
    private String message;
    private int progress;
    private JSONObject candidate;
    private final PackageInstaller.SessionCallback sessionEvents = new PackageInstaller.SessionCallback() {
        @Override public void onCreated(int id) { }
        @Override public void onBadgingChanged(int id) { }
        @Override public void onActiveChanged(int id, boolean value) { }
        @Override public void onProgressChanged(int id, float value) { }
        @Override public void onFinished(int id, boolean success) { finishInstallation(id, success, PackageInstaller.STATUS_FAILURE); }
    };
    private AppUpdateManager(Context context) {
        this.context=context; preferences=context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        message="当前版本 / Current: " + BuildConfig.VERSION_NAME;
        // Only startup cleans abandoned files. A live worker owns its unique temporary file.
        File directory=packageFile().getParentFile();
        File[] abandoned=directory.listFiles((dir,name) -> name.startsWith("release-") && name.endsWith(".partial"));
        if (abandoned!=null) for (File file:abandoned) file.delete();
        File legacy=new File(directory,"release.apk.partial"); if (legacy.exists()) legacy.delete();
        context.getPackageManager().getPackageInstaller().registerSessionCallback(sessionEvents,main);
        reloadCandidate();
        if (preferences.getBoolean("downloadInProgress",false)) {
            preferences.edit().remove("downloadInProgress").apply();
            message="下载已中断，请重试 / Download was interrupted. Try again.";
        }
        refreshInstallation();
    }
    File packageFile() { return new File(context.getFilesDir(),"updates/release.apk"); }
    void observe(Observer observer) { observers.add(observer); observer.changed(state()); }
    void removeObserver(Observer observer) { observers.remove(observer); }
    private State state() {
        boolean waiting=preferences.getInt("pendingSession",-1)>=0;
        boolean idle=active==null && !waiting;
        return new State(message,progress,!idle,idle && candidate!=null,
                idle && candidate!=null && packageFile().isFile(),
                active!=null ? active.cancellable && !active.cancelled : waiting);
    }
    private void changed() { State value=state(); for (Observer observer:new HashSet<>(observers)) observer.changed(value); }
    void notice(String value) { message=value; changed(); }
    void cancel() {
        Operation operation=active;
        if (operation!=null && operation.cancellable) {
            operation.cancel(); notice("正在取消 / Cancelling…");
        } else if (operation==null) {
            int id=preferences.getInt("pendingSession",-1);
            if (id>=0 && !confirmInstalled()) {
                try {
                    context.getPackageManager().getPackageInstaller().abandonSession(id);
                    finishInstallation(id,false,PackageInstaller.STATUS_FAILURE_ABORTED);
                } catch (RuntimeException error) {
                    notice("系统正在处理安装，请稍后查看 / Android is processing the installation. Check again shortly.");
                }
            }
        }
    }
    /** Package-private so instrumentation can exercise lifecycle/cancellation on the actual queue. */
    void start(String initial, boolean cancellable, Work work) {
        if (active!=null || preferences.getInt("pendingSession",-1)>=0) return;
        Operation operation=new Operation(cancellable); active=operation; progress=0; notice(initial);
        worker.execute(() -> {
            String result;
            try { result=work.run(operation); operation.check(); }
            catch (Exception error) {
                result=operation.cancelled ? "已取消，可重试 / Cancelled. Try again." : "更新失败，可重试 / Update failed: " + error.getMessage();
            }
            final String terminal=result;
            main.post(() -> {
                if (active!=operation) return;
                active=null; reloadCandidate();
                if (!operation.installationTerminal) message=terminal;
                else refreshInstallation();
                changed();
            });
        });
    }
    private void updateProgress(Operation operation,int percent) {
        main.post(() -> { if (active==operation && !operation.cancelled) { progress=percent; changed(); } });
    }
    private JSONObject readCandidate() throws Exception {
        String envelope=preferences.getString("candidateManifest","");
        if (!envelope.isEmpty()) {
            JSONObject payload=UpdateManifestVerifier.verify(envelope,System.currentTimeMillis()/1000L,UpdateTrust::publicKeyFor);
            JSONObject asset=assetFrom(payload);
            PackageInfo installed=requireVersionFloor(asset);
            if (asset.getInt("minimumOS")>android.os.Build.VERSION.SDK_INT ||
                    asset.getLong("build")<=versionCode(installed)) return null;
            return asset;
        }
        // Only a debuggable package can receive an instrumentation-owned local APK fixture.
        // Release packages never consult this preference and never trust an unsigned feed.
        if (BuildConfig.DEBUG) {
            String fixture=preferences.getString("debugUpdateFixture","");
            if (!fixture.isEmpty()) { JSONObject asset=new JSONObject(fixture); validateAsset(asset); return asset; }
        }
        return null;
    }
    private void reloadCandidate() {
        try { candidate=readCandidate(); }
        catch (Exception error) { candidate=null; }
    }
    private static JSONObject assetFrom(JSONObject payload) throws Exception {
        if (payload.getInt("schemaVersion")!=1) throw new Exception("Unsupported update format");
        JSONObject asset=new JSONObject(payload.getJSONObject("platforms").getJSONObject("android").toString());
        asset.put("version",payload.getString("version")); validateAsset(asset);
        if (UpdateManifestVerifier.integer(asset,"build")<=0 || UpdateManifestVerifier.integer(asset,"minimumOS")<26 ||
                UpdateManifestVerifier.integer(asset,"minimumOS")>Integer.MAX_VALUE)
            throw new Exception("Invalid Android update requirements");
        return asset;
    }
    /** Installed identity is a floor even before any signed feed has been saved. */
    PackageInfo requireVersionFloor(JSONObject asset) throws Exception {
        PackageInfo installed=context.getPackageManager().getPackageInfo(context.getPackageName(),0);
        String version=asset.getString("version"); long build=asset.getLong("build");
        UpdateManifestVerifier.requireNotOlder(version,build,installed.versionName,versionCode(installed));
        UpdateManifestVerifier.requireNotOlder(version,build,preferences.getString("trustedVersion",""),preferences.getLong("trustedBuild",0));
        return installed;
    }
    void check() {
        start("正在检查 / Checking…",true,operation -> {
            String envelope=new String(fetch(FEED,1024*1024,operation),StandardCharsets.UTF_8);
            JSONObject payload=UpdateManifestVerifier.verify(envelope,System.currentTimeMillis()/1000L,UpdateTrust::publicKeyFor);
            JSONObject asset=assetFrom(payload);
            String version=payload.getString("version"); long build=asset.getLong("build");
            PackageInfo installed=requireVersionFloor(asset);
            operation.check();
            boolean newer=build>versionCode(installed);
            // Update the rollback floor only after signature, schema and platform metadata validation.
            if (!preferences.edit().putString("trustedVersion",version).putLong("trustedBuild",build)
                    .putString("candidateManifest",newer?envelope:"").remove("candidate").remove("debugUpdateFixture")
                    .remove("failureCode").commit()) throw new Exception("Cannot save update metadata");
            if (asset.getInt("minimumOS")>android.os.Build.VERSION.SDK_INT) throw new Exception("This update requires a newer Android version");
            return newer ? "可更新至 / Available: " + version : "已是最新版本 / Up to date: " + BuildConfig.VERSION_NAME;
        });
    }
    static void validateAsset(JSONObject asset) throws Exception {
        String version=asset.getString("version");
        if (!version.matches("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)") || version.length()>64) throw new Exception("Invalid version");
        String expected="https://github.com/scholay/xsime/releases/download/v"+version+"/RIMES-Android-"+version+".apk";
        if (!expected.equals(asset.getString("url")) || !asset.getString("sha256").matches("[a-f0-9]{64}") ||
                UpdateManifestVerifier.integer(asset,"size")<=0 || UpdateManifestVerifier.integer(asset,"size")>512L*1024*1024) throw new Exception("Invalid package metadata");
        if (asset.has("build") && asset.getLong("build")<=0) throw new Exception("Invalid build number");
    }
    void download() {
        start("正在下载 / Downloading…",true,operation -> {
            JSONObject asset=readCandidate(); if (asset==null) throw new Exception("Check for updates again");
            File file=packageFile();
            if (!file.getParentFile().isDirectory() && !file.getParentFile().mkdirs()) throw new Exception("Cannot create update directory");
            File temporary=File.createTempFile("release-",".partial",file.getParentFile());
            preferences.edit().putBoolean("downloadInProgress",true).commit();
            try {
                HttpURLConnection connection=connect(asset.getString("url"),operation);
                long expected=asset.getLong("size"),total=0;
                MessageDigest digest=MessageDigest.getInstance("SHA-256");
                try (InputStream input=connection.getInputStream(); FileOutputStream output=new FileOutputStream(temporary)) {
                    long declared=connection.getContentLengthLong();
                    if (declared>=0 && declared!=expected) throw new Exception("Package length mismatch");
                    byte[] buffer=new byte[65536]; int count,last=-1;
                    while ((count=input.read(buffer))!=-1) {
                        operation.check(); total+=count;
                        if (total>expected) throw new Exception("Package exceeds expected size");
                        output.write(buffer,0,count); digest.update(buffer,0,count);
                        int percent=(int)(100*total/expected);
                        if (percent!=last) { last=percent; updateProgress(operation,percent); }
                    }
                    if (total!=expected || !hex(digest.digest()).equals(asset.getString("sha256"))) throw new Exception("Package integrity check failed");
                    output.getFD().sync();
                } finally { connection.disconnect(); operation.connection=null; }
                operation.check(); verifyPackage(temporary,asset); operation.check();
                if (!temporary.renameTo(file)) throw new Exception("Cannot stage package");
                return "下载并验证完成，点击安装 / Download verified. Ready to install.";
            } finally {
                if (temporary.exists()) temporary.delete();
                preferences.edit().remove("downloadInProgress").commit();
            }
        });
    }
    void verifyPackage(File file,JSONObject asset) throws Exception {
        PackageManager pm=context.getPackageManager();
        int flags=android.os.Build.VERSION.SDK_INT>=28?PackageManager.GET_SIGNING_CERTIFICATES:PackageManager.GET_SIGNATURES;
        PackageInfo next=pm.getPackageArchiveInfo(file.getPath(),flags);
        PackageInfo current=pm.getPackageInfo(context.getPackageName(),flags);
        if (next==null || !context.getPackageName().equals(next.packageName) || versionCode(next)!=asset.getLong("build") ||
                versionCode(next)<=versionCode(current) || !asset.getString("version").equals(next.versionName)) throw new Exception("Package identity/version mismatch");
        Signature[] expected=signatures(current),actual=signatures(next);
        if (expected==null || actual==null || expected.length!=actual.length || !Arrays.asList(expected).containsAll(Arrays.asList(actual))) throw new Exception("Package signing key mismatch");
        MessageDigest digest=MessageDigest.getInstance("SHA-256");
        try (InputStream input=new FileInputStream(file)) { byte[] buffer=new byte[65536]; int n; while ((n=input.read(buffer))!=-1) digest.update(buffer,0,n); }
        if (file.length()!=asset.getLong("size") || !hex(digest.digest()).equals(asset.getString("sha256"))) throw new Exception("Package changed; download again");
    }
    @SuppressWarnings("deprecation")
    static long versionCode(PackageInfo info) { return android.os.Build.VERSION.SDK_INT>=28?info.getLongVersionCode():info.versionCode; }
    @SuppressWarnings("deprecation")
    private static Signature[] signatures(PackageInfo info) {
        return android.os.Build.VERSION.SDK_INT>=28?(info.signingInfo==null?null:info.signingInfo.getApkContentsSigners()):info.signatures;
    }
    boolean matchesSession(int id) { return id>=0 && preferences.getInt("pendingSession",-1)==id; }
    private boolean confirmInstalled() {
        long expected=preferences.getLong("pendingCode",0);
        if (expected<=0) return false;
        try {
            PackageInfo installed=context.getPackageManager().getPackageInfo(context.getPackageName(),0);
            String version=preferences.getString("pendingVersion","");
            if (versionCode(installed)<expected || (versionCode(installed)==expected && !version.isEmpty() && !version.equals(installed.versionName))) return false;
            // Preserve trust floors and all other application data. Remove only updater staging/state.
            if (!preferences.edit().remove("pendingCode").remove("pendingVersion").remove("pendingSession")
                    .remove("failureCode").remove("candidateManifest").remove("candidate").remove("debugUpdateFixture").commit()) {
                notice("无法保存更新完成状态，请重试 / Could not save update completion. Try again."); return false;
            }
            candidate=null; if (packageFile().exists()) packageFile().delete();
            notice("更新完成 / Updated to "+installed.versionName); return true;
        } catch (PackageManager.NameNotFoundException error) { return false; }
    }
    void refreshInstallation() {
        if (active!=null) return;
        reloadCandidate();
        if (confirmInstalled()) return;
        int id=preferences.getInt("pendingSession",-1);
        if (id>=0) {
            PackageInstaller installer=context.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionInfo info=installer.getSessionInfo(id);
            // A process death while copying can leave an uncommitted session behind.
            // No worker is active here, so such a session cannot still be preparing.
            if (info==null || !info.isSealed()) {
                if (info!=null) try { installer.abandonSession(id); } catch (RuntimeException ignored) { }
                finishInstallation(id,false,PackageInstaller.STATUS_FAILURE); return;
            }
        }
        if (preferences.contains("failureCode")) notice("安装未完成，可重试 / Installation did not finish ("+preferences.getInt("failureCode",PackageInstaller.STATUS_FAILURE)+").");
        else if (id>=0) notice("等待系统安装确认 / Waiting for Android installation confirmation.");
        else changed();
    }
    void finishInstallation(int id,boolean success,int code) {
        if (!matchesSession(id)) return;
        if (active!=null) active.installationTerminal=true;
        if (confirmInstalled()) return;
        SharedPreferences.Editor result=preferences.edit().remove("pendingSession");
        if (!success) result.remove("pendingCode").remove("pendingVersion").putInt("failureCode",code);
        result.commit();
        if (success) {
            // A callback is not completion evidence. Re-read PackageManager after replacement.
            if (active==null) refreshInstallation();
            else notice("安装已返回，正在确认版本 / Confirming the installed version…");
        } else notice("安装未完成，可重试 / Installation did not finish ("+code+").");
    }
    void install() {
        start("正在验证并准备安装 / Preparing installation…",false,operation -> {
            JSONObject asset=readCandidate(); if (asset==null) throw new Exception("Check for updates again");
            verifyPackage(packageFile(),asset);
            PackageInstaller installer=context.getPackageManager().getPackageInstaller();
            PackageInstaller.SessionParams params=new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
            params.setAppPackageName(context.getPackageName()); params.setSize(packageFile().length());
            if (android.os.Build.VERSION.SDK_INT>=31) params.setRequireUserAction(PackageInstaller.SessionParams.USER_ACTION_REQUIRED);
            int id=installer.createSession(params);
            if (!preferences.edit().putInt("pendingSession",id).commit()) {
                installer.abandonSession(id); throw new Exception("Cannot save installation session");
            }
            try (PackageInstaller.Session session=installer.openSession(id); InputStream input=new FileInputStream(packageFile())) {
                try (OutputStream output=session.openWrite("base.apk",0,packageFile().length())) {
                    byte[] buffer=new byte[65536]; int n; while ((n=input.read(buffer))!=-1) output.write(buffer,0,n); session.fsync(output);
                }
                if (!preferences.edit().putLong("pendingCode",asset.getLong("build")).putString("pendingVersion",asset.getString("version"))
                        .putInt("pendingSession",id).remove("failureCode").commit()) throw new Exception("Cannot save installation state");
                session.commit(AppUpdateActivity.installationCallback(context,id).getIntentSender());
            } catch (Exception error) { installer.abandonSession(id); throw error; }
            return "请在系统安装器确认。更新完成后重新打开应用 / Confirm in Android, then reopen the app.";
        });
    }
    static String hex(byte[] bytes) { StringBuilder text=new StringBuilder(); for (byte b:bytes) text.append(String.format(java.util.Locale.ROOT,"%02x",b&255)); return text.toString(); }
    private static HttpURLConnection connect(String address,Operation operation) throws Exception {
        for (int redirects=0;redirects<6;redirects++) {
            operation.check(); URL url=new URL(address); String host=url.getHost();
            if (!"https".equals(url.getProtocol()) || url.getUserInfo()!=null || (url.getPort()!=-1 && url.getPort()!=443) ||
                    !(host.equals("github.com") || host.endsWith(".githubusercontent.com"))) throw new Exception("Untrusted update URL");
            HttpURLConnection connection=(HttpURLConnection)url.openConnection(); operation.connected(connection);
            connection.setConnectTimeout(20000); connection.setReadTimeout(30000); connection.setInstanceFollowRedirects(false);
            connection.setRequestProperty("User-Agent","XSIME-updater/2");
            int status;
            try { status=connection.getResponseCode(); }
            catch (Exception error) { connection.disconnect(); operation.connection=null; throw error; }
            if (status>=300 && status<400) { String location=connection.getHeaderField("Location"); connection.disconnect(); operation.connection=null; address=new URL(url,location).toString(); continue; }
            if (status!=200) { connection.disconnect(); operation.connection=null; throw new Exception("HTTP "+status); }
            return connection;
        }
        throw new Exception("Too many redirects");
    }
    private static byte[] fetch(String url,int limit,Operation operation) throws Exception {
        HttpURLConnection connection=connect(url,operation);
        try (InputStream input=connection.getInputStream(); ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192]; int n;
            while ((n=input.read(buffer))!=-1) { operation.check(); if (output.size()+n>limit) throw new Exception("Response too large"); output.write(buffer,0,n); }
            return output.toByteArray();
        } finally { connection.disconnect(); operation.connection=null; }
    }
}
