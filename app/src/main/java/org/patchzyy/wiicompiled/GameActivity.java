package org.patchzyy.wiicompiled;

import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import org.libsdl.app.SDLActivity;

public class GameActivity extends SDLActivity implements SensorEventListener {
    private static final String TAG = "WiiCompiled";
    private SensorManager sensorManager;
    private Sensor gyroSensor;
    @Override protected void onCreate(Bundle b) {
        try { AssetExtractor.extractIfNeeded(getApplicationContext()); } catch (Exception e) { Log.e(TAG,"extract",e); }
        sensorManager = (SensorManager) getSystemService(SENSOR_SERVICE);
        if (sensorManager != null) {
            gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);
            if (gyroSensor == null) gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE);
            if (gyroSensor == null) gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
        }
        super.onCreate(b);
        // After super.onCreate: loadLibraries() has loaded libwiicompiled.so,
        // so the native method below resolves. Calling earlier throws
        // UnsatisfiedLinkError and silently leaves the files dir unset.
        try { nativeSetFilesDir(getFilesDir().getAbsolutePath()); } catch (Throwable t) { Log.e(TAG,"setFilesDir",t); }
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        if (sensorManager!=null&&gyroSensor!=null) sensorManager.registerListener(this,gyroSensor,SensorManager.SENSOR_DELAY_GAME);
    }
    @Override protected void onResume(){ super.onResume(); if(sensorManager!=null&&gyroSensor!=null) sensorManager.registerListener(this,gyroSensor,SensorManager.SENSOR_DELAY_GAME); }
    @Override protected void onPause(){ if(sensorManager!=null) sensorManager.unregisterListener(this); super.onPause(); }
    // SDL3 is statically linked into libwiicompiled.so; the default
    // getMainSharedObject() already resolves the last entry to
    // nativeLibraryDir/libwiicompiled.so, so no override is needed.
    @Override protected String[] getLibraries(){ return new String[]{"wiicompiled"}; }
    @Override public void onSensorChanged(SensorEvent e){ float roll=0; if(e.sensor.getType()==Sensor.TYPE_GAME_ROTATION_VECTOR){ float[] rot=new float[9],o=new float[3]; SensorManager.getRotationMatrixFromVector(rot,e.values); SensorManager.getOrientation(rot,o); roll=o[2];} else if(e.sensor.getType()==Sensor.TYPE_GYROSCOPE) roll=e.values[2]*0.02f; else if(e.sensor.getType()==Sensor.TYPE_ACCELEROMETER) roll=(float)Math.atan2(e.values[0],e.values[1]); try{nativeOnGyroSteer(roll);}catch(UnsatisfiedLinkError ignored){}}
    @Override public void onAccuracyChanged(Sensor s,int a){}
    private native void nativeSetFilesDir(String path);
    private native void nativeOnGyroSteer(float roll);
    @Override public boolean onKeyDown(int kc,KeyEvent ev){ if(kc==KeyEvent.KEYCODE_BACK){ try{nativeOnBackPressed();}catch(UnsatisfiedLinkError ignored){} return true;} return super.onKeyDown(kc,ev);}
    private native void nativeOnBackPressed();

}
