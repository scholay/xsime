package org.scholay.rimes.android;

import android.util.Base64;
import org.json.JSONArray;
import org.json.JSONObject;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.PublicKey;
import java.security.Signature;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPublicKeySpec;
import java.util.Arrays;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Set;

/** Verifies publisher-owned P-256 signatures before any version or package metadata is trusted. */
final class UpdateManifestVerifier {
    interface Trust { String publicKeyFor(String keyId); }
    private static final long MAX_LIFETIME=90L*24*60*60;
    static JSONObject verify(String document,long now,Trust trust) throws Exception {
        if (document==null || document.length()>1024*1024 || now<0 || now>253402300499L) throw new Exception("Invalid update envelope");
        JSONObject outer=new JSONObject(document);
        JSONObject authentication=outer.getJSONObject("authentication");
        if (integer(authentication,"format")!=1 || !"ecdsa-p256-sha256".equals(string(authentication,"algorithm"))) throw new Exception("Unsupported update authentication");
        byte[] payload=decode(string(authentication,"payload"));
        if (payload.length==0 || payload.length>1024*1024) throw new Exception("Invalid signed payload size");
        JSONArray signatures=authentication.getJSONArray("signatures");
        if (signatures.length()<1 || signatures.length()>8) throw new Exception("Invalid update signature count");
        boolean valid=false; Set<String> identifiers=new HashSet<>();
        for (int i=0;i<signatures.length();i++) {
            JSONObject entry=signatures.getJSONObject(i);
            if (entry.length()!=2) throw new Exception("Invalid update signature record");
            String keyId=string(entry,"keyId");
            if (!keyId.matches("[A-Za-z0-9._-]{1,80}") || !identifiers.add(keyId)) throw new Exception("Invalid or duplicate update key");
            byte[] raw=decode(string(entry,"signature"));
            if (raw.length!=64) throw new Exception("Invalid update signature");
            String publicKey=trust.publicKeyFor(keyId);
            if (publicKey==null) continue;
            Signature verifier=Signature.getInstance("SHA256withECDSA");
            verifier.initVerify(publicKey(publicKey)); verifier.update(payload);
            try { valid|=verifier.verify(der(raw)); }
            catch (java.security.SignatureException invalid) { /* Another trusted rotation signature may still verify. */ }
        }
        if (!valid) throw new Exception("Update publisher signature is not trusted");
        JSONObject signed=new JSONObject(utf8(payload));
        outer.remove("authentication");
        if (!equivalent(outer,signed)) throw new Exception("Signed update metadata does not match envelope");
        long issued=integer(signed,"issuedAt"),expires=integer(signed,"expiresAt");
        if (issued<0 || issued>253402300799L || expires>253402300799L || issued>now+300 || expires<=now || expires<=issued || expires-issued>MAX_LIFETIME)
            throw new Exception("Update metadata is expired or has an invalid validity period");
        if (integer(signed,"schemaVersion")!=1) throw new Exception("Unsupported update format");
        version(string(signed,"version"));
        if (!string(signed,"sourceCommit").matches("[0-9a-f]{40}")) throw new Exception("Invalid source identity");
        return signed;
    }
    static long integer(JSONObject object,String key) throws Exception {
        Object value=object.get(key);
        if (!(value instanceof Integer) && !(value instanceof Long)) throw new Exception("Expected integer: "+key);
        return ((Number)value).longValue();
    }
    private static String string(JSONObject object,String key) throws Exception {
        Object value=object.get(key); if (!(value instanceof String)) throw new Exception("Expected string: "+key); return (String)value;
    }
    static void requireNotOlder(String version,long build,String trustedVersion,long trustedBuild) throws Exception {
        if (build<=0 || trustedBuild<0) throw new Exception("Invalid update build");
        BigInteger[] current=version(version);
        if (trustedVersion!=null && !trustedVersion.isEmpty()) {
            BigInteger[] floor=version(trustedVersion); int comparison=0;
            for (int i=0;i<3 && comparison==0;i++) comparison=current[i].compareTo(floor[i]);
            if (comparison<0 || build<trustedBuild) throw new Exception("Update metadata rollback rejected");
        } else if (build<trustedBuild) throw new Exception("Update build rollback rejected");
    }
    private static BigInteger[] version(String value) throws Exception {
        if (value==null || value.length()>64 || !value.matches("(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)")) throw new Exception("Invalid update version");
        String[] parts=value.split("\\."); return new BigInteger[]{new BigInteger(parts[0]),new BigInteger(parts[1]),new BigInteger(parts[2])};
    }
    private static byte[] decode(String value) throws Exception {
        if (value.length()%4!=0) throw new Exception("Invalid update base64");
        byte[] bytes=Base64.decode(value,Base64.NO_WRAP);
        if (!Base64.encodeToString(bytes,Base64.NO_WRAP).equals(value)) throw new Exception("Non-canonical update base64");
        return bytes;
    }
    private static String utf8(byte[] bytes) throws CharacterCodingException {
        return StandardCharsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT).decode(ByteBuffer.wrap(bytes)).toString();
    }
    private static PublicKey publicKey(String hex) throws Exception {
        if (!hex.matches("04[0-9a-fA-F]{128}")) throw new Exception("Invalid bundled update key");
        byte[] raw=new byte[65]; for (int i=0;i<raw.length;i++) raw[i]=(byte)Integer.parseInt(hex.substring(i*2,i*2+2),16);
        AlgorithmParameters parameters=AlgorithmParameters.getInstance("EC"); parameters.init(new ECGenParameterSpec("secp256r1"));
        ECPoint point=new ECPoint(new BigInteger(1,Arrays.copyOfRange(raw,1,33)),new BigInteger(1,Arrays.copyOfRange(raw,33,65)));
        return KeyFactory.getInstance("EC").generatePublic(new ECPublicKeySpec(point,parameters.getParameterSpec(ECParameterSpec.class)));
    }
    private static byte[] integer(byte[] raw,int offset) {
        int start=offset,end=offset+32;
        while (start<end-1 && raw[start]==0) start++;
        boolean pad=(raw[start]&0x80)!=0;
        byte[] value=new byte[end-start+(pad?1:0)]; System.arraycopy(raw,start,value,pad?1:0,end-start); return value;
    }
    private static byte[] der(byte[] raw) {
        byte[] r=integer(raw,0),s=integer(raw,32),result=new byte[6+r.length+s.length];
        int i=0; result[i++]=0x30; result[i++]=(byte)(result.length-2);
        result[i++]=0x02; result[i++]=(byte)r.length; System.arraycopy(r,0,result,i,r.length); i+=r.length;
        result[i++]=0x02; result[i++]=(byte)s.length; System.arraycopy(s,0,result,i,s.length); return result;
    }
    private static boolean equivalent(Object left,Object right) throws Exception {
        if (left instanceof JSONObject && right instanceof JSONObject) {
            JSONObject a=(JSONObject)left,b=(JSONObject)right;
            if (a.length()!=b.length()) return false;
            Iterator<String> keys=a.keys(); while (keys.hasNext()) { String key=keys.next(); if (!b.has(key) || !equivalent(a.get(key),b.get(key))) return false; }
            return true;
        }
        if (left instanceof JSONArray && right instanceof JSONArray) {
            JSONArray a=(JSONArray)left,b=(JSONArray)right; if (a.length()!=b.length()) return false;
            for (int i=0;i<a.length();i++) if (!equivalent(a.get(i),b.get(i))) return false; return true;
        }
        if (left instanceof Number && right instanceof Number) return new BigDecimal(left.toString()).compareTo(new BigDecimal(right.toString()))==0;
        return left==null?right==null:left.equals(right);
    }
}
