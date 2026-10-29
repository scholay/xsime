package org.scholay.rimes.android;

import android.app.Instrumentation;
import android.content.Context;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.util.Base64;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.atomic.AtomicReference;

final class UpdateManifestContract {
    interface Attempt { void run() throws Exception; }
    static void rejected(Attempt attempt) throws Exception {
        boolean failed=false; try { attempt.run(); } catch (Exception expected) { failed=true; }
        if (!failed) throw new AssertionError("Untrusted update metadata accepted");
    }
    static void run(Instrumentation instrumentation) throws Exception {
        String text;
        try (InputStream input=instrumentation.getContext().getAssets().open("signed_manifest.json"); ByteArrayOutputStream output=new ByteArrayOutputStream()) {
            byte[] buffer=new byte[8192]; int n; while ((n=input.read(buffer))!=-1) output.write(buffer,0,n);
            text=new String(output.toByteArray(),StandardCharsets.UTF_8);
        }
        JSONObject fixture=new JSONObject(text),original=fixture.getJSONObject("document"),keys=fixture.getJSONObject("trustedKeys");
        long now=fixture.getLong("now"); UpdateManifestVerifier.Trust trust=id -> keys.optString(id,null);
        JSONObject verified=UpdateManifestVerifier.verify(original.toString(),now,trust);
        if (!verified.getString("version").equals(original.getString("version"))) throw new AssertionError("Wrong verified payload");
        // Outer serialization is irrelevant; the exact signed payload bytes are authoritative.
        UpdateManifestVerifier.verify(original.toString(2),now,trust);
        JSONObject unsigned=new JSONObject(original.toString()); unsigned.remove("authentication");
        rejected(() -> UpdateManifestVerifier.verify(unsigned.toString(),now,trust));
        JSONObject altered=new JSONObject(original.toString()).put("version","99.0.0");
        rejected(() -> UpdateManifestVerifier.verify(altered.toString(),now,trust));
        JSONObject tampered=new JSONObject(original.toString());
        JSONObject auth=tampered.getJSONObject("authentication");
        byte[] signature=Base64.decode(auth.getJSONArray("signatures").getJSONObject(0).getString("signature"),Base64.NO_WRAP);
        signature[0]^=1; auth.getJSONArray("signatures").getJSONObject(0).put("signature",Base64.encodeToString(signature,Base64.NO_WRAP));
        rejected(() -> UpdateManifestVerifier.verify(tampered.toString(),now,trust));
        JSONObject reencoded=new JSONObject(original.toString());
        JSONObject reauth=reencoded.getJSONObject("authentication");
        String payload=new String(Base64.decode(reauth.getString("payload"),Base64.NO_WRAP),StandardCharsets.UTF_8)+"\n";
        reauth.put("payload",Base64.encodeToString(payload.getBytes(StandardCharsets.UTF_8),Base64.NO_WRAP));
        rejected(() -> UpdateManifestVerifier.verify(reencoded.toString(),now,trust));
        rejected(() -> UpdateManifestVerifier.verify(original.toString(),original.getLong("expiresAt"),trust));
        rejected(() -> UpdateManifestVerifier.verify(original.toString(),original.getLong("issuedAt")-301,trust));
        rejected(() -> UpdateManifestVerifier.verify(original.toString(),now,id -> null));
        rejected(() -> UpdateManifestVerifier.verify(original.toString(),Long.MAX_VALUE,trust));
        JSONObject extended=new JSONObject(original.toString());
        extended.getJSONObject("authentication").getJSONArray("signatures").getJSONObject(0).put("extension",true);
        rejected(() -> UpdateManifestVerifier.verify(extended.toString(),now,trust));
        JSONObject longId=new JSONObject(original.toString());
        String key80=new String(new char[80]).replace('\0','a');
        longId.getJSONObject("authentication").getJSONArray("signatures").getJSONObject(0).put("keyId",key80);
        String publicKey=keys.getString(keys.keys().next());
        UpdateManifestVerifier.verify(longId.toString(),now,id -> key80.equals(id)?publicKey:null);
        JSONObject duplicate=new JSONObject(original.toString()); JSONArray signatures=duplicate.getJSONObject("authentication").getJSONArray("signatures");
        signatures.put(new JSONObject(signatures.getJSONObject(0).toString()));
        rejected(() -> UpdateManifestVerifier.verify(duplicate.toString(),now,trust));
        // Unknown keys coexist with a trusted signature during a publisher key rotation.
        JSONObject rotation=new JSONObject(original.toString()); JSONArray rotationSignatures=rotation.getJSONObject("authentication").getJSONArray("signatures");
        rotationSignatures.put(new JSONObject(rotationSignatures.getJSONObject(0).toString()).put("keyId","future-untrusted-key"));
        UpdateManifestVerifier.verify(rotation.toString(),now,trust);
        UpdateManifestVerifier.requireNotOlder("1.10.0",24,"1.9.9",23);
        UpdateManifestVerifier.requireNotOlder("1.10.0",24,"1.10.0",24);
        rejected(() -> UpdateManifestVerifier.requireNotOlder("1.9.9",25,"1.10.0",24));
        rejected(() -> UpdateManifestVerifier.requireNotOlder("1.11.0",23,"1.10.0",24));
        rejected(() -> UpdateManifestVerifier.requireNotOlder("01.11.0",25,"1.10.0",24));
        installedVersionFloor(instrumentation);
    }
    private static void installedVersionFloor(Instrumentation instrumentation) throws Exception {
        Context context=instrumentation.getTargetContext();
        if (!BuildConfig.DEBUG || !"org.scholay.rimes.android.debug".equals(context.getPackageName()))
            throw new AssertionError("Trust-floor preferences are tested only in the disposable debug app");
        PackageInfo installed=context.getPackageManager().getPackageInfo(context.getPackageName(),0);
        long code=AppUpdateManager.versionCode(installed);
        if (code<=1 || "0.0.0".equals(installed.versionName)) throw new AssertionError("Version-floor fixture needs an established installed version");
        AtomicReference<AppUpdateManager> manager=new AtomicReference<>();
        instrumentation.runOnMainSync(() -> manager.set(AppUpdateManager.get(context)));
        SharedPreferences preferences=context.getSharedPreferences(AppUpdateManager.PREFS,Context.MODE_PRIVATE);
        boolean hadVersion=preferences.contains("trustedVersion"),hadBuild=preferences.contains("trustedBuild");
        String savedVersion=preferences.getString("trustedVersion",""); long savedBuild=preferences.getLong("trustedBuild",0);
        try {
            if (!preferences.edit().remove("trustedVersion").remove("trustedBuild").commit()) throw new AssertionError("Could not clear trust-floor fixture");
            // First check has no persisted high-water mark, but installed metadata is already trusted.
            rejected(() -> manager.get().requireVersionFloor(new JSONObject().put("version","0.0.0").put("build",code+1)));
            rejected(() -> manager.get().requireVersionFloor(new JSONObject().put("version",installed.versionName).put("build",code-1)));
            manager.get().requireVersionFloor(new JSONObject().put("version",installed.versionName).put("build",code));
            if (preferences.contains("trustedVersion") || preferences.contains("trustedBuild")) throw new AssertionError("A rejected check wrote the trust floor");
            if (!preferences.edit().putString("trustedVersion",installed.versionName).putLong("trustedBuild",code+1).commit()) throw new AssertionError("Could not save trusted-build fixture");
            rejected(() -> manager.get().requireVersionFloor(new JSONObject().put("version",installed.versionName).put("build",code)));
            manager.get().requireVersionFloor(new JSONObject().put("version",installed.versionName).put("build",code+1));
        } finally {
            SharedPreferences.Editor restore=preferences.edit().remove("trustedVersion").remove("trustedBuild");
            if (hadVersion) restore.putString("trustedVersion",savedVersion);
            if (hadBuild) restore.putLong("trustedBuild",savedBuild);
            if (!restore.commit()) throw new AssertionError("Could not restore trusted-version fixture");
        }
    }
}
