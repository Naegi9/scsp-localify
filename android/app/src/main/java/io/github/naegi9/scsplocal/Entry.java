package io.github.naegi9.scsplocal;

import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.IXposedHookZygoteInit;
import de.robv.android.xposed.XposedBridge;
import de.robv.android.xposed.callbacks.XC_LoadPackage;

public class Entry implements IXposedHookLoadPackage, IXposedHookZygoteInit {
    private static final String TAG = "SCSPLocal";
    private static String modulePath;
    private static boolean started;

    static native void nativeInit(String dataDir, String packageName);

    @Override
    public void initZygote(StartupParam startupParam) {
        modulePath = startupParam.modulePath;
    }

    private static void log(String s) {
        Log.i(TAG, s);
        try { XposedBridge.log("[" + TAG + "] " + s); } catch (Throwable ignored) { }
    }

    @Override
    public void handleLoadPackage(XC_LoadPackage.LoadPackageParam lp) {
        if (started) return;
        if (lp.processName != null && !lp.processName.equals(lp.packageName)) return;  // main process only
        started = true;
        log("module loaded into " + lp.packageName + " (modulePath=" + modulePath + ")");

        try {
            loadNativeLibs(lp.packageName);
            String dir = "/storage/emulated/0/Android/data/" + lp.packageName + "/files/scsp_localify";
            nativeInit(dir, lp.packageName);
            log("native init done, files: " + dir);
        } catch (Throwable t) {
            log("failed to start: " + Log.getStackTraceString(t));
        }
    }

    private static void loadNativeLibs(String pkg) throws Exception {
        try {
            System.loadLibrary("shadowhook");
            System.loadLibrary("scsplocal");
            log("native libs loaded from module");
            return;
        } catch (Throwable t) {
            log("loadLibrary failed (" + t + "), extracting from module apk");
        }
        if (modulePath == null) throw new IllegalStateException("module path unknown");
        File outDir = new File("/data/data/" + pkg + "/files/scsp_native");
        outDir.mkdirs();
        try (ZipFile zip = new ZipFile(modulePath)) {
            for (String name : new String[]{"libshadowhook.so", "libscsplocal.so"}) {
                ZipEntry e = zip.getEntry("lib/arm64-v8a/" + name);
                if (e == null) continue;
                File out = new File(outDir, name);
                try (InputStream in = zip.getInputStream(e); FileOutputStream fo = new FileOutputStream(out)) {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) fo.write(buf, 0, n);
                }
            }
        }
        for (String name : new String[]{"libshadowhook.so", "libscsplocal.so"}) {
            File f = new File(outDir, name);
            if (f.exists()) System.load(f.getAbsolutePath());
        }
        log("native libs loaded from " + outDir);
    }
}
