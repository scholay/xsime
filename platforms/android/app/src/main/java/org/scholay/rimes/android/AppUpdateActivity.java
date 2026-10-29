package org.scholay.rimes.android;

import android.app.Activity;
import android.app.ActivityOptions;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;
import org.json.JSONObject;

/** UI only. Download/install work belongs to the application-scoped manager. */
public final class AppUpdateActivity extends Activity {
    private static final String RESULT="org.scholay.rimes.android.UPDATE_RESULT";
    private AppUpdateManager manager;
    private TextView status;
    private ProgressBar progress;
    private Button check,download,install,cancel;
    private final AppUpdateManager.Observer observer=state -> {
        status.setText(state.message); progress.setProgress(state.progress);
        check.setEnabled(!state.busy); download.setEnabled(state.canDownload);
        install.setEnabled(state.canInstall); cancel.setEnabled(state.canCancel);
    };
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved); setTitle("检查更新 / Software update");
        manager=AppUpdateManager.get(this);
        LinearLayout body=new LinearLayout(this); body.setOrientation(LinearLayout.VERTICAL);
        int padding=(int)(24*getResources().getDisplayMetrics().density); body.setPadding(padding,padding,padding,padding);
        status=new TextView(this); status.setTextSize(16); body.addView(status);
        progress=new ProgressBar(this,null,android.R.attr.progressBarStyleHorizontal); progress.setMax(100); body.addView(progress);
        check=button(body,"检查更新 / Check for updates",manager::check);
        download=button(body,"下载更新 / Download",manager::download);
        install=button(body,"安装更新 / Install",this::install);
        cancel=button(body,"取消 / Cancel",manager::cancel);
        ScrollView scroll=new ScrollView(this); scroll.addView(body);
        scroll.setOnApplyWindowInsetsListener((view,insets) -> {
            if (android.os.Build.VERSION.SDK_INT>=30) {
                android.graphics.Insets safe=insets.getInsets(WindowInsets.Type.systemBars()|WindowInsets.Type.displayCutout());
                view.setPadding(safe.left,safe.top,safe.right,safe.bottom);
            } else view.setPadding(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),insets.getSystemWindowInsetRight(),insets.getSystemWindowInsetBottom());
            return insets;
        });
        setContentView(scroll); handleResult(getIntent());
    }
    @Override protected void onStart() { super.onStart(); manager.observe(observer); }
    @Override protected void onStop() { manager.removeObserver(observer); super.onStop(); }
    @Override protected void onResume() { super.onResume(); manager.refreshInstallation(); }
    @Override protected void onNewIntent(Intent intent) { super.onNewIntent(intent); setIntent(intent); handleResult(intent); }
    private Button button(LinearLayout body,String title,Runnable action) {
        Button value=new Button(this); value.setText(title); value.setOnClickListener(v -> action.run()); body.addView(value); return value;
    }
    private void install() {
        if (!getPackageManager().canRequestPackageInstalls()) {
            manager.notice("请允许此应用安装更新，然后返回点击安装 / Allow updates from this app, then return and tap Install.");
            startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:"+getPackageName()))); return;
        }
        manager.install();
    }
    private void handleResult(Intent intent) {
        if (!RESULT.equals(intent.getAction())) return;
        int id=intent.getIntExtra("updaterSession",-1);
        if (!manager.matchesSession(id)) return;
        int code=intent.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE);
        if (code==PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirmation=android.os.Build.VERSION.SDK_INT>=33?intent.getParcelableExtra(Intent.EXTRA_INTENT,Intent.class):intent.getParcelableExtra(Intent.EXTRA_INTENT);
            try {
                if (confirmation==null) throw new IllegalStateException("Missing Android installer confirmation");
                startActivity(confirmation);
            } catch (RuntimeException error) { manager.finishInstallation(id,false,PackageInstaller.STATUS_FAILURE_BLOCKED); }
        } else manager.finishInstallation(id,code==PackageInstaller.STATUS_SUCCESS,code);
        intent.setAction(null);
    }
    static void validateAsset(JSONObject asset) throws Exception { AppUpdateManager.validateAsset(asset); }
    static PendingIntent installationCallback(Context context,int sessionId) {
        Intent result=new Intent(context,AppUpdateActivity.class).setAction(RESULT).putExtra("updaterSession",sessionId)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_CLEAR_TOP|Intent.FLAG_ACTIVITY_SINGLE_TOP);
        ActivityOptions options=ActivityOptions.makeBasic();
        if (android.os.Build.VERSION.SDK_INT>=35) {
            options.setPendingIntentCreatorBackgroundActivityStartMode(android.os.Build.VERSION.SDK_INT>=36
                    ?ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOW_IF_VISIBLE:ActivityOptions.MODE_BACKGROUND_ACTIVITY_START_ALLOWED);
        }
        return PendingIntent.getActivity(context,sessionId,result,PendingIntent.FLAG_UPDATE_CURRENT|PendingIntent.FLAG_MUTABLE,options.toBundle());
    }
}
