package org.papaya.emulator;

public class PapayaNativeBridge {
    static {
        System.loadLibrary("papaya");
    }

    // Native C++23 JNI bindings
    public static native int initRuntime(int potatoMode, int deviceTier);
    public static native int loadRomFd(int fd, long totalSizeBytes);
    public static native void stepFrame();
    public static native void sendTouchInput(int slot, int buttons, int axisX, int axisY);
    public static native void shutdown();
}
