package org.scholay.rimes.testhost;

import android.app.Instrumentation;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import org.json.JSONObject;
import org.xmlpull.v1.XmlPullParser;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.StringReader;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Iterator;

/** External process survives replacement of the actual keyboard app. No simulated completion flag. */
final class AppUpdateUpgradeContract {
    private static final String PACKAGE="org.scholay.rimes.android.debug";
    static String run(Instrumentation instrumentation) throws Exception {
        if (android.os.Build.VERSION.SDK_INT>=29)
            instrumentation.getUiAutomation().adoptShellPermissionIdentity("android.permission.QUERY_ALL_PACKAGES");
        try {
            PackageManager pm=instrumentation.getTargetContext().getPackageManager();
            PackageInfo before=pm.getPackageInfo(PACKAGE,0);
            if ((before.applicationInfo.flags&ApplicationInfo.FLAG_DEBUGGABLE)==0) throw new AssertionError("Only the disposable debug app can be upgraded");
            JSONObject fixture=new JSONObject(string(shell(instrumentation,"run-as "+PACKAGE+" cat files/update-fixture-state.json")));
            long previous=fixture.getLong("previousCode"),expected=fixture.getLong("expectedCode");
            if (code(before)!=previous || expected<=previous) throw new AssertionError("Expected two distinct increasing installed versions");
            shell(instrumentation,"appops set "+PACKAGE+" REQUEST_INSTALL_PACKAGES allow");
            openUpdater(instrumentation);
            String oldPid=string(shell(instrumentation,"pidof "+PACKAGE)).trim();
            click(instrumentation,PACKAGE,new String[]{"安装更新 / Install"},15000);
            click(instrumentation,"packageinstaller",new String[]{"Update","Install","更新","安装"},30000);
            long deadline=SystemClock.uptimeMillis()+90000;
            PackageInfo after=before;
            while (SystemClock.uptimeMillis()<deadline) {
                try { after=pm.getPackageInfo(PACKAGE,0); if (code(after)==expected) break; }
                catch (PackageManager.NameNotFoundException replacing) { }
                SystemClock.sleep(200);
            }
            if (code(after)!=expected || !fixture.getString("expectedVersion").equals(after.versionName)) throw new AssertionError("Android did not install the expected package");
            openUpdater(instrumentation);
            awaitText(instrumentation,"Updated to "+after.versionName,15000);
            String newPid=string(shell(instrumentation,"pidof "+PACKAGE)).trim();
            if (oldPid.isEmpty() || newPid.isEmpty() || oldPid.equals(newPid)) throw new AssertionError("The keyboard app process was not replaced");
            JSONObject retained=fixture.getJSONObject("retained"); Iterator<String> paths=retained.keys();
            while (paths.hasNext()) {
                String path=paths.next();
                if (!path.matches("[A-Za-z0-9_./-]+")) throw new AssertionError("Invalid fixture path");
                byte[] data=shell(instrumentation,"run-as "+PACKAGE+" cat "+path);
                if (!retained.getString(path).equals(hex(MessageDigest.getInstance("SHA-256").digest(data)))) throw new AssertionError("Private fixture data changed during upgrade");
            }
            String preferences=string(shell(instrumentation,"run-as "+PACKAGE+" cat shared_prefs/keyboard.xml"));
            XmlPullParser parser=android.util.Xml.newPullParser(); parser.setInput(new StringReader(preferences));
            boolean height=false,space=false,learning=false;
            while (parser.next()!=XmlPullParser.END_DOCUMENT) if (parser.getEventType()==XmlPullParser.START_TAG) {
                String name=parser.getAttributeValue(null,"name"),value=parser.getAttributeValue(null,"value");
                if ("portrait_height".equals(name)) height="117".equals(value);
                if ("wide_space".equals(name)) space="true".equals(value);
                if ("learning".equals(name)) learning="false".equals(value);
            }
            if (!height || !space || !learning) throw new AssertionError("Keyboard preferences were not retained");
            return "PASS real application upgrade; oldCode="+previous+" newCode="+expected+" processReplaced=true privateDataRetained=true\n";
        } finally { if (android.os.Build.VERSION.SDK_INT>=29) instrumentation.getUiAutomation().dropShellPermissionIdentity(); }
    }
    private static long code(PackageInfo value) { return android.os.Build.VERSION.SDK_INT>=28?value.getLongVersionCode():value.versionCode; }
    private static String string(byte[] bytes) { return new String(bytes,StandardCharsets.UTF_8); }
    private static String hex(byte[] data) { StringBuilder text=new StringBuilder(); for (byte value:data) text.append(String.format(java.util.Locale.ROOT,"%02x",value&255)); return text.toString(); }
    private static byte[] shell(Instrumentation instrumentation,String command) throws Exception {
        try (ParcelFileDescriptor process=instrumentation.getUiAutomation().executeShellCommand(command);
             InputStream input=new ParcelFileDescriptor.AutoCloseInputStream(process); ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192]; int n;
            while ((n=input.read(buffer))!=-1) { if (output.size()+n>1024*1024) throw new AssertionError("Oversized diagnostic output"); output.write(buffer,0,n); }
            return output.toByteArray();
        }
    }
    private static void openUpdater(Instrumentation instrumentation) throws Exception {
        shell(instrumentation,"am start -W -f 0x10008000 -n "+PACKAGE+"/.SetupActivity");
        click(instrumentation,PACKAGE,new String[]{"Software update","检查更新"},15000);
    }
    private static void click(Instrumentation instrumentation,String packagePart,String[] labels,long timeout) {
        long deadline=SystemClock.uptimeMillis()+timeout;
        while (SystemClock.uptimeMillis()<deadline) {
            AccessibilityNodeInfo root=instrumentation.getUiAutomation().getRootInActiveWindow();
            if (root!=null && root.getPackageName()!=null && root.getPackageName().toString().contains(packagePart)) {
                AccessibilityNodeInfo target=find(root,labels);
                if (target!=null && target.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return;
                scroll(root);
            }
            SystemClock.sleep(100);
        }
        throw new AssertionError("Expected update/installer action did not appear");
    }
    private static AccessibilityNodeInfo find(AccessibilityNodeInfo node,String[] labels) {
        if (node.isVisibleToUser() && node.isEnabled() && node.isClickable()) {
            for (String label:labels) if (label.contentEquals(node.getText()==null?"":node.getText()) || label.contentEquals(node.getContentDescription()==null?"":node.getContentDescription())) return node;
        }
        for (int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo child=node.getChild(i); if (child!=null) { AccessibilityNodeInfo match=find(child,labels); if (match!=null) return match; } }
        return null;
    }
    private static boolean scroll(AccessibilityNodeInfo node) {
        if (node.isScrollable() && node.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)) return true;
        for (int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo child=node.getChild(i); if (child!=null && scroll(child)) return true; }
        return false;
    }
    private static void awaitText(Instrumentation instrumentation,String text,long timeout) {
        long deadline=SystemClock.uptimeMillis()+timeout;
        while (SystemClock.uptimeMillis()<deadline) {
            AccessibilityNodeInfo root=instrumentation.getUiAutomation().getRootInActiveWindow();
            if (root!=null && PACKAGE.contentEquals(root.getPackageName()==null?"":root.getPackageName())) {
                for (AccessibilityNodeInfo node:root.findAccessibilityNodeInfosByText(text)) if (node.isVisibleToUser()) return;
            }
            SystemClock.sleep(100);
        }
        throw new AssertionError("Updater did not confirm the installed version");
    }
}
