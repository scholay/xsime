package org.scholay.rimes.android;
import android.app.Instrumentation;
import android.os.Bundle;

/** Exercises the actual packaged JNI library, including modified-UTF-8 traps. */
public final class EngineInstrumentation extends Instrumentation {
    private Bundle arguments;
    private String bufferRenderingResult="";
    private int chordChecks,iconChecks,touchChecks;
    @Override public void onCreate(Bundle arguments) { this.arguments=arguments; super.onCreate(arguments); start(); }
    @Override public void onStart() {
        Bundle result=new Bundle();
        try {
            if(arguments!=null && "app-update-prepare".equals(arguments.getString("mode"))) {
                result.putString("stream", AppUpdateUpgradeFixture.prepare(this,arguments.getString("apk"))); finish(-1,result); return;
            }
            if(arguments!=null && "app-update".equals(arguments.getString("mode"))) {
                AppUpdateContract.run(this); result.putString("stream", "PASS application update contracts and system installation cancellation\n"); finish(-1,result); return;
            }
            if(arguments!=null && "wubi".equals(arguments.getString("mode"))) {
                result.putString("stream",WubiContract.run(this)); finish(-1,result); return;
            }
            if(arguments!=null && "scheme-import".equals(arguments.getString("mode"))) {
                result.putString("stream",SchemeImportContract.run(this,arguments.getString("archive"))); finish(-1,result); return;
            }
            if(arguments!=null && "upgrade-retain-baseline".equals(arguments.getString("mode"))) {
                result.putString("stream",UpgradeDataContract.retainBaseline(this)); finish(-1,result); return;
            }
            if(arguments!=null && ("upgrade-baseline".equals(arguments.getString("mode")) || "upgrade-verify".equals(arguments.getString("mode")))) {
                result.putString("stream",UpgradeDataContract.run(this,"upgrade-verify".equals(arguments.getString("mode")))); finish(-1,result); return;
            }
            if(arguments!=null && "layout-preferences".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS layout preference checks="+KeyboardSettingsLayoutContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "clipboard-store".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS clipboard store checks="+TextClipboardStoreContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "feedback-engine".equals(arguments.getString("mode"))) {
                result.putString("stream",EngineFeedbackContract.run(this)); finish(-1,result); return;
            }
            if(arguments!=null && "community-prepare".equals(arguments.getString("mode"))) {
                CommunitySettingsContract.prepare(this);
                result.putString("stream","PASS community test preferences backed up; learning and remote AI disabled\n"); finish(-1,result); return;
            }
            if(arguments!=null && "community-restore".equals(arguments.getString("mode"))) {
                CommunitySettingsContract.restore(this);
                result.putString("stream","PASS original community test preferences restored\n"); finish(-1,result); return;
            }
            if(arguments!=null && "delete-repeat".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS native delete repeat checks="+DeleteRepeatContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "buffer-hold".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS native Buffer hold checks="+BufferHoldContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "space-touch".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS native space touch checks="+SpaceTouchContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "upward-number".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS upward number checks="+UpwardNumberContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "comet-diagnostics".equals(arguments.getString("mode"))) {
                NetworkAiContract.foreground(this);
                StringBuilder diagnostic=new StringBuilder();
                try { diagnostic.append("DNS addresses=").append(java.net.InetAddress.getAllByName("api.cometapi.com").length).append('\n'); }
                catch(Exception error) { diagnostic.append("DNS ").append(error.getClass().getSimpleName()).append(": ").append(error.getMessage()).append('\n'); }
                for(String agent:new String[]{"", "RIMES-Android"}) {
                    javax.net.ssl.HttpsURLConnection connection=null;
                    try {
                        connection=(javax.net.ssl.HttpsURLConnection)new java.net.URL("https://api.cometapi.com/api/models").openConnection();
                        connection.setInstanceFollowRedirects(false); connection.setConnectTimeout(10000); connection.setReadTimeout(10000);
                        if(!agent.isEmpty()) connection.setRequestProperty("User-Agent",agent);
                        int code=connection.getResponseCode();
                        diagnostic.append("Public HTTPS agent=").append(agent.isEmpty()?"system":agent).append(" status=").append(code).append('\n');
                        if(code!=200 && connection.getErrorStream()!=null) {
                            byte[] bytes=new byte[256]; int count=connection.getErrorStream().read(bytes);
                            if(count>0) diagnostic.append("Public unauthenticated error: ").append(new String(bytes,0,count,java.nio.charset.StandardCharsets.UTF_8)).append('\n');
                        }
                    } catch(Exception error) { diagnostic.append("Public HTTPS ").append(error.getClass().getSimpleName()).append(": ").append(error.getMessage()).append('\n'); }
                    finally { if(connection!=null) connection.disconnect(); }
                }
                result.putString("stream",diagnostic.toString()); finish(-1,result); return;
            }
            if(arguments!=null && "official-plugins".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS official plugin checks="+OfficialPluginContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "ai-network".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS AI network checks="+NetworkAiContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && ("ai-prepare".equals(arguments.getString("mode")) || "comet-prepare".equals(arguments.getString("mode")))) {
                result.putString("stream",NetworkAiContract.prepareLive(this)); finish(-1,result); return;
            }
            if(arguments!=null && ("ai-configure".equals(arguments.getString("mode")) || "comet-configure".equals(arguments.getString("mode")))) {
                NetworkAiContract.configureLive(this);
                result.putString("stream","PASS temporary encrypted profile configured; no API request\n"); finish(-1,result); return;
            }
            if(arguments!=null && ("ai-clear".equals(arguments.getString("mode")) || "comet-clear".equals(arguments.getString("mode")))) {
                NetworkAiContract.restoreLive(this);
                result.putString("stream","PASS prior AI profile and plugin state restored\n"); finish(-1,result); return;
            }
            if(arguments!=null && "appsettings".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS grouped app settings checks="+AppSettingsContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "delivery".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS service delivery checks="+ServiceDeliveryContract.run(this)+"\n"); finish(-1,result); return;
            }
            if(arguments!=null && "plugin-frames".equals(arguments.getString("mode"))) {
                result.putString("stream",PluginFrameContract.run(this)); finish(-1,result); return;
            }
            if(arguments!=null && "benchmark".equals(arguments.getString("mode"))) {
                result.putString("stream",EngineBenchmark.run(this,arguments)); finish(-1,result); return;
            }
            if(arguments!=null && "plugins".equals(arguments.getString("mode"))) {
                result.putString("stream","PASS plugin backend checks="+PluginBackendContract.run(this)+"\n"); finish(-1,result); return;
            }
            boolean renderingOnly=arguments!=null && "rendering".equals(arguments.getString("mode"));
            final NativeRimeEngine isolatedEngine;
            if(renderingOnly) isolatedEngine=null;
            else {
                String selectedIme=android.provider.Settings.Secure.getString(getTargetContext().getContentResolver(),
                        android.provider.Settings.Secure.DEFAULT_INPUT_METHOD);
                if(selectedIme!=null && selectedIme.startsWith(getTargetContext().getPackageName()+"/"))
                    throw new IllegalStateException("Select another IME before the isolated JNI contract.");
                java.io.File data=EngineResources.prepare(getTargetContext());
                if(!data.isDirectory()) throw new AssertionError("packaged resources");
                java.io.File user=new java.io.File(getTargetContext().getNoBackupFilesDir(),"engine-contract-user-"+java.util.UUID.randomUUID());
                if(!user.mkdirs()) throw new java.io.IOException("Cannot create isolated engine contract directory");
                // librime is a process singleton. Initialize its synthetic user directory before
                // any Activity/UI fixture can bind the IME service and claim the real directory.
                isolatedEngine=EngineWorker.QUEUE.submit(() -> {
                    NativeRimeEngine engine=new NativeRimeEngine();
                    engine.initialize(data.getAbsolutePath(),user.getAbsolutePath()); return engine;
                }).get(10,java.util.concurrent.TimeUnit.SECONDS);
            }
            AppUpdateContract.validateManifest();
            // Offscreen pixel sweeps and the 1,000-draw benchmark must finish
            // before the attached touch fixture owns an input channel. Otherwise
            // its pending focus-loss event can time out behind the benchmark.
            renderOnMain("icons", () -> iconChecks=KeyboardIconContract.run(getTargetContext()));
            renderOnMain("keycaps", this::keycapRendering);
            renderOnMain("chords", () -> chordChecks=ChordSurfaceContract.run(getTargetContext()));
            renderOnMain("chord-readout", this::chordReadoutRetirement);
            renderOnMain("buffer-rail", this::renderBufferRail);
            renderOnMain("candidate-matrix", () -> bufferRenderingResult+="CANDIDATE_MATRIX checks="+CandidateMatrixContract.run(getTargetContext())+"\n");
            renderOnMain("keyboard-height", () -> bufferRenderingResult+="HEIGHT_FIT checks="+KeyboardHeightContract.run(getTargetContext())+"\n");
            renderOnMain("appearance", () -> bufferRenderingResult+="PET_APPEARANCE checks="+KeyboardAppearanceContract.run(getTargetContext())+"\n");
            touchChecks=NativeTouchContract.run(this);
            Bundle rendering=new Bundle(); rendering.putString("stream","NATIVE_TOUCH checks="+touchChecks+"\nVECTOR_ICONS checks="+iconChecks+"\nCHORD_SURFACE checks="+chordChecks+"\n"+bufferRenderingResult); sendStatus(0,rendering);
            if(renderingOnly) {
                result.putString("stream","PASS native keycaps, chord and Buffer rail rendering\n"+bufferRenderingResult); finish(-1,result); return;
            }
            for(String value:new String[]{"中文","A𠮷😀Z","\u0000","你好\u0000𠮷"}) {
                if(!value.equals(NativeRimeEngine.roundTripNative(value))) throw new AssertionError("JNI Unicode round trip");
            }
            EngineWorker.QUEUE.submit(() -> {
                NativeRimeEngine engine=isolatedEngine;
                long session=engine.createSession();
                if(!engine.selectSchema(session,"rimes_pinyin_private")) throw new AssertionError("private schema");
                for(char key:"shi".toCharArray()) engine.processKey(session,key);
                org.scholay.rimes.core.RimeEngine.Snapshot anchor=engine.snapshot(session);
                org.scholay.rimes.core.CandidateMatrix.Preview preview=org.scholay.rimes.core.CandidateMatrix.preview(engine,session,anchor,36);
                if(preview.items.size()<20 || engine.snapshot(session).pageStart!=anchor.pageStart) throw new AssertionError("Matrix preview/restore");
                org.scholay.rimes.core.CandidateMatrix.Item chosen=preview.items.get(18);
                if(!engine.selectCandidate(session,chosen.index).commit.equals(chosen.text)) throw new AssertionError("Matrix absolute candidate commit");
                engine.clearComposition(session);
                for(char key:"nihao".toCharArray()) engine.processKey(session,key);
                org.scholay.rimes.core.RimeEngine.Snapshot partial=engine.selectCandidate(session,1);
                if(!partial.preedit.contains("你") || partial.caret!=partial.preedit.length())
                    throw new AssertionError("UTF-8 byte caret to UTF-16: "+partial.preedit+" caret="+partial.caret);
                engine.clearComposition(session);
                for(String schema:new String[]{"rimes_pinyin9","rimes_pinyin9_private"}) {
                    if(!engine.selectSchema(session,schema)) throw new AssertionError("nine-key schema");
                    org.scholay.rimes.core.RimeEngine.Snapshot nine=org.scholay.rimes.core.RimeEngine.Snapshot.EMPTY;
                    for(char key:"64426".toCharArray()) nine=engine.processKey(session,key);
                    if(nine.candidates.isEmpty() || !nine.candidates.get(0).equals("你好")) throw new AssertionError("nine-key candidates: "+nine.candidates);
                    if(!engine.selectCandidate(session,0).commit.equals("你好")) throw new AssertionError("nine-key commit");
                    engine.clearComposition(session);
                }
                engine.destroySession(session);
            }).get(10,java.util.concurrent.TimeUnit.SECONDS);
            android.view.inputmethod.EditorInfo info=new android.view.inputmethod.EditorInfo();
            for(int type:new int[]{android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD,
                    android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD,
                    android.text.InputType.TYPE_CLASS_TEXT|android.text.InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD,
                    android.text.InputType.TYPE_CLASS_NUMBER|android.text.InputType.TYPE_NUMBER_VARIATION_PASSWORD}) {
                info.inputType=type;
                if(!RimesInputMethodService.isPassword(info) || RimesInputMethodService.allowsBuffer(info)) throw new AssertionError("password policy");
            }
            info.inputType=android.text.InputType.TYPE_CLASS_TEXT;
            info.imeOptions=android.view.inputmethod.EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING;
            if(RimesInputMethodService.isPassword(info) || RimesInputMethodService.allowsBuffer(info)) throw new AssertionError("private field policy");
            result.putString("stream","PASS 18 keycap palettes in light/dark/pressed states and scrolled text; Buffer text/chips/large viewport; JNI Chinese/non-BMP/NUL round trips, UTF-16 preedit caret, resources, nine-key normal/private schemas and platform password/private policies\n"+bufferRenderingResult); finish(-1,result);
        } catch(Throwable error) { result.putString("stream","FAIL "+android.util.Log.getStackTraceString(error)); finish(0,result); }
    }
    private void keycapRendering() {
        for(int night:new int[]{android.content.res.Configuration.UI_MODE_NIGHT_NO,android.content.res.Configuration.UI_MODE_NIGHT_YES}) {
            android.content.res.Configuration configuration=new android.content.res.Configuration(getTargetContext().getResources().getConfiguration());
            configuration.uiMode=(configuration.uiMode&~android.content.res.Configuration.UI_MODE_NIGHT_MASK)|night;
            android.content.Context context=getTargetContext().createConfigurationContext(configuration);
            for(KeyboardTheme theme:KeyboardTheme.ALL) {
                android.widget.FrameLayout parent=new android.widget.FrameLayout(context);
                KeyButton key=new KeyButton(context); key.setText("q"); key.theme(theme); parent.addView(key,new android.widget.FrameLayout.LayoutParams(160,100));
                parent.measure(android.view.View.MeasureSpec.makeMeasureSpec(160,android.view.View.MeasureSpec.EXACTLY),android.view.View.MeasureSpec.makeMeasureSpec(100,android.view.View.MeasureSpec.EXACTLY));
                parent.layout(0,0,160,100); key.scrollTo(20000,0);
                android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(160,100,android.graphics.Bitmap.Config.ARGB_8888);
                parent.draw(new android.graphics.Canvas(bitmap));
                if(bitmap.getPixel(30,30)!=theme.palette(context).key) throw new AssertionError("keycap viewport: "+theme.id+" / "+night);
                key.setPressed(true); parent.draw(new android.graphics.Canvas(bitmap));
                if(bitmap.getPixel(30,30)!=theme.palette(context).accent) throw new AssertionError("pressed keycap: "+theme.id);
                bitmap.recycle();
            }
        }
    }
    private void renderOnMain(String phase,Runnable contract) {
        Bundle started=new Bundle(); started.putString("stream","RENDER_PHASE "+phase+" started\n"); sendStatus(0,started);
        java.util.concurrent.atomic.AtomicReference<Throwable> failure=new java.util.concurrent.atomic.AtomicReference<>();
        runOnMainSync(() -> { try { contract.run(); } catch(Throwable error) { failure.set(error); } });
        if(failure.get()!=null) throw new AssertionError("Rendering phase "+phase+" failed",failure.get());
        waitForIdleSync();
        Bundle finished=new Bundle(); finished.putString("stream","RENDER_PHASE "+phase+" passed\n"); sendStatus(0,finished);
    }
    private void chordReadoutRetirement() {
        org.scholay.rimes.core.ChordGesture gesture=new org.scholay.rimes.core.ChordGesture(org.scholay.rimes.core.ChordProfile.builtIn());
        gesture.begin(0,'d'); gesture.begin(1,'i'); gesture.move(0,'v');
        ChordPreview view=new ChordPreview(getTargetContext());
        view.render(gesture.preview(),KeyboardTheme.ALL[0],false);
        if(view.getContentDescription()==null || !view.getContentDescription().toString().contains("ni"))
            throw new AssertionError("chord readout must expose live preview");
        view.render(null,KeyboardTheme.ALL[0],false);
        if(view.getContentDescription()!=null) throw new AssertionError("old target chord readout retained");
    }
    private void renderBufferRail() {
        int draws=0;
        for(int night:new int[]{android.content.res.Configuration.UI_MODE_NIGHT_NO,android.content.res.Configuration.UI_MODE_NIGHT_YES}) {
            android.content.res.Configuration configuration=new android.content.res.Configuration(getTargetContext().getResources().getConfiguration());
            configuration.uiMode=(configuration.uiMode&~android.content.res.Configuration.UI_MODE_NIGHT_MASK)|night;
            android.content.Context context=getTargetContext().createConfigurationContext(configuration);
            float density=context.getResources().getDisplayMetrics().density;
            for(KeyboardTheme theme:new KeyboardTheme[]{KeyboardTheme.ALL[0],KeyboardTheme.ALL[1]}) for(boolean landscape:new boolean[]{false,true}) {
                BufferRail rail=new BufferRail(context); int width=Math.round(320*density),height=Math.round((landscape?28:36)*density);
                rail.render(java.util.List.of("你好𠮷😀"),"",theme,landscape); measureRail(rail,width,height);
                android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(width,height,android.graphics.Bitmap.Config.ARGB_8888);
                drawRail(rail,bitmap); draws++;
                int inkPixels=0;
                for(int y=Math.round(7*density);y<height-Math.round(7*density);y++) for(int x=Math.round(8*density);x<Math.round(38*density);x++) {
                    int pixel=bitmap.getPixel(x,y);
                    if(night==android.content.res.Configuration.UI_MODE_NIGHT_YES
                            ?android.graphics.Color.red(pixel)>180 && android.graphics.Color.green(pixel)>180 && android.graphics.Color.blue(pixel)>180
                            :android.graphics.Color.red(pixel)<70 && android.graphics.Color.green(pixel)<70 && android.graphics.Color.blue(pixel)<70) inkPixels++;
                }
                if(inkPixels<20) throw new AssertionError("Buffer glyphs missing: "+theme.id+" / "+night+" / "+landscape);
                int background=night==android.content.res.Configuration.UI_MODE_NIGHT_YES?0xff000000:0xffffffff;
                if(bitmap.getPixel(Math.round(22*density),Math.round(8*density))==background || rail.visibleChipCount()!=1)
                    throw new AssertionError("Buffer full-height chip missing");
                rail.render(java.util.List.of("你好𠮷😀"),"ni'hao",theme,landscape); measureRail(rail,width,height); drawRail(rail,bitmap); draws++;
                if(!rail.getContentDescription().toString().endsWith("ni'hao")) throw new AssertionError("Buffer preedit projection");
                rail.clearProjection(); measureRail(rail,width,height); bitmap.eraseColor(0); drawRail(rail,bitmap); draws++;
                if(rail.getContentDescription()!=null || rail.visibleChipCount()!=0) throw new AssertionError("Buffer old target cache retained");
                bitmap.recycle();
            }
        }
        android.content.Context context=getTargetContext(); float density=context.getResources().getDisplayMetrics().density;
        BufferRail rail=new BufferRail(context); int width=Math.round(320*density),height=Math.round(36*density);
        java.util.List<String> blocks=java.util.Collections.nCopies(org.scholay.rimes.core.BufferSession.MAX_CHARACTERS,"中");
        rail.render(blocks,"",KeyboardTheme.ALL[0],false); measureRail(rail,width,height);
        android.graphics.Bitmap bitmap=android.graphics.Bitmap.createBitmap(width,height,android.graphics.Bitmap.Config.ARGB_8888);
        drawRail(rail,bitmap);
        Bundle viewport=new Bundle(); viewport.putString("stream","BUFFER_VIEWPORT "+rail.drawingState()+"\n"); sendStatus(0,viewport);
        if(rail.getScrollX()==0 || rail.visibleChipCount()>32 || rail.visibleChipCount()==0)
            throw new AssertionError("Buffer large viewport not bounded: "+rail.drawingState());
        rail.scrollTo(0,0); bitmap.eraseColor(0); drawRail(rail,bitmap);
        if(rail.visibleChipCount()==0 || rail.visibleChipCount()>32) throw new AssertionError("Buffer first viewport missing: "+rail.drawingState());
        rail.fullScroll(android.view.View.FOCUS_RIGHT); bitmap.eraseColor(0); drawRail(rail,bitmap);
        if(rail.getScrollX()==0 || rail.visibleChipCount()==0 || rail.visibleChipCount()>32) throw new AssertionError("Buffer last viewport missing: "+rail.drawingState());
        int capacityInk=0; boolean dark=KeyboardTheme.ALL[0].palette(context).dark;
        for(int y=Math.round(7*density);y<height-Math.round(7*density);y++) for(int x=Math.round(12*density);x<width-Math.round(12*density);x++) {
            int pixel=bitmap.getPixel(x,y);
            if(dark?android.graphics.Color.red(pixel)>180 && android.graphics.Color.green(pixel)>180 && android.graphics.Color.blue(pixel)>180
                    :android.graphics.Color.red(pixel)<70 && android.graphics.Color.green(pixel)<70 && android.graphics.Color.blue(pixel)<70) capacityInk++;
        }
        if(capacityInk<50) throw new AssertionError("Buffer last viewport glyphs missing: "+rail.drawingState());
        long measurements=rail.confirmedMeasurementCount(),start=android.os.SystemClock.elapsedRealtimeNanos();
        for(int i=0;i<1000;i++) {
            rail.render(blocks,"ni"+(i%10),KeyboardTheme.ALL[0],false); measureRail(rail,width,height); drawRail(rail,bitmap);
        }
        double milliseconds=(android.os.SystemClock.elapsedRealtimeNanos()-start)/1_000_000.0;
        if(rail.confirmedMeasurementCount()!=measurements || rail.visibleChipCount()>32) throw new AssertionError("Buffer composition remeasured confirmed blocks");
        bufferRenderingResult="BUFFER_RENDER draws="+draws+" capacityBlocks="+blocks.size()+" visibleChips="+rail.visibleChipCount()+" confirmedMeasurements="+measurements+" capacityGlyphPixels="+capacityInk+" renders=1000 elapsedMs="+milliseconds+"\n";
        rail.clearProjection(); bitmap.recycle();
    }
    private static void measureRail(BufferRail rail,int width,int height) {
        if(rail.getParent()==null) {
            android.widget.FrameLayout parent=new android.widget.FrameLayout(rail.getContext());
            parent.addView(rail,new android.widget.FrameLayout.LayoutParams(width,height));
        }
        android.view.View parent=(android.view.View)rail.getParent();
        parent.measure(android.view.View.MeasureSpec.makeMeasureSpec(width,android.view.View.MeasureSpec.EXACTLY),
                android.view.View.MeasureSpec.makeMeasureSpec(height,android.view.View.MeasureSpec.EXACTLY));
        parent.layout(0,0,width,height);
    }
    private static void drawRail(BufferRail rail,android.graphics.Bitmap bitmap) {
        // Parent.drawChild applies the scroll transform used by the real input view. Calling a
        // scrolled rail.draw directly leaves its canvas at the root origin and is not a viewport.
        ((android.view.View)rail.getParent()).draw(new android.graphics.Canvas(bitmap));
    }
}
