package org.patchzyy.wiicompiled;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.util.Log;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

public class PickerActivity extends Activity {
    private static final String TAG = "WiiCompiled";
    private static final int REQ_PICK_ROM = 9001;
    static File getRomDir(Activity c) { return new File(c.getFilesDir(), "rom"); }
    static boolean hasRom(Activity c) {
        File dir = getRomDir(c);
        if (!dir.isDirectory()) return false;
        // An extracted DATA tree (files/ + sys/fst.bin, with no single large
        // image) is a valid boot source; a raw-image probe alone would strand
        // it on the picker screen forever.
        if (new File(dir, "sys/fst.bin").isFile() && new File(dir, "files").isDirectory()) return true;
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File f : fs) if (f.isFile() && f.length() > 1024*1024) return true;
        return false;
    }
    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        try { AssetExtractor.extractIfNeeded(getApplicationContext()); } catch (Exception e) { Log.e(TAG,"extract",e); }
        //adb shell am start -n org.patchzyy.wiicompiled/.PickerActivity -a org.patchzyy.wiicompiled.IMPORT_DATA_TAR
        try {
            Intent it = getIntent();
            if (it != null && "org.patchzyy.wiicompiled.IMPORT_DATA_TAR".equals(it.getAction())) {
                TextView t = new TextView(this);
                t.setText("Importing DATA from /sdcard/data.tar ...");
                t.setTextColor(0xFFFFFFFF); t.setGravity(Gravity.CENTER);
                setContentView(t);
                importDataTar(this);
                return;
            }
        } catch (Exception e) { Log.e(TAG,"importAction",e); }
        if (hasRom(this)) { launchGame(); return; }
        showPicker();
    }
    private void launchGame() {
        Intent i = new Intent(this, GameActivity.class);
        startActivity(i);
        finish();
    }
    private void showPicker() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL); root.setGravity(Gravity.CENTER);
        root.setPadding(dp(24),dp(32),dp(24),dp(32)); root.setBackgroundColor(0xFF0F0F0F);
        TextView t=new TextView(this); t.setText("Mario Kart Wii — ROM Required"); t.setTextSize(22); t.setTextColor(0xFFFFFFFF); t.setGravity(Gravity.CENTER); root.addView(t);
        TextView body=new TextView(this); body.setText("No game data found.\n\nPut your Mario Kart Wii dump here (any region):\n"+getRomDir(this).getAbsolutePath()+"\n\nAccepted: ISO / RVZ / WIA / WBFS / CISO / GCM (PAL RMCP01, NTSC-U RMCE01, NTSC-J RMCJ01, etc.)"); body.setTextColor(0xFFCCCCCC); body.setTextSize(13); body.setPadding(0,dp(20),0,dp(24)); body.setGravity(Gravity.CENTER); root.addView(body);
        Button pick=new Button(this); pick.setText("Pick ROM file..."); pick.setOnClickListener(v->launchPicker()); root.addView(pick);
        Button cont=new Button(this); cont.setText("Try booting anyway"); cont.setAlpha(0.7f);
        LinearLayout.LayoutParams lp=new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,ViewGroup.LayoutParams.WRAP_CONTENT); lp.topMargin=dp(12); lp.gravity=Gravity.CENTER; cont.setLayoutParams(lp);
        cont.setOnClickListener(v-> launchGame()); root.addView(cont);
        setContentView(root);
    }
    private void launchPicker(){ Intent i=new Intent(Intent.ACTION_OPEN_DOCUMENT); i.addCategory(Intent.CATEGORY_OPENABLE); i.setType("*/*"); i.putExtra(Intent.EXTRA_MIME_TYPES,new String[]{"application/octet-stream","*/*"}); i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION|Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION); try{startActivityForResult(i,REQ_PICK_ROM);}catch(Exception e){Toast.makeText(this,"No picker",Toast.LENGTH_LONG).show();}}
    @Override protected void onActivityResult(int r,int rs,Intent d){ super.onActivityResult(r,rs,d); if(r==REQ_PICK_ROM&&rs==RESULT_OK&&d!=null&&d.getData()!=null){ Uri u=d.getData(); try{getContentResolver().takePersistableUriPermission(u,Intent.FLAG_GRANT_READ_URI_PERMISSION);}catch(Exception ignored){} copyUri(u);}}
    private void copyUri(Uri uri){ String name="RMCP01.iso"; try(Cursor c=getContentResolver().query(uri,null,null,null,null)){ if(c!=null&&c.moveToFirst()){int idx=c.getColumnIndex(OpenableColumns.DISPLAY_NAME); if(idx>=0){String n=c.getString(idx); if(n!=null&&!n.isEmpty()) name=n;}}}catch(Exception ignored){} File dir=getRomDir(this); dir.mkdirs(); File out=new File(dir,name); String fn=name; Toast.makeText(this,"Copying "+fn+"...",Toast.LENGTH_SHORT).show(); new Thread(()->{ try(InputStream in=getContentResolver().openInputStream(uri); FileOutputStream fos=new FileOutputStream(out)){ byte[]b=new byte[256*1024]; int rr; long tot=0; while((rr=in.read(b))!=-1){fos.write(b,0,rr); tot+=rr;} long t=tot; runOnUiThread(()->{Toast.makeText(this,"Staged "+fn+" ("+(t>>20)+" MB)",Toast.LENGTH_LONG).show(); launchGame();});}catch(Exception e){Log.e(TAG,"copy",e); runOnUiThread(()->Toast.makeText(this,"Copy failed: "+e.getMessage(),Toast.LENGTH_LONG).show());}}).start();}
    // Debug/testing entry: unpack a tar of an extracted DATA tree staged at
    // /sdcard/data.tar straight into files/rom (expects top-level DATA/).
    // Not wired to any button; invoked via: adb shell am broadcast -a
    // org.patchzyy.wiicompiled.IMPORT_DATA_TAR.
    static void importDataTar(Activity c) {
        new Thread(()->{
            try {
                // Prefer the app's own external files dir (no storage permission
                // needed under scoped storage): adb push data.tar there, i.e.
                // /sdcard/Android/data/org.patchzyy.wiicompiled/files/data.tar
                java.io.File tar = new java.io.File(c.getExternalFilesDir(null), "data.tar");
                if (!tar.canRead()) tar = new java.io.File("/sdcard/data.tar");
                java.io.File rom = getRomDir(c); rom.mkdirs();
                // Minimal tar reader: regular files + dirs, pax-safe via prefix.
                try (java.io.InputStream raw = new java.io.FileInputStream(tar)) {
                    byte[] hdr = new byte[512];
                    long staged = 0; int files = 0;
                    for (;;) {
                        int got = 0;
                        while (got < 512) { int r = raw.read(hdr, got, 512-got); if (r < 0) break; got += r; }
                        if (got == 0) break;
                        if (got < 512) throw new java.io.IOException("short tar header");
                        boolean zero = true;
                        for (byte x : hdr) if (x != 0) { zero = false; break; }
                        if (zero) break; // end-of-archive padding
                        String n = cstr(hdr, 0, 100);
                        String pre = cstr(hdr, 345, 155);
                        if (!pre.isEmpty()) n = pre + "/" + n;
                        long size = 0;
                        try { size = Long.parseLong(cstr(hdr, 124, 12).trim(), 8); } catch (Exception ignored) {}
                        char kind = (char) hdr[156];
                        // Strip a single top-level DATA/ prefix if present.
                        if (n.startsWith("DATA/")) n = n.substring(5);
                        java.io.File dst = new java.io.File(rom, n);
                        if (kind == '5' || n.endsWith("/")) { dst.mkdirs(); }
                        else if (!n.isEmpty()) {
                            dst.getParentFile().mkdirs();
                            try (java.io.FileOutputStream fos = new java.io.FileOutputStream(dst)) {
                                byte[] bb = new byte[256*1024]; long left = size;
                                while (left > 0) { int r = raw.read(bb, 0, (int)Math.min(bb.length, left)); if (r < 0) throw new java.io.IOException("short tar data"); fos.write(bb, 0, r); left -= r; }
                            }
                            staged += size; files++;
                        }
                        long skip = (512 - (size % 512)) % 512;
                        while (skip > 0) { long s = raw.skip(skip); if (s <= 0) break; skip -= s; }
                    }
                    long st = staged; int fc = files;
                    ((Activity)c).runOnUiThread(()->Toast.makeText(c,"Imported DATA ("+fc+" files, "+(st>>20)+" MB)",Toast.LENGTH_LONG).show());
                }
            } catch (Exception e) { Log.e(TAG,"importDataTar",e); try { ((Activity)c).runOnUiThread(()->Toast.makeText(c,"Import failed: "+e.getMessage(),Toast.LENGTH_LONG).show()); } catch (Exception ignored) {} }
        }).start();
    }
    private int dp(int v){return Math.round(v*getResources().getDisplayMetrics().density);}
    // NUL-terminated string out of a tar header field (NULs, not Java
    // regex-split: "\0" as a regex splits on every empty match when the
    // field has no NUL, yielding a zero-length first element).
    private static String cstr(byte[] hdr, int off, int len) {
        int end = off;
        while (end < off + len && hdr[end] != 0) end++;
        return new String(hdr, off, end - off, java.nio.charset.StandardCharsets.UTF_8);
    }
}
