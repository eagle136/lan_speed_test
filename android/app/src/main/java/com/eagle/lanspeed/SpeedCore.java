package com.eagle.lanspeed;

public final class SpeedCore {
    public static final int ROLE_PC = 1;
    public static final int ROLE_PHONE = 2;

    static {
        System.loadLibrary("lanspeed");
    }

    private SpeedCore() {
    }

    public static native long nativeCreate(int role, String peer, int port, int seconds);

    public static native void nativeSetPipeline(long handle, boolean enabled);

    public static native int nativeStart(long handle);

    public static native void nativeStop(long handle);

    public static native void nativeDestroy(long handle);

    public static native String nativeStatus(long handle);

    public static native String nativeLocalIp(String peer, int port);

    public static native int nativeDiscStart(int udpPort, int tcpPort, String name);

    public static native void nativeDiscStop();

    /** One active discovery round. Returns "ip\tname\tid" or null. */
    public static native String nativeDiscScan(int timeoutMs);

    /** Most recently heard peer as "ip\tname\tid", or null. */
    public static native String nativeDiscLatest();

    public static native String nativeDiscSelfId();

    /** Enables/disables core diagnostic logging to logcat (default off). */
    public static native void nativeSetLog(boolean enabled);
}
