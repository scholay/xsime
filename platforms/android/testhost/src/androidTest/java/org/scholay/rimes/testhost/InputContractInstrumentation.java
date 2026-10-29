package org.scholay.rimes.testhost;

import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.Instrumentation;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import java.util.concurrent.atomic.AtomicReference;

/** Drives real RIMES buttons through accessibility, then checks actual host text. No text-injection IME. */
public final class InputContractInstrumentation extends Instrumentation {
    private String IME="org.scholay.rimes.android.debug";
    private HostActivity host;
    private Bundle arguments;
    private int assertions;
    private boolean allowStaleRejection;
    @Override public void onCreate(Bundle args) {
        super.onCreate(args); arguments=args;
        IME=args.getString("ime","org.scholay.rimes.android.debug");
        if(!IME.equals("org.scholay.rimes.android") && !IME.equals("org.scholay.rimes.android.debug"))
            throw new IllegalArgumentException("Input contract accepts only the RIMES app packages");
        start();
    }
    @Override public void onStart() {
        Bundle result=new Bundle();
        try {
            if ("app-update-upgrade".equals(arguments.getString("mode"))) {
                result.putString("stream",AppUpdateUpgradeContract.run(this)); finish(-1,result); return;
            }
            report("START accessibility");
            AccessibilityServiceInfo info=getUiAutomation().getServiceInfo();
            info.flags|=AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS;
            getUiAutomation().setServiceInfo(info);
            report("START host");
            ActivityMonitor monitor=addMonitor(HostActivity.class.getName(),null,false);
            // Launch under the same shell identity as adb, avoiding OEM background-activity
            // restrictions after instrumentation restarts its target process. Never wait forever.
            try(android.os.ParcelFileDescriptor launch=getUiAutomation().executeShellCommand(
                    "am start -W -f 0x10008000 -n org.scholay.rimes.testhost/.HostActivity")) {
                host=(HostActivity)waitForMonitorWithTimeout(monitor,15000);
            }
            removeMonitor(monitor); check(host!=null,"validation host launch within 15 seconds");
            waitForIdleSync(); report("START contract");
            // OEMs can deny an implicit show after an instrumentation process restart.
            // Begin every contract with a real editor touch so IME ownership is observable.
            touchHostField();
            if("candidate-matrix".equals(arguments.getString("mode"))) matrixContract();
            else if("closure-fields".equals(arguments.getString("mode"))) closureFields();
            else if("delete-video".equals(arguments.getString("mode"))) deleteVideoContract();
            else if("closure-punctuation".equals(arguments.getString("mode"))) closurePunctuation();
            else if("closure-thumbs".equals(arguments.getString("mode"))) closureThumbs();
            else if("community".equals(arguments.getString("mode"))) communityContract();
            else if("chord".equals(arguments.getString("mode"))) chordContract();
            else if("touch".equals(arguments.getString("mode"))) nativeTouchContract();
            else if("password".equals(arguments.getString("mode"))) { focus(host.first); pinyin(); passwordContract(); }
            else if("benchmark".equals(arguments.getString("mode"))) benchmark();
            else if("layout".equals(arguments.getString("mode"))) layoutContract();
            else if("appearance".equals(arguments.getString("mode"))) appearanceContract();
            else if("plugins".equals(arguments.getString("mode"))) pluginsContract();
            else if("comet-live".equals(arguments.getString("mode"))) cometLiveContract();
            else if("comet-denied".equals(arguments.getString("mode"))) cometDeniedContract();
            else if(!"soak".equals(arguments.getString("mode"))) contract();
            else soak(Long.parseLong(arguments.getString("seconds","1800")));
            result.putString("stream","PASS input contract; assertions="+assertions+"\n"); finish(-1,result);
        } catch(Throwable error) {
            try {
                android.graphics.Bitmap bitmap=getUiAutomation().takeScreenshot();
                if(bitmap!=null) try(java.io.FileOutputStream out=getTargetContext().openFileOutput("failure.png",0)) {
                    bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG,100,out);
                }
            } catch(Exception ignored) { /* Original assertion remains authoritative. */ }
            result.putString("stream","FAIL "+android.util.Log.getStackTraceString(error)); finish(0,result);
        }
    }
    private void check(boolean value,String label) { assertions++; if(!value) throw new AssertionError(label); }
    private String read(EditText field) {
        AtomicReference<String> value=new AtomicReference<>(); runOnMainSync(() -> value.set(field.getText().toString())); return value.get();
    }
    private void expect(EditText field,String wanted,String label) {
        long deadline=SystemClock.uptimeMillis()+4000;
        String actual;
        do { actual=read(field); if(wanted.equals(actual)) { assertions++; return; } SystemClock.sleep(25); } while(SystemClock.uptimeMillis()<deadline);
        throw new AssertionError(label+" expected=["+wanted+"] actual=["+actual+"]");
    }
    private void expectStable(EditText field,String wanted,String label) {
        long until=SystemClock.uptimeMillis()+600;
        do { check(wanted.equals(read(field)),label); SystemClock.sleep(30); } while(SystemClock.uptimeMillis()<until);
    }
    private void waitKeyboardHidden() {
        long deadline=SystemClock.uptimeMillis()+5000;
        do {
            boolean present=false;
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows())
                if(window.getType()==AccessibilityWindowInfo.TYPE_INPUT_METHOD) { present=true; break; }
            if(!present) { check(true,"keyboard window hidden after animation"); return; }
            SystemClock.sleep(30);
        } while(SystemClock.uptimeMillis()<deadline);
        throw new AssertionError("Keyboard window did not hide within five seconds");
    }
    private AccessibilityNodeInfo search(AccessibilityNodeInfo node,String value,boolean description) {
        if(node==null) return null;
        CharSequence label=description?node.getContentDescription():node.getText();
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName()) && value.contentEquals(label==null?"":label)
                && node.isVisibleToUser() && node.isClickable()) return node;
        for(int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo found=search(node.getChild(i),value,description); if(found!=null) return found; }
        return null;
    }
    private AccessibilityNodeInfo find(String value,boolean description) {
        if(value.equals("↵")) { value="Enter"; description=true; }
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            AccessibilityNodeInfo node=search(window.getRoot(),value,description); if(node!=null) return node;
            if(!description) { node=search(window.getRoot(),value,true); if(node!=null) return node; }
        }
        return null;
    }
    private AccessibilityNodeInfo searchLabel(AccessibilityNodeInfo node,String value,boolean description,boolean visible) {
        if(node==null) return null;
        CharSequence label=description?node.getContentDescription():node.getText();
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName())
                && value.contentEquals(label==null?"":label) && (!visible || node.isVisibleToUser())) return node;
        for(int i=0;i<node.getChildCount();i++) {
            AccessibilityNodeInfo found=searchLabel(node.getChild(i),value,description,visible); if(found!=null) return found;
        }
        return null;
    }
    /** Output and source rows intentionally are not clickable. */
    private AccessibilityNodeInfo findLabel(String value,boolean description) {
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            AccessibilityNodeInfo found=searchLabel(window.getRoot(),value,description,true); if(found!=null) return found;
        }
        return null;
    }
    private AccessibilityNodeInfo waitLabel(String value,boolean description) {
        long deadline=SystemClock.uptimeMillis()+5000;
        do { AccessibilityNodeInfo node=findLabel(value,description); if(node!=null) return node; SystemClock.sleep(30); } while(SystemClock.uptimeMillis()<deadline);
        throw new AssertionError("Missing keyboard label: "+value);
    }
    private AccessibilityNodeInfo searchPluginPanelButton(AccessibilityNodeInfo node,String value) {
        if(node==null) return null;
        if("android.widget.ScrollView".contentEquals(node.getClassName()==null?"":node.getClassName())
                && node.isVisibleToUser() && searchLabel(node,"Buffer 插件设置",false,true)!=null)
            return search(node,value,true);
        for(int i=0;i<node.getChildCount();i++) {
            AccessibilityNodeInfo found=searchPluginPanelButton(node.getChild(i),value); if(found!=null) return found;
        }
        return null;
    }
    private void clickPluginPanel(String value) {
        AccessibilityNodeInfo button=null; long deadline=SystemClock.uptimeMillis()+5000;
        do {
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
                button=searchPluginPanelButton(window.getRoot(),value); if(button!=null) break;
            }
            if(button!=null && button.isEnabled()) break;
            SystemClock.sleep(30);
        } while(SystemClock.uptimeMillis()<deadline);
        check(button!=null && button.isEnabled() && button.performAction(AccessibilityNodeInfo.ACTION_CLICK),"click panel "+value);
        SystemClock.sleep(120);
    }
    private AccessibilityNodeInfo waitButton(String value) {
        long deadline=SystemClock.uptimeMillis()+5000;
        do { AccessibilityNodeInfo node=find(value,false); if(node!=null && node.isEnabled()) return node; SystemClock.sleep(30); } while(SystemClock.uptimeMillis()<deadline);
        throw new AssertionError("Missing enabled keyboard button: "+value);
    }
    private boolean scrollChooser(AccessibilityNodeInfo node) {
        if(node==null) return false;
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName())
                && "android.widget.ScrollView".contentEquals(node.getClassName()==null?"":node.getClassName())
                && node.isVisibleToUser() && node.isScrollable())
            return node.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD);
        for(int i=0;i<node.getChildCount();i++) if(scrollChooser(node.getChild(i))) return true;
        return false;
    }
    private boolean scrollPluginBar(AccessibilityNodeInfo node,String label,int action) {
        if(node==null) return false;
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName())
                && "android.widget.HorizontalScrollView".contentEquals(node.getClassName()==null?"":node.getClassName())
                && node.isVisibleToUser() && node.isScrollable() && searchLabel(node,label,true,false)!=null)
            return node.performAction(action);
        for(int i=0;i<node.getChildCount();i++) if(scrollPluginBar(node.getChild(i),label,action)) return true;
        return false;
    }
    private AccessibilityNodeInfo searchPet(AccessibilityNodeInfo node) {
        if(node==null) return null;
        CharSequence label=node.getContentDescription();
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName()) && node.isVisibleToUser()
                && node.isClickable() && label!=null && label.toString().startsWith("宠物与皮肤：")) return node;
        for(int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo found=searchPet(node.getChild(i)); if(found!=null) return found; }
        return null;
    }
    private AccessibilityNodeInfo petButton() {
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            AccessibilityNodeInfo found=searchPet(window.getRoot()); if(found!=null) return found;
        }
        return null;
    }
    private AccessibilityNodeInfo appearanceChooser(AccessibilityNodeInfo node) {
        if(node==null) return null;
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName()) && node.isVisibleToUser()
                && "android.widget.ScrollView".contentEquals(node.getClassName()==null?"":node.getClassName())
                && searchLabel(node,"宠物与皮肤选项",true,false)!=null) return node;
        for(int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo found=appearanceChooser(node.getChild(i)); if(found!=null) return found; }
        return null;
    }
    private AccessibilityNodeInfo appearanceChooser() {
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            AccessibilityNodeInfo found=appearanceChooser(window.getRoot()); if(found!=null) return found;
        }
        return null;
    }
    private void petAppearance() {
        AccessibilityNodeInfo pet=petButton();
        check(pet!=null && pet.isClickable() && pet.isLongClickable()
                && pet.getActionList().contains(AccessibilityNodeInfo.AccessibilityAction.ACTION_CLICK)
                && pet.getActionList().contains(AccessibilityNodeInfo.AccessibilityAction.ACTION_LONG_CLICK),
                "attached pet exposes native tap and long-click accessibility actions");
        check("轻点切换宠物与皮肤；长按选择".contentEquals(pet.getHintText()==null?"":pet.getHintText()),
                "attached pet explains its tap and hold actions");
        check(pet!=null && pet.isEnabled() && pet.performAction(AccessibilityNodeInfo.ACTION_LONG_CLICK),"hold real pet opens or closes appearance");
        SystemClock.sleep(150);
    }
    private int visiblePetCount(AccessibilityNodeInfo node) {
        if(node==null) return 0;
        CharSequence label=node.getContentDescription();
        int count=IME.contentEquals(node.getPackageName()==null?"":node.getPackageName())
                && node.isVisibleToUser() && label!=null && label.toString().startsWith("宠物与皮肤：")?1:0;
        for(int i=0;i<node.getChildCount();i++) count+=visiblePetCount(node.getChild(i));
        return count;
    }
    private void checkSinglePet() {
        int count=0;
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) count+=visiblePetCount(window.getRoot());
        check(count==1,"exactly one pet control is visible");
    }
    private void appearanceContract() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        checkSinglePet();
        String before=petButton().getContentDescription().toString();
        check(petButton().performAction(AccessibilityNodeInfo.ACTION_CLICK),"tap visible pet");
        SystemClock.sleep(200);
        check(!before.contentEquals(petButton().getContentDescription()),"tap changes pet and skin");
        check(appearanceChooser()==null,"pet tap keeps the typing keyboard open");
        petAppearance(); check(appearanceChooser()!=null,"pet hold opens the chooser");
        waitButton("宠物与皮肤选项"); screenshot("pet-chooser");
        petAppearance(); check(appearanceChooser()==null,"pet hold returns to keyboard");
        tap("Buffer off"); checkSinglePet();
        check(find("清空 Buffer",false)==null && find("粘贴剪贴板文字",false)==null,
                "Buffer rail has no duplicate clear or paste controls");
        type("nihao"); tap("Space"); waitLabel("Buffer 你好",true);
        expect(host.first,"","confirmed Chinese stays in Buffer");
        screenshot("buffer-chrome");
        tap("清空 Buffer"); waitLabel("Buffer ",true);
        check(appearanceChooser()==null,"explicit clear returns to the typing keyboard");
        expect(host.first,"","panel clear never changes host text");
        checkSinglePet();
        report("PASS pet tap/hold accessibility, single Buffer pet and explicit panel clear");
    }
    private void scrollAppearanceTo(String label) {
        for(int attempt=0;find(label,false)==null && attempt<12;attempt++) {
            AccessibilityNodeInfo chooser=appearanceChooser();
            if(chooser==null || !chooser.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD)) break;
            SystemClock.sleep(120);
        }
    }
    /** Removed rail controls are explicit panel actions, then return to the same input target. */
    private void bufferPanelAction(String value) {
        if(appearanceChooser()==null) petAppearance();
        click("键位与方案");
        String label=value.equals("清空 Buffer")?"清空 Buffer":value;
        scrollAppearanceTo(label); click(label);
        if(appearanceChooser()!=null) petAppearance();
    }
    private void tap(String value) {
        if(value.equals("键位布局") && petButton()!=null) {
            petAppearance();
            if(appearanceChooser()!=null) click("键位与方案");
            return;
        }
        if((value.equals("清空 Buffer") || value.equals("粘贴剪贴板文字")) && petButton()!=null && find(value,false)==null) {
            bufferPanelAction(value); return;
        }
        if((value.equals("拼音") || value.equals("自然码") || value.equals("五笔")) && modern()) {
            schema(value.equals("拼音")?"自然码":value.equals("自然码")?"五笔":"拼音"); return;
        }
        if(value.equals("Insert all") && modern() && find(value,false)==null) {
            AccessibilityNodeInfo send=find("Insert next",false);
            if(send==null) send=waitButton("Insert");
            check(send.isEnabled() && send.performAction(AccessibilityNodeInfo.ACTION_LONG_CLICK),"long click Insert next sends all");
            SystemClock.sleep(120); return;
        }
        // Match the visible glyph: current QWERTY has permanent marks, while older
        // layouts and nine-key route through their number page or punctuation selector.
        if((value.equals(",") || value.equals(".")) && modern() && find(value,false)==null) {
            String mark=value.equals(",")?"，":"。";
            if(find(mark,false)!=null) { click(mark); return; }
            if(find("q",false)!=null) {
                click("123"); waitButton("1");
                click(find(mark,false)!=null?mark:value);
                // Current pages return automatically for a first sentence mark.
                if(find("q",false)==null) click("ABC");
                waitButton("q"); return;
            }
            if(find("中文标点",false)!=null) { click("中文标点"); click(find(mark,false)!=null?mark:value); return; }
        }
        if(value.startsWith("Buffer 插件：")) {
            for(int action:new int[]{AccessibilityNodeInfo.ACTION_SCROLL_FORWARD,AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD}) {
                for(int attempt=0;find(value,false)==null && attempt<8;attempt++) {
                    boolean moved=false;
                    for(AccessibilityWindowInfo window:getUiAutomation().getWindows())
                        if(scrollPluginBar(window.getRoot(),value,action)) { moved=true; break; }
                    if(!moved) break;
                    SystemClock.sleep(120);
                }
                if(find(value,false)!=null) break;
            }
        }
        // The appearance panel scrolls independently; large fonts can place palettes below its viewport.
        if(value.startsWith("配色 ")) {
            if(appearanceChooser()!=null && find("宠物与皮肤选项",true)!=null) click("宠物与皮肤选项");
            for(int attempt=0;find(value,false)==null && attempt<8;attempt++) {
                boolean moved=false;
                for(AccessibilityWindowInfo window:getUiAutomation().getWindows())
                    if(scrollChooser(window.getRoot())) { moved=true; break; }
                if(!moved) break;
                SystemClock.sleep(120);
            }
        }
        click(value);
    }
    /** Direct click avoids re-entering schema, symbol-page or plugin routing in tap(). */
    private void click(String value) {
        AccessibilityNodeInfo node=waitButton(value);
        check(node.performAction(AccessibilityNodeInfo.ACTION_CLICK),"click "+value);
        SystemClock.sleep(120);
    }
    private void type(String text) { for(int i=0;i<text.length();i++) tap(text.substring(i,i+1)); }
    private void focus(EditText field) {
        runOnMainSync(() -> { field.setText(""); host.focus(field); getTargetContext().getSystemService(InputMethodManager.class).restartInput(field); });
        waitButton("q"); SystemClock.sleep(400);
    }
    private void pinyin() {
        if(find("英",false)!=null && find("英",false).isEnabled()) tap("英");
        if(modern()) { schema("拼音"); return; }
        if(find("自然码",false)!=null) tap("自然码");
        if(find("五笔",false)!=null) tap("五笔");
        waitButton("拼音");
    }
    private boolean modern() { AccessibilityNodeInfo node=find("键位布局",true); return petButton()!=null || node!=null && ("⚙".contentEquals(node.getText()) || "✓".contentEquals(node.getText())); }
    private void schema(String name) { tap("键位布局"); tap("中文方案 "+name); tap("键位布局"); }
    private void report(String value) {
        Bundle b=new Bundle(); b.putString("stream",value+"\n"); sendStatus(0,b);
        android.util.Log.i("RIMES-TEST",value);
    }
    /** End-to-end ACTION_CLICK acknowledgement to actual EditText mutation, without tap sleeps. */
    private void benchmark() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        java.util.ArrayList<Long> first=new java.util.ArrayList<>(), commit=new java.util.ArrayList<>();
        java.util.concurrent.atomic.AtomicLong changedAt=new java.util.concurrent.atomic.AtomicLong();
        runOnMainSync(() -> host.first.addTextChangedListener(new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s,int start,int count,int after) {}
            public void onTextChanged(CharSequence s,int start,int before,int count) { changedAt.set(SystemClock.elapsedRealtimeNanos()); }
            public void afterTextChanged(android.text.Editable s) {}
        }));
        int count=Integer.parseInt(arguments.getString("samples","60"));
        for(int i=0;i<count+5;i++) {
            focus(host.first); pinyin();
            AccessibilityNodeInfo n=waitButton("n"); changedAt.set(0);
            long start=SystemClock.elapsedRealtimeNanos();
            check(n.performAction(AccessibilityNodeInfo.ACTION_CLICK),"benchmark first key accepted");
            expect(host.first,"n","benchmark composing mutation");
            long firstTime=changedAt.get()-start;
            for(char c:"ihao".toCharArray()) {
                check(waitButton(String.valueOf(c)).performAction(AccessibilityNodeInfo.ACTION_CLICK),"benchmark next key accepted");
            }
            waitButton("你好"); AccessibilityNodeInfo space=waitButton("Space"); changedAt.set(0);
            start=SystemClock.elapsedRealtimeNanos();
            check(space.performAction(AccessibilityNodeInfo.ACTION_CLICK),"benchmark candidate accepted");
            expect(host.first,"你好","benchmark actual Chinese commit");
            long commitTime=changedAt.get()-start;
            check(firstTime>0 && commitTime>0,"benchmark monotonic timestamps");
            if(i>=5) { first.add(firstTime); commit.add(commitTime); }
        }
        org.json.JSONObject data=new org.json.JSONObject();
        data.put("scope","accessibility ACTION_CLICK request to actual host TextWatcher; includes IPC/scheduling; no touch hardware or frame presentation");
        data.put("warmup",5); data.put("first_key_ms",stats(first)); data.put("candidate_commit_ms",stats(commit));
        data.put("ime",String.valueOf(android.provider.Settings.Secure.getString(host.getContentResolver(),"default_input_method")));
        try(java.io.FileOutputStream out=getTargetContext().openFileOutput("benchmark.json",0)) {
            out.write(data.toString(2).getBytes(java.nio.charset.StandardCharsets.UTF_8));
        }
        report("PASS BENCHMARK "+data);
    }
    private org.json.JSONObject stats(java.util.List<Long> samples) throws Exception {
        java.util.Collections.sort(samples); double sum=0; for(long value:samples) sum+=value;
        return new org.json.JSONObject().put("samples",samples.size()).put("mean",sum/samples.size()/1e6)
                .put("p50",samples.get((samples.size()-1)/2)/1e6)
                .put("p95",samples.get((int)Math.ceil(samples.size()*.95)-1)/1e6)
                .put("max",samples.get(samples.size()-1)/1e6);
    }
    private void pluginSource(String wanted,String label) {
        check(waitLabel("Buffer "+wanted,true)!=null,label+" retains source in keyboard");
        expect(host.first,"",label+" does not change host");
    }
    private AccessibilityNodeInfo outputNode(AccessibilityNodeInfo node,String prefix) {
        if(node==null) return null;
        CharSequence desc=node.getContentDescription();
        if(IME.contentEquals(node.getPackageName()==null?"":node.getPackageName()) && node.isVisibleToUser()
                && desc!=null && desc.toString().startsWith(prefix)) return node;
        for(int i=0;i<node.getChildCount();i++) { AccessibilityNodeInfo found=outputNode(node.getChild(i),prefix); if(found!=null) return found; }
        return null;
    }
    private AccessibilityNodeInfo pluginOutput(String name,String status) {
        String prefix="插件输出："+name+" · "+status+" · ";
        long until=SystemClock.uptimeMillis()+("comet-live".equals(arguments.getString("mode"))?65000:15000);
        do {
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
                AccessibilityNodeInfo value=outputNode(window.getRoot(),prefix); if(value!=null) return value;
            }
            SystemClock.sleep(30);
        } while(SystemClock.uptimeMillis()<until);
        throw new AssertionError("Missing plugin output "+prefix);
    }
    private String resultText(AccessibilityNodeInfo output,String name) {
        return output.getContentDescription().toString().substring(("插件输出："+name+" · READY · ").length());
    }
    private void translationDirection(String label) {
        tap("Buffer 设置"); waitLabel("Buffer 插件设置",false); tap("翻译方向："+label);
        for(int i=0;find("返回键盘",false)==null && i<8;i++) {
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) if(scrollChooser(window.getRoot())) break;
            SystemClock.sleep(100);
        }
        tap("返回键盘");
    }
    private AccessibilityNodeInfo pluginSendButton() {
        for(String label:new String[]{"Insert next","Insert","插入"}) {
            AccessibilityNodeInfo node=find(label,false); if(node!=null) return node;
        }
        return null;
    }
    private void tapPluginSend() {
        AccessibilityNodeInfo send=pluginSendButton();
        check(send!=null && send.isEnabled() && send.performAction(AccessibilityNodeInfo.ACTION_CLICK),"send complete plugin output");
        SystemClock.sleep(120);
    }
    private void pluginsContract() throws Exception {
        String[] names={"翻译","快问","润色","作诗","画画"};
        focusAny(host.first); layout("26"); pinyin();
        tap("Buffer off"); type("nihao"); tap("Space"); pluginSource("你好","initial Buffer");
        tap("Buffer 插件：翻译"); translationDirection("自动中英"); tap("Buffer 插件：翻译");
        for(String name:names) {
            tap("Buffer 插件："+name);
            pluginSource("你好",name+" selection");
            if(name.equals("翻译")) check(find("执行翻译",false)==null,"live translation has no manual execution button");
            else {
                pluginOutput(name,"IDLE");
                AccessibilityNodeInfo send=pluginSendButton(); check(send!=null && !send.isEnabled(),name+" cannot send source before execution");
                tap("执行"+name);
            }
            AccessibilityNodeInfo output=pluginOutput(name,"READY"); String generated=resultText(output,name);
            check(!generated.isEmpty() && !generated.equals("你好"),name+" returns generated output");
            if(!name.equals("翻译")) check(generated.contains("Mock"),name+" visibly labels synthetic AI output");
            else {
                check(generated.toLowerCase(java.util.Locale.ROOT).contains("hello"),"dictionary hello entry arrives automatically");
                check(find("执行翻译",false)==null,"ready translation has no manual execution button");
            }
            pluginSource("你好",name+" result keeps captured source until Send");
            android.graphics.Rect top=new android.graphics.Rect(),bottom=new android.graphics.Rect();
            output.getBoundsInScreen(top); waitLabel("Buffer 你好",true).getBoundsInScreen(bottom);
            check(top.bottom<=bottom.top,name+" result above source");
            tapPluginSend(); expect(host.first,generated,name+" sends result exactly once");
            waitLabel("Buffer ",true); pluginOutput(name,"IDLE");
            tap("Enter"); expectStable(host.first,generated,name+" empty Return cannot resend result");
            focus(host.first); tap("Buffer 插件："+name); type("nihao"); tap("Space"); pluginSource("你好",name+" new target source");
            tap("Buffer 插件："+name); // ordinary Buffer preserves source
        }
        focus(host.first); tap("Buffer 插件：翻译"); tap("中英切换"); type("hello");
        String reverse=resultText(pluginOutput("翻译","READY"),"翻译");
        check("你好".equals(reverse),"dictionary English to Chinese on real keyboard");
        tap("123"); tap("!"); tap("ABC"); pluginSource("hello!","edited live source stays local");
        reverse=resultText(pluginOutput("翻译","READY"),"翻译");
        check("你好!".equals(reverse),"new edit replaces the old live output automatically");
        translationDirection("英 → 中"); pluginSource("hello!","direction change keeps source");
        reverse=resultText(pluginOutput("翻译","READY"),"翻译"); tapPluginSend(); expect(host.first,reverse,"reverse dictionary Send");
        focus(host.first); tap("Buffer 插件：翻译"); type("hello"); pluginOutput("翻译","READY");
        focus(host.second); expectStable(host.second,"","target switch never adopts live translation");
        check(findLabel("Buffer hello",true)==null,"target switch clears old live source");
        check(!waitButton("Buffer off").isSelected(),"target switch clears live Buffer authority");
        focus(host.first); tap("Buffer 插件：翻译"); type("rimeszzunknown"); pluginOutput("翻译","ERROR");
        pluginSource("rimeszzunknown","uncovered source preserved"); check(!pluginSendButton().isEnabled(),"uncovered translation cannot Send");
        check(waitButton("重试翻译").isEnabled(),"failed live translation has explicit retry");
        tap("重试翻译"); pluginOutput("翻译","ERROR");
        pluginSource("rimeszzunknown","retry preserves uncovered source and never sends it");
        focus(host.first); tap("Buffer 插件：翻译"); type("hello");
        getUiAutomation().performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK);
        waitKeyboardHidden(); SystemClock.sleep(650);
        focus(host.second); expectStable(host.second,"","hiding retires pending live translation");
        check(!waitButton("Buffer off").isSelected(),"hidden live source is not restored on another field");
        focus(host.first); tap("Buffer 插件：翻译"); translationDirection("自动中英"); focus(host.first); pinyin();
        // Return settles unfinished code first, executes only on a later clean Return.
        focus(host.first); tap("Buffer 插件：快问"); type("ni"); tap("Enter"); pluginSource("ni","raw Return"); pluginOutput("快问","IDLE");
        tap("Enter"); String answer=resultText(pluginOutput("快问","READY"),"快问");
        type("a"); pluginOutput("快问","IDLE");
        check(!pluginSendButton().isEnabled(),"editing invalidates generated output");
        tap("Enter"); pluginSource("nia","updated source");
        tap("执行快问"); focus(host.second); SystemClock.sleep(700);
        expect(host.first,"","old host receives no canceled result"); expectStable(host.second,"","new host rejects late result");
        check(!waitButton("Buffer off").isSelected(),"new target Buffer off");
        tap("Buffer 插件：快问"); type("nihao"); tap("Space"); tap("执行快问");
        AccessibilityNodeInfo cancel=find("取消执行",false);
        if(cancel!=null && cancel.isEnabled()) { check(cancel.performAction(AccessibilityNodeInfo.ACTION_CLICK),"cancel running mock"); SystemClock.sleep(700); pluginOutput("快问","IDLE"); pluginSource("你好","explicit cancellation preserves source"); }
        else throw new AssertionError("Mock stream must expose cancellation");
        tap("执行快问"); getUiAutomation().performGlobalAction(android.accessibilityservice.AccessibilityService.GLOBAL_ACTION_BACK); waitKeyboardHidden(); SystemClock.sleep(700);
        expectStable(host.second,"","hidden keyboard cannot send late output");
        focus(host.privateInput);
        for(String name:names) check(find("Buffer 插件："+name,false)==null,"private denies plugin "+name);
        check(!waitLabel("Buffer off",true).isEnabled(),"private denies Buffer"); expect(host.privateInput,"","private field unchanged");
        focus(host.first); layout("26"); report("PASS local dictionary, four OpenAI-format mock plugins, source/output isolation, exact Send, editing/cancel/hide/target/private guards");
    }
    /** Explicit live mode; the IME's encrypted test profile must already be configured. */
    private void cometDeniedContract() throws Exception {
        touchHostField(); focusAny(host.first); layout("26"); pinyin(); tap("中英切换");
        tap("Buffer 插件：快问"); type("hello"); pluginSource("hello","live denied request");
        tap("执行快问");
        AccessibilityNodeInfo output=pluginOutput("快问","ERROR");
        check(output.getContentDescription().toString().contains("当前地区"),"actual region restriction shown in keyboard");
        pluginSource("hello","region restriction");
        check(!pluginSendButton().isEnabled(),"failed online result cannot send");
        android.graphics.Bitmap image=getUiAutomation().takeScreenshot();
        if(image!=null) try(java.io.FileOutputStream out=getTargetContext().openFileOutput("comet-region-error.png",0)) {
            image.compress(android.graphics.Bitmap.CompressFormat.PNG,100,out); image.recycle();
        }
        type("a"); pluginOutput("快问","IDLE"); pluginSource("helloa","editing after failed request");
        focus(host.second); expectStable(host.second,"","failed result cannot reach a new target");
        focus(host.privateInput); check(find("Buffer 插件：快问",false)==null,"private field denies online AI");
        onlinePasswordGuard(); touchHostField(); focus(host.first);
        report("PASS live CometAPI region restriction; source retained, Send disabled, editing/target/private/password guards");
    }
    private void cometLiveContract() throws Exception {
        touchHostField(); focusAny(host.first); layout("26"); pinyin(); tap("中英切换");
        String[][] cases={{"快问","why is the sky blue"},{"润色","we is happy today"},{"翻译","good morning"}};
        for(String[] test:cases) {
            focus(host.first); tap("Buffer 插件："+test[0]);
            for(char c:test[1].toCharArray()) { if(c==' ') tap("Space"); else tap(String.valueOf(c)); }
            pluginSource(test[1],test[0]+" source");
            check(!pluginSendButton().isEnabled(),"online result cannot send before completion");
            long start=SystemClock.elapsedRealtime(); tap("执行"+test[0]);
            String generated=resultText(pluginOutput(test[0],"READY"),test[0]);
            check(!generated.isEmpty() && !generated.contains("Mock") && !generated.equals(test[1]),"real generated result "+test[0]);
            if(test[0].equals("润色")) check(generated.toLowerCase(java.util.Locale.ROOT).contains("are happy"),"model fixes English agreement");
            if(test[0].equals("翻译")) check(generated.codePoints().anyMatch(c -> Character.UnicodeScript.of(c)==Character.UnicodeScript.HAN),"AI returns Chinese translation");
            pluginSource(test[1],test[0]+" before send");
            if(test[0].equals("润色")) {
                android.graphics.Bitmap image=getUiAutomation().takeScreenshot();
                if(image!=null) try(java.io.FileOutputStream out=getTargetContext().openFileOutput("comet-live.png",0)) {
                    image.compress(android.graphics.Bitmap.CompressFormat.PNG,100,out); image.recycle();
                }
            }
            tapPluginSend(); expect(host.first,generated,"exact live output inserted once");
            tap("Enter"); expectStable(host.first,generated,"live output not duplicated");
            report("LIVE "+test[0]+" elapsedMs="+(SystemClock.elapsedRealtime()-start)+" output="+generated);
        }
        focus(host.first); tap("Buffer 插件：快问"); type("hello"); tap("执行快问");
        tap("取消执行"); SystemClock.sleep(1200); pluginOutput("快问","IDLE"); pluginSource("hello","live cancellation");
        check(!pluginSendButton().isEnabled(),"cancelled stream cannot send");
        tap("执行快问"); focus(host.second); expectStable(host.second,"","late reply cannot reach a new target");
        SystemClock.sleep(2500); expect(host.first,"","old target unchanged"); expect(host.second,"","new target unchanged after network completion window");
        focus(host.privateInput); check(find("Buffer 插件：快问",false)==null,"private field denies online AI");
        onlinePasswordGuard(); touchHostField(); focus(host.first);
        report("PASS live configured-provider quick question, polish, translation, manual Send, cancellation, target loss and private/password guards");
    }
    private void touchHostField() {
        android.graphics.Rect rect=new android.graphics.Rect();
        runOnMainSync(() -> host.first.getGlobalVisibleRect(rect));
        check(!rect.isEmpty(),"host field visible before initial physical focus");
        long time=SystemClock.uptimeMillis();
        android.view.MotionEvent down=android.view.MotionEvent.obtain(time,time,android.view.MotionEvent.ACTION_DOWN,rect.exactCenterX(),rect.exactCenterY(),0);
        android.view.MotionEvent up=android.view.MotionEvent.obtain(time,time+70,android.view.MotionEvent.ACTION_UP,rect.exactCenterX(),rect.exactCenterY(),0);
        down.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN); up.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
        try { check(getUiAutomation().injectInputEvent(down,true) && getUiAutomation().injectInputEvent(up,true),"initial physical editor tap opens IME"); }
        finally { down.recycle(); up.recycle(); }
    }
    private void onlinePasswordGuard() {
        // An OEM secure keyboard can replace RIMES here; requiring RIMES's q key is incorrect.
        runOnMainSync(() -> { host.password.setText(""); host.focus(host.password); getTargetContext().getSystemService(InputMethodManager.class).restartInput(host.password); });
        SystemClock.sleep(700);
        AtomicReference<Boolean> focused=new AtomicReference<>(false);
        runOnMainSync(() -> focused.set(host.password.hasFocus()));
        check(focused.get(),"actual password editor focused");
        long until=SystemClock.uptimeMillis()+600;
        do { check(find("Buffer 插件：快问",false)==null,"password field exposes no online AI action"); SystemClock.sleep(50); } while(SystemClock.uptimeMillis()<until);
        expect(host.password,"","password editor unchanged by AI");
    }
    /** Never skip a secure-IME substitution: diagnose the live state and retain the assertions. */
    private void passwordContract() {
        runOnMainSync(() -> {host.password.setText("");host.focus(host.password);host.getSystemService(InputMethodManager.class).restartInput(host.password);});
        SystemClock.sleep(700); passwordDiagnostics("before-input");
        AtomicReference<Boolean> focused=new AtomicReference<>(false);
        runOnMainSync(() -> focused.set(host.password.hasFocus() && host.getCurrentFocus()==host.password));
        check(focused.get(),"the actual native password editor has exclusive focus");
        try {
            type("nihao"); expect(host.password,"nihao","password direct input");
            check(find("›",false)==null,"password has no candidates");
            check(find("Buffer off",false)!=null && !find("Buffer off",false).isEnabled(),"password Buffer denied");
            check(find("Buffer 插件：快问",false)==null,"password exposes no AI action");
            passwordDiagnostics("passed"); report("PASS native password direct input, no candidates, Buffer/AI denied; no screenshot required");
        } catch(RuntimeException | AssertionError error) {passwordDiagnostics("failed");throw error;}
    }
    /** Only focus/IME/window metadata; never editor text, labels or secure screenshots. */
    private void passwordDiagnostics(String phase) {
        AtomicReference<String> editor=new AtomicReference<>();
        runOnMainSync(() -> {
            InputMethodManager manager=host.getSystemService(InputMethodManager.class);
            String current="unavailable-before-API34";
            if(android.os.Build.VERSION.SDK_INT>=34) {
                android.view.inputmethod.InputMethodInfo info=manager.getCurrentInputMethodInfo(); current=info==null?"null":info.getId();
            }
            android.view.WindowInsets insets=host.content.getRootWindowInsets();
            String imeVisible=android.os.Build.VERSION.SDK_INT>=30?String.valueOf(insets!=null && insets.isVisible(android.view.WindowInsets.Type.ime())):"unavailable-before-API30";
            editor.set("editorFocused="+host.password.hasFocus()+" currentFocusIsPassword="+(host.getCurrentFocus()==host.password)
                    +" windowFocused="+host.password.hasWindowFocus()+" inputType=0x"+Integer.toHexString(host.password.getInputType())
                    +" immActiveEditor="+manager.isActive(host.password)+" acceptingText="+manager.isAcceptingText()
                    +" imeInsetsVisible="+imeVisible+" currentIme="+current+" configuredDefaultIme="
                    +android.provider.Settings.Secure.getString(host.getContentResolver(),android.provider.Settings.Secure.DEFAULT_INPUT_METHOD));
        });
        report("PASSWORD_DIAGNOSTIC phase="+phase+" "+editor.get());
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            AccessibilityNodeInfo root=window.getRoot(); android.graphics.Rect rect=new android.graphics.Rect();window.getBoundsInScreen(rect);
            report("PASSWORD_WINDOW phase="+phase+" id="+window.getId()+" type="+window.getType()+" active="+window.isActive()
                    +" focused="+window.isFocused()+" bounds="+rect.toShortString()+" rootPackage="+(root==null?"null":root.getPackageName()));
        }
        report("PASSWORD_AX phase="+phase+" rimesQ="+(find("q",false)!=null)+" rimesN="+(find("n",false)!=null));
    }
    private void matrixContract() throws Exception {
        focus(host.first); pinyin(); type("shi");
        String first=candidate(0);
        tap("展开候选"); waitButton("收起候选");
        SystemClock.sleep(300);
        check(findLabel("矩阵候选",true)!=null,"expanded matrix visible in real IME");
        String later=candidate(27);
        tap("收起候选"); tap("Space");
        expect(host.first,first,"preview restores original engine page");
        focus(host.first); type("shi"); tap("展开候选"); SystemClock.sleep(300);
        AccessibilityNodeInfo matrix=findLabel("矩阵候选",true);
        check(matrix!=null && matrix.performAction(AccessibilityNodeInfo.ACTION_SCROLL_FORWARD),"matrix scrolls in real IME");
        SystemClock.sleep(200);
        tap(later); expect(host.first,later,"scrolled later-page candidate commits exact text");
        check(find("收起候选",true)==null,"commit closes the matrix");
        focus(host.first); type("shi"); tap("展开候选"); tap("n");
        check(find("收起候选",true)==null,"editing composition retires old matrix");
        report("PASS matrix expansion, scroll, later-page commit, collapse and edit retirement");
    }

    private void contract() throws Exception {
        focus(host.first); pinyin(); type("nihao");
        check(find("Buffer off",false)!=null && !find("Buffer off",false).isEnabled(),"Buffer toggle locked during composition");
        tap("Space"); expect(host.first,"你好","first candidate"); tap(","); tap("."); expect(host.first,"你好，。","punctuation");
        tap("⌫"); expect(host.first,"你好，","host delete");
        focus(host.first); type("nihaox"); tap("⌫"); tap("Space"); expect(host.first,"你好","composition delete");
        tap("Space"); expect(host.first,"你好 ","space without composition");
        for(int cycle=0;cycle<10;cycle++) {
            focus(host.first);
            for(char key:"nihao".toCharArray()) check(waitButton(String.valueOf(key)).performAction(AccessibilityNodeInfo.ACTION_CLICK),"rapid key");
            tap("Space"); expect(host.first,"你好","rapid ordered results");
        }
        focus(host.first); type("zhongguoren");
        String longChoice=candidate(0); check(longChoice.length()>=3,"long phrase candidate");
        android.graphics.Rect phraseBounds=new android.graphics.Rect(); waitButton(longChoice).getBoundsInScreen(phraseBounds);
        check(phraseBounds.width()>=longChoice.length()*14*host.getResources().getDisplayMetrics().scaledDensity,"long candidate is readable");
        tap(longChoice); expect(host.first,longChoice,"long phrase commit");
        focus(host.first); type("nihao"); tap("↵"); expect(host.first,"nihao","raw Return");
        focus(host.first); type("ni"); tap("拟"); expect(host.first,"拟","non-first candidate");
        focus(host.first); type("ni"); tap("›");
        // Candidate labels are read from the actual current page, never assumed.
        String choice=candidate(0); tap(choice); expect(host.first,choice,"second-page selection");
        focus(host.first); type("ni"); String pageOne=candidate(0); tap("›"); tap("‹");
        check(pageOne.equals(candidate(0)),"previous candidate page"); tap("Space"); expect(host.first,pageOne,"first page restored");
        focus(host.first); tap("拼音"); type("nihk"); tap("Space"); expect(host.first,"你好","natural-code");
        focus(host.first); tap("自然码"); type("wq"); tap("Space"); expect(host.first,"你","Wubi"); tap("五笔");
        focus(host.first); type("ni"); tap("拼音"); expect(host.first,"ni","schema switch settles raw");
        type("nihk"); tap("Space"); expect(host.first,"ni你好","new schema follows settled code"); tap("自然码"); tap("五笔");
        focus(host.first); type("ni"); tap("中"); type("abc"); expect(host.first,"niabc","mode switch settles raw"); tap("英");
        focus(host.first); tap("Buffer off"); type("nihao"); tap("Space"); expect(host.first,"","Buffer never changes host");
        tap("中"); type("ab"); tap("Space"); type("c"); tap("Insert"); expect(host.first,"你好","whole Chinese block");
        tap("Insert all"); expect(host.first,"你好ab c","remaining blocks once"); tap("↵"); expect(host.first,"你好ab c","empty Buffer Return");
        tap("英");
        focus(host.first); tap("Buffer off"); type("nihao"); tap("Space"); type("ni"); tap("拼音");
        expect(host.first,"","schema switch stays inside Buffer"); tap("Insert"); expect(host.first,"你好","schema switch preserves Chinese block");
        tap("Insert all"); expect(host.first,"你好ni","schema switch preserves raw block"); tap("自然码"); tap("五笔");
        focus(host.first); tap("Buffer off"); type("ni"); tap("↵"); expect(host.first,"","raw Return only stages"); tap("↵"); expect(host.first,"ni","second Return sends staged raw");
        focus(host.first); tap("Buffer off"); type("nihao"); tap("Space"); tap("Buffer on"); expect(host.first,"","Buffer off never sends");
        tap("Buffer off"); tap("⌫"); check(find("Insert",false)!=null && !find("Insert",false).isEnabled(),"delete whole block");
        focus(host.first); type("ni"); runOnMainSync(() -> host.focus(host.second)); SystemClock.sleep(500);
        // Android may finish the old EditText composition before revoking its InputConnection.
        // Its visible raw code can remain in that old editor; the IME must never replay it.
        check(read(host.first).isEmpty() || read(host.first).equals("ni"),"old editor has no extra commit");
        expect(host.second,"","new field does not receive old results");
        type("nihao"); tap("Space"); expect(host.second,"你好","new field works");
        focus(host.first); runOnMainSync(() -> { host.first.setText("abcd"); host.first.setSelection(1,3); }); SystemClock.sleep(200);
        type("nihao"); tap("Space"); expect(host.first,"a你好d","host selection replacement");
        focus(host.privateInput); type("nihao"); tap("Space"); expect(host.privateInput,"你好","private Chinese");
        check(find("Buffer off",false)!=null && !find("Buffer off",false).isEnabled(),"private Buffer denied");
        if(!"true".equals(arguments.getString("skipPassword"))) {
        passwordContract();
        } else report("PENDING password keyboard automation");
        focus(host.first); pinyin(); type("ni");
        runOnMainSync(() -> host.getSystemService(InputMethodManager.class).hideSoftInputFromWindow(host.first.getWindowToken(),0));
        SystemClock.sleep(400); expect(host.first,"","hide clears composition");
        focus(host.first); type("nihao"); tap("Space"); expect(host.first,"你好","reopen");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)); SystemClock.sleep(700);
        check(host.getResources().getConfiguration().orientation==android.content.res.Configuration.ORIENTATION_LANDSCAPE,"actual landscape configuration");
        focus(host.first); pinyin(); tap("Buffer off"); type("nihao"); tap("Space");
        expect(host.first,"","landscape Buffer isolation"); tap("Insert all"); expect(host.first,"你好","landscape input");
        android.graphics.Rect bounds=new android.graphics.Rect(); waitButton("↵").getBoundsInScreen(bounds);
        check(bounds.bottom<=host.getResources().getDisplayMetrics().heightPixels,"landscape Return visible");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT)); SystemClock.sleep(500);
        check(host.getResources().getConfiguration().orientation==android.content.res.Configuration.ORIENTATION_PORTRAIT,"actual portrait configuration");
        webContract(); report("PASS native host, candidates, schemes, Buffer, private fields, selection, hide, orientation and WebView");
    }
    private String candidate(int index) {
        String prefix="候选"+(index+1)+" ";
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) {
            String result=candidateLabel(window.getRoot(),prefix); if(result!=null) return result;
        }
        throw new AssertionError("candidate missing");
    }
    private String candidateLabel(AccessibilityNodeInfo node,String prefix) {
        if(node==null) return null;
        String desc=String.valueOf(node.getContentDescription());
        if(desc.startsWith(prefix)) return String.valueOf(node.getText());
        for(int i=0;i<node.getChildCount();i++) { String found=candidateLabel(node.getChild(i),prefix); if(found!=null) return found; }
        return null;
    }
    private String js(String code) throws Exception {
        java.util.concurrent.ArrayBlockingQueue<String> result=new java.util.concurrent.ArrayBlockingQueue<>(1);
        runOnMainSync(() -> host.web.evaluateJavascript(code,result::add));
        String value=result.poll(5,java.util.concurrent.TimeUnit.SECONDS); check(value!=null,"WebView response"); return value;
    }
    private void webContract() throws Exception {
        runOnMainSync(() -> { host.first.setVisibility(android.view.View.GONE); host.second.setVisibility(android.view.View.GONE); host.password.setVisibility(android.view.View.GONE); host.privateInput.setVisibility(android.view.View.GONE); host.web.requestFocus(); });
        SystemClock.sleep(300); js("document.getElementById('first').focus()");
        runOnMainSync(() -> host.getSystemService(InputMethodManager.class).showSoftInput(host.web,InputMethodManager.SHOW_IMPLICIT));
        waitButton("q"); pinyin(); type("nihao"); tap("Space"); check("\"你好\"".equals(js("document.getElementById('first').value")),"WebView Chinese commit");
        tap("Buffer off"); type("nihao"); tap("Space"); check("\"你好\"".equals(js("document.getElementById('first').value")),"WebView Buffer isolated");
        tap("Insert all"); check("\"你好你好\"".equals(js("document.getElementById('first').value")),"WebView exact insertion");
        js("document.getElementById('second').focus()"); SystemClock.sleep(200); type("nihao"); tap("Space");
        check("\"你好\"".equals(js("document.getElementById('second').value")),"WebView target switch");
    }
    private void focusAny(EditText field) {
        runOnMainSync(() -> { field.setText(""); host.focus(field); getTargetContext().getSystemService(InputMethodManager.class).restartInput(field); });
        long deadline=SystemClock.uptimeMillis()+5000;
        while(petButton()==null && find("键位布局",false)==null && SystemClock.uptimeMillis()<deadline) SystemClock.sleep(30);
        check(petButton()!=null || find("键位布局",false)!=null,"keyboard appearance entry available"); SystemClock.sleep(400);
    }
    private void layout(String value) { tap("键位布局"); tap("布局 "+value+" 键"); tap("键位布局"); }
    private void nine(String code) {
        String[] labels={"ABC","DEF","GHI","JKL","MNO","PQRS","TUV","WXYZ"};
        for(char c:code.toCharArray()) tap("九键 "+c+" "+labels[c-'2']);
    }
    private android.graphics.Rect bounds(String label) {
        android.graphics.Rect result=new android.graphics.Rect(); waitButton(label).getBoundsInScreen(result); return result;
    }
    private void touch(String label) {
        if(label.equals("清空 Buffer") && petButton()!=null && find(label,false)==null) { bufferPanelAction(label); return; }
        android.graphics.Rect rect=bounds(label); long time=SystemClock.uptimeMillis();
        android.view.MotionEvent down=android.view.MotionEvent.obtain(time,time,android.view.MotionEvent.ACTION_DOWN,rect.exactCenterX(),rect.exactCenterY(),0);
        android.view.MotionEvent up=android.view.MotionEvent.obtain(time,time+60,android.view.MotionEvent.ACTION_UP,rect.exactCenterX(),rect.exactCenterY(),0);
        down.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN); up.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
        try {
            check(getUiAutomation().injectInputEvent(down,true),"touch down "+label);
            check(getUiAutomation().injectInputEvent(up,true),"touch up "+label);
        } finally { down.recycle(); up.recycle(); }
        SystemClock.sleep(120);
    }
    private void screenshot(String name) throws Exception {
        android.graphics.Bitmap bitmap=getUiAutomation().takeScreenshot();
        check(bitmap!=null,"layout screenshot available");
        try(java.io.FileOutputStream out=getTargetContext().openFileOutput("layout-"+name+".png",0)) {
            check(bitmap.compress(android.graphics.Bitmap.CompressFormat.PNG,100,out),"layout screenshot saved");
        }
    }
    private void letterGeometry() {
        android.graphics.Rect q=bounds("q"),a=bounds("a"),z=bounds("z"),space=bounds("Space"),shift=bounds("⇧"),del=bounds("⌫");
        for(char c:"qwertyuiopasdfghjklzxcvbnm".toCharArray()) {
            android.graphics.Rect key=bounds(String.valueOf(c));
            check(Math.abs(key.width()-q.width())<=1,"equal width: "+c);
            check(Math.abs(key.height()-q.height())<=1,"equal height: "+c);
            check(key.left>=0 && key.right<=host.getResources().getDisplayMetrics().widthPixels,"key within display: "+c);
        }
        check(a.left>q.left && z.left>a.left,"staggered equal-width rows");
        check(space.width()>=q.width()*3,"seven-key footer keeps Space at least three letter cells wide");
        check(Math.abs(space.exactCenterX()-(q.left+bounds("p").right)/2f)<=host.getResources().getDisplayMetrics().density,"Space is centered within the QWERTY letter range");
        check(Math.abs(shift.top-z.top)<=1 && Math.abs(del.top-z.top)<=1,"Shift and Delete flank third row");
        check(bounds("Enter").bottom<=host.getResources().getDisplayMetrics().heightPixels,"Return above system navigation");
    }
    private void layoutContract() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        letterGeometry(); screenshot("qwerty");
        for(char c:"nihao".toCharArray()) touch(String.valueOf(c)); touch("Space");
        expect(host.first,"你好","real touch coordinates commit once"); focus(host.first);
        tap("键位布局"); screenshot("appearance"); tap("配色 犀牛"); tap("键位布局"); screenshot("rhino");
        type("nihao"); screenshot("candidates"); tap("Space"); expect(host.first,"你好","new QWERTY candidate");
        focus(host.first); tap("Buffer off"); type("nihao"); tap("Space");
        layout("9"); expect(host.first,"","layout switch preserves isolated Buffer");
        check(waitButton("Enter").isSelected(),"confirmed Buffer Return has active cap");
        screenshot("nine-buffer"); nine("64426"); tap("Space"); expect(host.first,"","nine-key confirmed text stays in Buffer");
        tap("Insert all"); expect(host.first,"你好你好","both layouts retain complete blocks");
        focusAny(host.first); nine("64"); tap("选拼音"); screenshot("spelling"); tap("拼音 ni"); nine("426");
        tap("选拼音"); tap("拼音 hao"); screenshot("nine-composition"); tap("Space"); expect(host.first,"你好","nine-key spelling constraints");
        focusAny(host.first); nine("64"); tap("选拼音"); tap("拼音 ni"); tap("⌫"); nine("426"); tap("Space");
        expect(host.first,"你好","nine-key delete unpins syllable");
        focusAny(host.first); nine("64"); layout("26"); expect(host.first,"64","layout switch settles unconfirmed raw code");
        type("hao"); tap("Space"); expect(host.first,"64好","26-key restored engine");
        focus(host.first); tap("123"); type("123"); screenshot("numbers"); tap("符号页"); tap("#"); screenshot("symbols");
        tap("ABC"); expect(host.first,"123#","numeric and symbol pages");
        tap("表情"); screenshot("emoji"); tap("😀"); expect(host.first,"123#😀","non-BMP emoji"); tap("⌫"); expect(host.first,"123#","emoji code-point delete"); tap("表情");
        tap("键位布局"); tap("配色 原生"); tap("键位布局");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)); SystemClock.sleep(800);
        focus(host.first); pinyin(); letterGeometry(); screenshot("landscape");
        type("nihao"); tap("Space"); expect(host.first,"你好","landscape QWERTY");
        layout("9"); focusAny(host.first); nine("64426"); tap("Space"); expect(host.first,"你好","landscape nine-key"); screenshot("nine-landscape");
        layout("26");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT)); SystemClock.sleep(600);
        focus(host.first); letterGeometry(); screenshot("portrait-restored");
        report("PASS keyboard layouts, equal touch geometry, theme, spelling, emoji, Buffer and orientation");
    }
    private void inject(long downTime,int action,String... labels) {
        android.graphics.Rect[] rectangles=new android.graphics.Rect[labels.length];
        for(int i=0;i<labels.length;i++) rectangles[i]=bounds(labels[i]);
        inject(downTime,action,rectangles);
    }
    private void inject(long downTime,int action,android.graphics.Rect... rectangles) {
        android.view.MotionEvent.PointerProperties[] properties=new android.view.MotionEvent.PointerProperties[rectangles.length];
        android.view.MotionEvent.PointerCoords[] coordinates=new android.view.MotionEvent.PointerCoords[rectangles.length];
        for(int i=0;i<rectangles.length;i++) {
            properties[i]=new android.view.MotionEvent.PointerProperties(); properties[i].id=i; properties[i].toolType=android.view.MotionEvent.TOOL_TYPE_FINGER;
            coordinates[i]=new android.view.MotionEvent.PointerCoords(); android.graphics.Rect r=rectangles[i];
            coordinates[i].x=r.exactCenterX(); coordinates[i].y=r.exactCenterY(); coordinates[i].pressure=1; coordinates[i].size=1;
        }
        android.view.MotionEvent event=android.view.MotionEvent.obtain(downTime,SystemClock.uptimeMillis(),action,rectangles.length,properties,coordinates,0,0,1,1,0,0,android.view.InputDevice.SOURCE_TOUCHSCREEN,0);
        try { boolean accepted=getUiAutomation().injectInputEvent(event,true);
            if(!accepted && allowStaleRejection) report("InputDispatcher rejected retired stream event "+action);
            check(accepted || allowStaleRejection,"real multi-touch event "+action); } finally { event.recycle(); }
        SystemClock.sleep(40);
    }
    private void finalRightUp(long downTime,String label) {
        finalRightUp(downTime,bounds(label));
    }
    private void finalRightUp(long downTime,android.graphics.Rect r) {
        android.view.MotionEvent.PointerProperties property=new android.view.MotionEvent.PointerProperties(); property.id=1; property.toolType=android.view.MotionEvent.TOOL_TYPE_FINGER;
        android.view.MotionEvent.PointerCoords coord=new android.view.MotionEvent.PointerCoords(); coord.x=r.exactCenterX(); coord.y=r.exactCenterY(); coord.pressure=1; coord.size=1;
        android.view.MotionEvent event=android.view.MotionEvent.obtain(downTime,SystemClock.uptimeMillis(),android.view.MotionEvent.ACTION_UP,1,new android.view.MotionEvent.PointerProperties[]{property},new android.view.MotionEvent.PointerCoords[]{coord},0,0,1,1,0,0,android.view.InputDevice.SOURCE_TOUCHSCREEN,0);
        try { check(getUiAutomation().injectInputEvent(event,true),"last right finger lift"); } finally { event.recycle(); } SystemClock.sleep(120);
    }
    private void chord(String leftStart,String leftEnd,String right) {
        long down=SystemClock.uptimeMillis();
        inject(down,android.view.MotionEvent.ACTION_DOWN,leftStart);
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),leftStart,right);
        inject(down,android.view.MotionEvent.ACTION_MOVE,leftEnd,right);
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,leftEnd,right);
        finalRightUp(down,right);
    }
    private void chooseChord(boolean split) { tap("键位布局"); tap(split?"布局 分体并击":"布局 正交并击"); tap("键位布局"); waitButton("D"); }
    /** New community feedback contract; existing modes retain their independent assertions. */
    private void communityContract() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        int start=assertions;
        type("nihao"); waitButton("你好"); touch("，"); expect(host.first,"你好，","footer comma confirms current Chinese before punctuation");
        type("nihao"); waitButton("你好"); touch("。"); expect(host.first,"你好，你好。","footer period confirms current Chinese before punctuation");
        report("COMMUNITY native punctuation checks="+(assertions-start)); start=assertions;

        focusAny(host.first); layout("9");
        android.graphics.Rect abc=bounds("九键 2 ABC"),space=bounds("Space"),del=bounds("Delete"),enter=bounds("Enter");
        check(Math.abs(space.height()-abc.height())<=2,"nine-key Space has one full row");
        check(space.width()>=abc.width()*2.8,"nine-key Space spans the central three columns");
        check(Math.abs(space.exactCenterX()-(abc.left+abc.width()/2f))<=abc.width()*0.55,"nine-key Space is centered on the numeric grid");
        check(del.height()>=abc.height()*1.9 && Math.abs(del.height()-enter.height())<=2,"nine-key Delete and Return each span two rows");
        check(Math.abs(del.top-abc.top)<=2 && del.bottom<=enter.top+2,"nine-key Delete owns the upper two right-hand rows");
        nine("64426"); expect(host.first,"ni'hao","nine-key host composing text is phonetic, not keypad digits");
        communityScreenshot("native-nine-key");
        touch("你好"); expect(host.first,"你好","nine-key real candidate touch commits Chinese");
        report("COMMUNITY nine-key geometry/composition checks="+(assertions-start)); start=assertions;

        focusAny(host.first); layout("26"); pinyin(); tap("Buffer off");
        type("nihao"); tap("Space"); type("ni"); waitLabel("Buffer 你好ni",true);
        expect(host.first,"","Buffer composing and confirmed text remain isolated");
        touch("清空 Buffer"); waitLabel("Buffer ",true); expectStable(host.first,"","explicit panel Clear never mutates the host");
        check(pluginSendButton()!=null && !pluginSendButton().isEnabled(),"Clear disables delivery of discarded blocks");
        check(find("你好",false)==null,"Clear removes stale composition candidates");
        type("nihao"); tap("Space"); touch("Insert"); expect(host.first,"你好","fresh Buffer sends without discarded old content");
        waitLabel("Buffer ",true); tap("Enter"); expectStable(host.first,"你好","empty Buffer Return cannot replay cleared content");
        report("COMMUNITY explicit panel Buffer clear checks="+(assertions-start)); start=assertions;

        communityHeldDelete(); communityBufferDelete(); communityHeldTarget();
        report("COMMUNITY held native/codepoint/Buffer/target checks="+(assertions-start)); start=assertions;

        focusAny(host.first); pinyin(); type("nihao"); waitButton("你好"); communitySwipeNumber("q");
        expect(host.first,"你好1","hold/up number confirms Chinese and inserts a literal digit rather than candidate selection");
        focusAny(host.first); tap("Buffer off"); type("nihao"); waitButton("你好"); communitySwipeNumber("q");
        waitLabel("Buffer 你好1",true); expect(host.first,"","literal swipe number remains in Buffer");
        touch("Insert"); expect(host.first,"你好1","Buffer delivers the literal swipe result once");
        report("COMMUNITY real QWERTY swipe checks="+(assertions-start)); start=assertions;

        focusAny(host.first); pinyin(); communityFooter(); communityScreenshot("portrait");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)); SystemClock.sleep(800);
        check(host.getResources().getConfiguration().orientation==android.content.res.Configuration.ORIENTATION_LANDSCAPE,"community landscape configuration applied");
        focusAny(host.first); pinyin(); communityFooter(); communityScreenshot("landscape");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT)); SystemClock.sleep(700);
        check(host.getResources().getConfiguration().orientation==android.content.res.Configuration.ORIENTATION_PORTRAIT,"community portrait restored");
        focusAny(host.first); pinyin(); communityIdleShortcuts(); communityScreenshot("idle-shortcuts");
        report("COMMUNITY visible footer/idle shortcut checks="+(assertions-start)); start=assertions;

        communityWeb();
        report("COMMUNITY WebView checks="+(assertions-start));
        chooseChord(false); communityScreenshot("orthogonal-chord");
        chooseChord(true); communityScreenshot("split-chord"); layout("26");
        report("PASS COMMUNITY real native/WebView composition, fixed punctuation, nine-key phonetics/geometry, Clear, held deletion, literal swipe, footer pixels/touches, sampled stable idle plugin pixels");
    }
    /** Actual EditorInfo classes, not a text field with a numeric key page. */
    private void closureFields() throws Exception {
        focusAny(host.first); layout("9"); pinyin();
        try {
            for(String selected:new String[]{"9","26"}) {
                layout(selected);
                for(int kind:new int[]{android.text.InputType.TYPE_CLASS_NUMBER,android.text.InputType.TYPE_CLASS_PHONE}) {
                    runOnMainSync(() -> { host.second.setInputType(kind); host.second.setText(""); host.focus(host.second);
                        host.getSystemService(InputMethodManager.class).restartInput(host.second); });
                    waitButton("6"); type("64426"); expect(host.second,"64426","NUMBER/PHONE commits literal digits");
                    java.util.concurrent.atomic.AtomicInteger composing=new java.util.concurrent.atomic.AtomicInteger();
                    runOnMainSync(() -> composing.set(android.view.inputmethod.BaseInputConnection.getComposingSpanStart(host.second.getText())));
                    check(composing.get()==-1,"NUMBER/PHONE has no composing span");
                    check(find("你好",false)==null && find("九键 2 ABC",false)==null,"NUMBER/PHONE does not show Chinese candidates or phonetic keypad");
                    check(find("Buffer 插件：翻译",false)==null,"NUMBER/PHONE hides plugin shortcuts");
                    AccessibilityNodeInfo buffer=find("Buffer off",false);
                    check(buffer==null || !buffer.isEnabled(),"NUMBER/PHONE denies Buffer activation");
                    touch("Delete"); expect(host.second,"6442","NUMBER/PHONE direct delete remains usable");
                    // Do not reselect layout when returning to a text editor.
                    focusAny(host.first);
                    if(selected.equals("9")) { waitButton("九键 2 ABC"); nine("64426"); expect(host.first,"ni'hao","text restores nine-key phonetic composition"); }
                    else { waitButton("q"); type("nihao"); }
                    tap("Space"); expect(host.first,"你好","text restores selected Chinese layout after numeric target");
                    expectStable(host.second,"6442","retired numeric target receives no Chinese replay");
                    report("CLOSURE_FIELDS layout="+selected+" inputType="+kind+" direct=true composing=false restored=true");
                }
            }
        } finally {
            runOnMainSync(() -> host.second.setInputType(android.text.InputType.TYPE_CLASS_TEXT));
            focusAny(host.first); layout("26");
        }
        report("PASS CLOSURE_FIELDS real NUMBER/PHONE EditorInfo and preserved nine-key/QWERTY preferences");
    }
    /** Records actual IME presentation; only ROI statistics may enter public evidence. */
    private void deleteVideoContract() throws Exception {
        boolean buffered=Boolean.parseBoolean(arguments.getString("buffered","false"));
        focusAny(host.first); layout("26"); pinyin();
        if(buffered) { tap("中"); tap("Buffer off"); for(int block=0;block<24;block++) { tap("a"); tap("Space"); } }
        else runOnMainSync(() -> {host.first.setText("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");host.first.setSelection(host.first.length());});
        SystemClock.sleep(400);
        String[] labels={"Buffer 插件：翻译","Buffer 插件：快问","Buffer 插件：润色"};
        for(int attempt=0;find(labels[0],false)==null && attempt<8;attempt++) {
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) if(scrollPluginBar(window.getRoot(),labels[0],AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)) break;
            SystemClock.sleep(80);
        }
        android.graphics.Rect roi=new android.graphics.Rect();for(String label:labels) roi.union(bounds(label));
        android.graphics.Rect[] frames=new android.graphics.Rect[labels.length];for(int i=0;i<labels.length;i++)frames[i]=bounds(labels[i]);
        String state=buffered?"buffer":"idle",path="/data/local/tmp/rimes-delete-"+state+".mp4";
        org.json.JSONObject metadata=new org.json.JSONObject().put("state",state).put("roi",new org.json.JSONArray(new int[]{roi.left,roi.top,roi.width(),roi.height()}));
        report("DELETE_VIDEO_ROI "+metadata.toString());
        try(android.os.ParcelFileDescriptor recording=getUiAutomation().executeShellCommand("screenrecord --bit-rate 12000000 --time-limit 18 "+path)) {
            SystemClock.sleep(1800);
            touch("Delete"); SystemClock.sleep(600);
            for(int i=0;i<8;i++) {touch("Delete");}
            SystemClock.sleep(400);
            android.graphics.Rect key=bounds("Delete");long down=communityDown(key);SystemClock.sleep(communityHoldMillis()+750);communityUp(down,key);
            for(int i=0;i<labels.length;i++) {
                AccessibilityNodeInfo chip=find(labels[i],false);android.graphics.Rect actual=new android.graphics.Rect();
                check(chip!=null && chip.isVisibleToUser() && chip.isEnabled(),"recorded delete keeps visible enabled plugin");chip.getBoundsInScreen(actual);check(frames[i].equals(actual),"recorded delete keeps plugin bounds");
            }
            try(java.io.InputStream input=new android.os.ParcelFileDescriptor.AutoCloseInputStream(recording)) { while(input.read()!=-1) {} }
        }
        try(java.io.FileOutputStream output=getTargetContext().openFileOutput("delete-video-"+state+".json",0)) {output.write(metadata.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));}
        if(buffered) expectStable(host.first,"","recorded Buffer deletion does not mutate host");
        report("PASS DELETE_VIDEO state="+state+" actualIME=true operations=single,8-clicks,held scope=recorded-frames-only");
    }
    private void closurePunctuation() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        type("nihao"); waitButton("你好"); communitySwipeNumber("a");expect(host.first,"你好，","upward comma confirms composing Chinese");
        communitySwipeNumber("s");expect(host.first,"你好，。","upward period uses Chinese mode");
        focusAny(host.first); tap("Buffer off");type("nihao");waitButton("你好");communitySwipeNumber("d");
        waitLabel("Buffer 你好？",true);expect(host.first,"","upward punctuation stays in Buffer");touch("Insert");expect(host.first,"你好？","Buffer punctuation sends exactly once");
        focusAny(host.first);tap("中");communitySwipeNumber("a");communitySwipeNumber("s");expect(host.first,",.","upward marks use English mode");tap("英");
        focusAny(host.first);layout("9");nine("64426");waitButton("你好");communitySwipeNumber("九键 2 ABC");expect(host.first,"你好，","nine-key alternate confirms Chinese and marks");
        focusAny(host.first);layout("26");
        report("PASS CLOSURE_PUNCTUATION QWERTY/nine-key Chinese/English, composition and Buffer");
    }
    /** Raw InputDispatcher streams exercise native split clicks on the actual ordinary IME. */
    private void closureThumbs() {
        focusAny(host.first); layout("26"); pinyin(); tap("中");
        android.graphics.Rect q=bounds("q"),p=bounds("p"),delete=bounds("Delete");
        for(boolean rightFirst:new boolean[]{false,true}) {
            runOnMainSync(() -> host.first.setText(""));
            SystemClock.sleep(400); // Let the host's synthetic selection update retire before DOWN.
            long down=SystemClock.uptimeMillis();
            inject(down,android.view.MotionEvent.ACTION_DOWN,q);
            check(SystemClock.uptimeMillis()-down<200,"second finger arrives before alternate hold deadline");
            inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),q,p);
            inject(down,android.view.MotionEvent.ACTION_POINTER_UP|(rightFirst?1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT:0),q,p);
            if(rightFirst) inject(down,android.view.MotionEvent.ACTION_UP,q); else finalRightUp(down,p);
            expect(host.first,rightFirst?"pq":"qp","ordinary overlapping short taps each commit exactly once");
            expectStable(host.first,rightFirst?"pq":"qp","ordinary overlapping short taps remain committed exactly once");
        }
        runOnMainSync(() -> host.first.setText(""));
        SystemClock.sleep(400);
        long down=SystemClock.uptimeMillis();
        inject(down,android.view.MotionEvent.ACTION_DOWN,q);
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),q,p);
        SystemClock.sleep(700);
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,q,p); finalRightUp(down,p);
        expect(host.first,"qp","split siblings cannot reacquire an alternate after a long hold");
        expectStable(host.first,"qp","split sibling commits remain stable after release");
        runOnMainSync(() -> {host.first.setText("abc");host.first.setSelection(3);});
        SystemClock.sleep(400);
        down=SystemClock.uptimeMillis();
        inject(down,android.view.MotionEvent.ACTION_DOWN,delete);
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),delete,q);
        SystemClock.sleep(communityHoldMillis()+200);
        expect(host.first,"abc","two-finger stream never starts held repeats");
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,delete,q); finalRightUp(down,q);
        expect(host.first,"abq","overlapping ordinary Delete and letter each commit once");
        expectStable(host.first,"abq","overlapping Delete and letter commits remain stable after release");
        runOnMainSync(() -> host.first.setText(""));
        SystemClock.sleep(400);
        down=SystemClock.uptimeMillis();
        inject(down,android.view.MotionEvent.ACTION_DOWN,q);SystemClock.sleep(400);
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),q,p);
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,q,p);finalRightUp(down,p);
        expectStable(host.first,"","second finger cancels an armed alternate without release clicks");
        touch("p");expect(host.first,"p","fresh ordinary stream remains usable after cancellation");
        tap("英");
        report("PASS CLOSURE_THUMBS actual ordinary IME split short taps, both release orders, hold suppression and armed cancellation");
    }
    private long communityDown(android.graphics.Rect rect) {
        long down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,rect); return down;
    }
    private void communityUp(long down,android.graphics.Rect rect) { inject(down,android.view.MotionEvent.ACTION_UP,rect); SystemClock.sleep(200); }
    private long communityHoldMillis() { return android.view.ViewConfiguration.getLongPressTimeout()+300L; }
    private void communitySwipeNumber(String label) {
        android.graphics.Rect origin=bounds(label),end=new android.graphics.Rect(origin);
        end.offset(0,-Math.round(24*host.getResources().getDisplayMetrics().density));
        long down=communityDown(origin); SystemClock.sleep(400);
        inject(down,android.view.MotionEvent.ACTION_MOVE,end); communityUp(down,end);
    }
    private static boolean wellFormedUtf16(CharSequence text) {
        for(int i=0;i<text.length();i++) {
            char value=text.charAt(i);
            if(Character.isHighSurrogate(value)) { if(++i>=text.length() || !Character.isLowSurrogate(text.charAt(i))) return false; }
            else if(Character.isLowSurrogate(value)) return false;
        }
        return true;
    }
    private void communityHeldDelete() {
        focusAny(host.first); pinyin();
        String original="abcdefghijklmnopqrstuvwx😀𠮷Z";
        runOnMainSync(() -> { host.first.setText(original); host.first.setSelection(original.length()); }); SystemClock.sleep(250);
        java.util.concurrent.atomic.AtomicBoolean valid=new java.util.concurrent.atomic.AtomicBoolean(true);
        java.util.concurrent.atomic.AtomicInteger mutations=new java.util.concurrent.atomic.AtomicInteger();
        android.text.TextWatcher watcher=new android.text.TextWatcher() {
            public void beforeTextChanged(CharSequence s,int start,int count,int after) {}
            public void onTextChanged(CharSequence s,int start,int before,int count) { mutations.incrementAndGet(); if(!wellFormedUtf16(s)) valid.set(false); }
            public void afterTextChanged(android.text.Editable value) {}
        };
        runOnMainSync(() -> host.first.addTextChangedListener(watcher));
        try {
            android.graphics.Rect key=bounds("Delete"); long down=communityDown(key);
            SystemClock.sleep(communityHoldMillis()); communityUp(down,key);
            String after=read(host.first);
            check(mutations.get()>=3 && after.length()<original.length(),"real held Delete performs repeated host mutations");
            check(valid.get() && wellFormedUtf16(after),"every native host deletion keeps non-BMP surrogate pairs intact");
            check(original.startsWith(after),"held deletion only removes a suffix");
            expectStable(host.first,after,"held Delete stops after release without late extra deletion");
        } finally { runOnMainSync(() -> host.first.removeTextChangedListener(watcher)); }
    }
    private void communityBufferDelete() {
        focusAny(host.first); pinyin(); tap("Buffer off"); type("nihao"); tap("Space"); waitLabel("Buffer 你好",true);
        touch("Delete"); waitLabel("Buffer ",true); expect(host.first,"","short Delete removes one complete Chinese Buffer block");
        for(int i=0;i<3;i++) { type("nihao"); tap("Space"); }
        waitLabel("Buffer 你好你好你好",true); android.graphics.Rect key=bounds("Delete"); long down=communityDown(key);
        SystemClock.sleep(communityHoldMillis()); communityUp(down,key); waitLabel("Buffer ",true);
        expectStable(host.first,"","held empty Buffer deletion never falls through to host deletion");
        check(pluginSendButton()!=null && !pluginSendButton().isEnabled(),"held Buffer deletion consumes complete blocks");
    }
    private void communityHeldTarget() {
        focusAny(host.first); pinyin(); String original="abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
        runOnMainSync(() -> {host.first.setText(original);host.first.setSelection(original.length());host.second.setText("fresh😀");}); SystemClock.sleep(250);
        android.graphics.Rect key=bounds("Delete"); long down=communityDown(key); SystemClock.sleep(communityHoldMillis());
        check(read(host.first).length()<original.length(),"old target receives authorized held deletions before switching");
        runOnMainSync(() -> {host.focus(host.second);host.second.setSelection(host.second.getText().length());}); SystemClock.sleep(400);
        expect(host.second,"fresh😀","target switch does not adopt old delete timer");
        allowStaleRejection=true;
        try { communityUp(down,key); } finally { allowStaleRejection=false; }
        expectStable(host.second,"fresh😀","held Delete and late UP cannot delete the new target");
        touch("Delete"); expect(host.second,"fresh","a fresh Delete still removes the new target's whole emoji");
    }
    private android.graphics.Rect communityImeBounds() {
        for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) if(window.getType()==AccessibilityWindowInfo.TYPE_INPUT_METHOD) {
            android.graphics.Rect rect=new android.graphics.Rect(); window.getBoundsInScreen(rect); if(!rect.isEmpty()) return rect;
        }
        throw new AssertionError("Community footer requires a visible IME window");
    }
    private void communityFooter() {
        String[] labels={"数字与字母","表情","，","Space","。","中英切换","Enter"};
        android.graphics.Rect window=communityImeBounds(); android.graphics.Bitmap bitmap=getUiAutomation().takeScreenshot();
        check(bitmap!=null,"footer has an actual screen raster");
        try {
            android.graphics.Rect screen=new android.graphics.Rect(0,0,bitmap.getWidth(),bitmap.getHeight());
            float density=host.getResources().getDisplayMetrics().density; int top=-1,bottom=-1,last=-1;
            for(String label:labels) {
                android.graphics.Rect rect=bounds(label);
                check(!rect.isEmpty() && window.contains(rect) && screen.contains(rect),"whole footer cell is inside the visible IME/display: "+label);
                check(top<0 || Math.abs(rect.top-top)<=2 && Math.abs(rect.bottom-bottom)<=2,"footer shares one visible row: "+label);
                check(last<0 || rect.left>=last-2,"footer semantic order: "+label); top=rect.top; bottom=rect.bottom; last=rect.right;
                // OEM display/dark-mode transforms can change an accent swatch (the
                // verified phone renders Apple's blue as #3B82F7). Require the same
                // opaque blue fill in the upper and lower cap, rather than accepting
                // an arbitrary background or adding device-specific colors to a list.
                int accent=label.equals("Enter")?communityAccentFill(bitmap,rect):0;
                int painted=0;
                for(int offset:new int[]{5,7,9}) {
                    int y=Math.max(rect.top,rect.bottom-Math.round(offset*density));
                    for(float fraction:new float[]{0.2f,0.3f,0.7f,0.8f}) {
                        int x=Math.min(rect.right-1,rect.left+Math.round(rect.width()*fraction));
                        int color=bitmap.getPixel(x,y);
                        if(label.equals("Enter")?color==accent:color==0xffffffff || color==0xffabb0ba
                                || color==0xff6e6e6e || color==0xff404040) painted++;
                    }
                }
                check(painted>=6,"footer lower cap is actually painted rather than cropped/covered: "+label);
            }
        } finally { bitmap.recycle(); }
        touch("数字与字母"); waitButton("1"); touch("数字与字母"); waitButton("q");
        touch("表情"); waitButton("😀"); touch("表情"); waitButton("q");
        touch("，"); touch("Space"); touch("。"); expect(host.first,"， 。","footer mark/space centers deliver their visible values");
        touch("中英切换"); touch("q"); expect(host.first,"， 。q","language center switches to direct English"); touch("中英切换");
        java.util.concurrent.atomic.AtomicBoolean returnSeen=new java.util.concurrent.atomic.AtomicBoolean();
        runOnMainSync(() -> host.first.setOnEditorActionListener((view,action,event) -> {returnSeen.set(true);return false;}));
        try { touch("Enter"); check(returnSeen.get() || !"， 。q".equals(read(host.first)),"Return center reaches the real editor action/text path"); }
        finally { runOnMainSync(() -> host.first.setOnEditorActionListener(null)); }
    }
    private int communityAccentFill(android.graphics.Bitmap bitmap,android.graphics.Rect rect) {
        java.util.HashMap<Integer,Integer> counts=new java.util.HashMap<>();
        for(float vertical:new float[]{0.2f,0.25f,0.3f}) for(float horizontal:new float[]{0.2f,0.3f,0.7f,0.8f}) {
            int x=Math.min(rect.right-1,rect.left+Math.round(rect.width()*horizontal));
            int y=Math.min(rect.bottom-1,rect.top+Math.round(rect.height()*vertical));
            int color=bitmap.getPixel(x,y); counts.put(color,counts.getOrDefault(color,0)+1);
        }
        int color=0,count=0;
        for(java.util.Map.Entry<Integer,Integer> sample:counts.entrySet()) if(sample.getValue()>count) {color=sample.getKey();count=sample.getValue();}
        float[] expected=new float[3],actual=new float[3];
        boolean dark=(host.getResources().getConfiguration().uiMode&android.content.res.Configuration.UI_MODE_NIGHT_MASK)==android.content.res.Configuration.UI_MODE_NIGHT_YES;
        android.graphics.Color.colorToHSV(dark?0xff0a84ff:0xff007aff,expected); android.graphics.Color.colorToHSV(color,actual);
        float hue=Math.abs(expected[0]-actual[0]); hue=Math.min(hue,360-hue);
        check(count>=6 && android.graphics.Color.alpha(color)==255 && hue<=15 && actual[1]>=0.65f && actual[2]>=0.7f,
                "Return upper cap matches the opaque blue accent family before lower-edge comparison");
        return color;
    }
    /** Delivery evidence contains only the IME window and synthetic keyboard state. */
    private void communityScreenshot(String name) throws Exception {
        android.graphics.Bitmap full=getUiAutomation().takeScreenshot(); check(full!=null,"community keyboard screenshot available");
        android.graphics.Bitmap keyboard=null;
        try {
            android.graphics.Rect rect=communityImeBounds();
            check(new android.graphics.Rect(0,0,full.getWidth(),full.getHeight()).contains(rect),"community screenshot contains the complete keyboard window");
            keyboard=android.graphics.Bitmap.createBitmap(full,rect.left,rect.top,rect.width(),rect.height());
            try(java.io.FileOutputStream out=getTargetContext().openFileOutput("community-"+name+".png",0)) {
                check(keyboard.compress(android.graphics.Bitmap.CompressFormat.PNG,100,out),"community keyboard screenshot saved: "+name);
            }
        } finally { if(keyboard!=null && keyboard!=full) keyboard.recycle();full.recycle(); }
    }
    private long communityPluginRaster(android.graphics.Rect rect) {
        android.graphics.Bitmap bitmap=getUiAutomation().takeScreenshot(); check(bitmap!=null,"idle shortcut raster available");
        try {
            check(new android.graphics.Rect(0,0,bitmap.getWidth(),bitmap.getHeight()).contains(rect),"shortcut ROI contains only visible keyboard pixels");
            int[] pixels=new int[rect.width()*rect.height()]; bitmap.getPixels(pixels,0,rect.width(),rect.left,rect.top,rect.width(),rect.height());
            java.util.zip.CRC32 crc=new java.util.zip.CRC32();
            for(int pixel:pixels) {crc.update(pixel>>>24);crc.update(pixel>>>16);crc.update(pixel>>>8);crc.update(pixel);}
            return crc.getValue();
        } finally {bitmap.recycle();}
    }
    private void communityIdleShortcuts() {
        runOnMainSync(() -> {host.first.setText("abcdefghijklmnopqrst");host.first.setSelection(20);}); SystemClock.sleep(250);
        String[] labels={"Buffer 插件：翻译","Buffer 插件：快问","Buffer 插件：润色"};
        for(int attempt=0;find(labels[0],false)==null && attempt<8;attempt++) {
            for(AccessibilityWindowInfo window:getUiAutomation().getWindows()) if(scrollPluginBar(window.getRoot(),labels[0],AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD)) break;
            SystemClock.sleep(80);
        }
        android.graphics.Rect[] rectangles=new android.graphics.Rect[labels.length]; android.graphics.Rect roi=new android.graphics.Rect();
        for(int i=0;i<labels.length;i++) {rectangles[i]=bounds(labels[i]);roi.union(rectangles[i]);}
        long baseline=communityPluginRaster(roi);
        for(int step=0;step<8;step++) {
            touch("Delete");
            for(int i=0;i<labels.length;i++) {
                AccessibilityNodeInfo chip=find(labels[i],false); android.graphics.Rect current=new android.graphics.Rect();
                check(chip!=null && chip.isEnabled() && chip.isVisibleToUser(),"idle delete preserves enabled visible shortcut: "+labels[i]);
                chip.getBoundsInScreen(current); check(rectangles[i].equals(current),"idle delete keeps shortcut frame stable: "+labels[i]);
            }
            check(communityPluginRaster(roi)==baseline,"sampled idle plugin pixels/alpha remain unchanged after delete");
        }
        report("COMMUNITY_IDLE_PIXELS samples=9 stable=true scope=plugin-shortcut-ROI-only; no host/body screenshots saved");
    }
    private String communityWebValue(String id) throws Exception {
        Object decoded=new org.json.JSONTokener(js("document.getElementById('"+id+"').value")).nextValue();
        check(decoded instanceof String,"WebView value is a JSON string"); return (String)decoded;
    }
    private void communityWebExpect(String id,String wanted,String label) throws Exception {
        long deadline=SystemClock.uptimeMillis()+4000; String actual;
        do {actual=communityWebValue(id);if(wanted.equals(actual)){check(true,label);return;}SystemClock.sleep(25);} while(SystemClock.uptimeMillis()<deadline);
        throw new AssertionError(label+" expected=["+wanted+"] actual=["+actual+"]");
    }
    private void communityWebReset(String id) throws Exception {
        js("document.getElementById('first').blur();document.getElementById('second').blur();var e=document.getElementById('"+id+"');e.value='';e.focus();");
        runOnMainSync(() -> {host.getSystemService(InputMethodManager.class).restartInput(host.web);host.getSystemService(InputMethodManager.class).showSoftInput(host.web,InputMethodManager.SHOW_IMPLICIT);});
        waitButton("q"); SystemClock.sleep(350); pinyin();
    }
    private void communityWeb() throws Exception {
        runOnMainSync(() -> {host.first.setVisibility(android.view.View.GONE);host.second.setVisibility(android.view.View.GONE);host.password.setVisibility(android.view.View.GONE);host.privateInput.setVisibility(android.view.View.GONE);host.web.requestFocus();});
        SystemClock.sleep(300); communityWebReset("first"); type("nihao"); waitButton("你好"); touch("，");
        communityWebExpect("first","你好，","WebView footer comma confirms Chinese");
        type("nihao"); waitButton("你好"); touch("。"); communityWebExpect("first","你好，你好。","WebView footer period confirms Chinese");
        communityWebReset("first"); type("nihao"); waitButton("你好"); communitySwipeNumber("q");
        communityWebExpect("first","你好1","WebView upward digit is literal and confirms Chinese");
        communityWebReset("first"); tap("Buffer off"); type("nihao"); tap("Space"); type("ni"); waitLabel("Buffer 你好ni",true);
        touch("清空 Buffer"); waitLabel("Buffer ",true); communityWebExpect("first","","WebView Clear preserves untouched host");
        type("nihao"); tap("Space"); touch("Insert"); communityWebExpect("first","你好","WebView fresh Send cannot resurrect cleared blocks");
        type("nihao"); tap("Space"); touch("Delete"); waitLabel("Buffer ",true);
        communityWebExpect("first","你好","WebView short Buffer Delete removes a whole Chinese block without touching host");
        communityWebReset("first"); String original="abcdefghijklmnopqrstuvwx😀𠮷Z";
        js("var e=document.getElementById('first');e.value="+org.json.JSONObject.quote(original)+";e.setSelectionRange(e.value.length,e.value.length);"); SystemClock.sleep(250);
        android.graphics.Rect key=bounds("Delete"); long down=communityDown(key); SystemClock.sleep(communityHoldMillis()); communityUp(down,key);
        String after=communityWebValue("first");
        check(after.codePointCount(0,after.length())<=original.codePointCount(0,original.length())-3 && original.startsWith(after),"WebView held Delete removes multiple complete codepoints");
        check(wellFormedUtf16(after),"WebView held Delete preserves non-BMP integrity"); SystemClock.sleep(600);
        communityWebExpect("first",after,"WebView held Delete stops after release");
        js("var e=document.getElementById('first');e.value='abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz';e.setSelectionRange(e.value.length,e.value.length);"); SystemClock.sleep(250);
        key=bounds("Delete"); down=communityDown(key); SystemClock.sleep(communityHoldMillis());
        check(communityWebValue("first").length()<52,"WebView old field receives authorized held deletions");
        js("var e=document.getElementById('second');e.value='fresh😀';e.focus();e.setSelectionRange(e.value.length,e.value.length);"); SystemClock.sleep(400);
        communityWebExpect("second","fresh😀","WebView focus change retires old held Delete");
        allowStaleRejection=true;
        try {communityUp(down,key);} finally {allowStaleRejection=false;}
        SystemClock.sleep(600); communityWebExpect("second","fresh😀","WebView new field rejects old repeated deletion and late UP");
        touch("Delete"); communityWebExpect("second","fresh","WebView new target accepts a fresh whole-emoji Delete");
        communityWebReset("first"); layout("9"); nine("64426"); communityWebExpect("first","ni'hao","WebView nine-key composition shows phonetic syllables");
        communityScreenshot("webview-nine-key");
        touch("你好"); communityWebExpect("first","你好","WebView nine-key candidate commits actual Chinese");
        layout("26");
    }
    private void nativeTouchContract() throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        runOnMainSync(() -> host.traceConnections=true);
        long down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,"n");
        runOnMainSync(() -> host.focus(host.second)); SystemClock.sleep(400);
        allowStaleRejection=true; inject(down,android.view.MotionEvent.ACTION_UP,"n"); allowStaleRejection=false;
        expectStable(host.second,"","held ordinary key cannot type into a new field");
        type("nihao"); tap("Space"); expect(host.second,"你好","new ordinary clicks remain valid");
        focusAny(host.first); runOnMainSync(() -> host.first.setText("abcd")); SystemClock.sleep(200);
        down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,"n");
        runOnMainSync(() -> host.first.setSelection(1,3)); SystemClock.sleep(300);
        allowStaleRejection=true; inject(down,android.view.MotionEvent.ACTION_UP,"n"); allowStaleRejection=false;
        expectStable(host.first,"abcd","selection retirement rejects old ordinary lift");
        android.graphics.Rect heldKey=bounds("n");
        down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,heldKey);
        expect(host.first,"abcd","ordinary DOWN before hiding does not type");
        runOnMainSync(() -> host.getSystemService(InputMethodManager.class).hideSoftInputFromWindow(host.first.getWindowToken(),0)); waitKeyboardHidden();
        expect(host.first,"abcd","hiding a held key preserves selected host text");
        allowStaleRejection=true; inject(down,android.view.MotionEvent.ACTION_UP,heldKey); allowStaleRejection=false;
        expectStable(host.first,"abcd","hidden keyboard rejects old ordinary lift");
        focusAny(host.first); type("nihao"); tap("Space"); expect(host.first,"你好","fresh target after hiding remains usable");
        report("PASS native held keys, target/selection/hide retirement and fresh accessibility clicks");
    }
    private void chordContract() throws Exception {
        focusAny(host.first); layout("26"); pinyin(); chooseChord(false); screenshot("chord");
        long down=SystemClock.uptimeMillis();
        inject(down,android.view.MotionEvent.ACTION_DOWN,"D");
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),"D","I");
        inject(down,android.view.MotionEvent.ACTION_MOVE,"V","I"); screenshot("chord-held");
        expect(host.first,"","held chord does not preedit or commit");
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,"V","I"); expect(host.first,"","one hand released does not commit");
        finalRightUp(down,"I"); expect(host.first,"ni","both lifted resolve to Natural Code ni");
        check(!waitButton("Enter").isSelected(),"composition Return has neutral cap");
        chord("X","C","K"); waitButton("你好"); tap("Space"); expect(host.first,"你好","real two-thumb ni+hao commits once");
        focusAny(host.first); tap("Buffer off"); chord("D","V","I"); chord("X","C","K"); tap("Space"); expect(host.first,"","confirmed chord word stays in Buffer");
        check(waitButton("Enter").isSelected(),"confirmed chord Buffer Return has active cap");
        screenshot("chord-buffer"); tap("Insert"); expect(host.first,"你好","chord Buffer exact delivery");
        focusAny(host.first); down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,"D");
        inject(down,android.view.MotionEvent.ACTION_POINTER_DOWN|(1<<android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT),"D","V");
        inject(down,android.view.MotionEvent.ACTION_POINTER_UP,"D","V"); finalRightUp(down,"V");
        expect(host.first,"","two fingers on same hand cancel all");
        down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,"D"); inject(down,android.view.MotionEvent.ACTION_CANCEL,"D");
        allowStaleRejection=true; inject(down,android.view.MotionEvent.ACTION_UP,"V"); allowStaleRejection=false; expect(host.first,"","cancelled late lift cannot type");
        down=SystemClock.uptimeMillis(); inject(down,android.view.MotionEvent.ACTION_DOWN,"D");
        runOnMainSync(() -> host.focus(host.second)); SystemClock.sleep(400); allowStaleRejection=true; inject(down,android.view.MotionEvent.ACTION_UP,"V"); allowStaleRejection=false;
        expect(host.second,"","old touch never reaches new target");
        chooseChord(true); chord("D","V","I"); chord("X","C","K"); tap("Space"); expect(host.second,"你好","split chord works"); screenshot("chord-split");
        focusAny(host.privateInput); chord("D","V","I"); chord("X","C","K"); tap("Space"); expect(host.privateInput,"你好","private chord scheme works");
        check(!find("Buffer off",false).isEnabled(),"private chord Buffer disabled");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE)); SystemClock.sleep(700);
        focusAny(host.first); chord("D","V","I"); chord("X","C","K"); tap("Space"); expect(host.first,"你好","landscape chord"); screenshot("chord-landscape");
        runOnMainSync(() -> host.setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_PORTRAIT)); SystemClock.sleep(600);
        focusAny(host.first); layout("26"); pinyin(); type("nihao"); tap("Space"); expect(host.first,"你好","ordinary layout restored");
        report("PASS chord real multi-touch, split/orthogonal, all-fingers release, cancellation, Buffer, privacy, target revocation and rotation");
    }
    private void soak(long seconds) throws Exception {
        focusAny(host.first); layout("26"); pinyin();
        boolean mixed=Boolean.parseBoolean(arguments.getString("chords","false"));
        long start=SystemClock.elapsedRealtime(), next=start+60000; int cycles=0;
        while(SystemClock.elapsedRealtime()-start<seconds*1000) {
            EditText field=(cycles%2==0)?host.first:host.second;
            focusAny(field);
            boolean useChord=mixed && cycles%3==2;
            if(useChord) chooseChord(cycles%2==0); else { layout("26"); pinyin(); }
            if(cycles%2==0) tap("Buffer off");
            if(useChord) { chord("D","V","I"); chord("X","C","K"); } else type("nihao");
            tap("Space");
            if(cycles%2==0) { expect(field,"","soak Buffer isolation"); tap("Insert all"); }
            expect(field,"你好","soak exactly one commit"); tap(",");
            if(cycles%2==0) tap("Insert all");
            expect(field,"你好，","soak punctuation");
            cycles++;
            long now=SystemClock.elapsedRealtime();
            if(now>=next) { report("SOAK elapsed="+((now-start)/1000)+"s cycles="+cycles+" assertions="+assertions); next=now+60000; }
        }
        report("PASS SOAK duration="+((SystemClock.elapsedRealtime()-start)/1000)+"s cycles="+cycles+" mixedChords="+mixed+" no duplicate or cross-field commit");
    }
}
