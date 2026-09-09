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
    private int dp(int v){return Math.round(v*getResources().getDisplayMetrics().density);}
}
