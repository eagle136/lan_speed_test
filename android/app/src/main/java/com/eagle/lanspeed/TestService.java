package com.eagle.lanspeed;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;

public class TestService extends Service {
    private static final String CHANNEL = "lanspeed_test";
    private static final int NOTIFY_ID = 1001;
    private static final long WAKE_TIMEOUT_MS = 10 * 60 * 1000L;

    private PowerManager.WakeLock wakeLock;

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        PowerManager pm = (PowerManager) getApplicationContext()
                .getSystemService(Context.POWER_SERVICE);
        if (pm != null) {
            if (wakeLock == null) {
                wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,
                        "LanSpeed:test");
                wakeLock.setReferenceCounted(false);
            }
            if (!wakeLock.isHeld())
                wakeLock.acquire(WAKE_TIMEOUT_MS);
        }
        showForegroundNotification();
        return START_NOT_STICKY;
    }

    @Override
    public void onDestroy() {
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        super.onDestroy();
    }

    private void showForegroundNotification() {
        NotificationManager nm = (NotificationManager)
                getSystemService(Context.NOTIFICATION_SERVICE);
        Notification notification;
        if (Build.VERSION.SDK_INT >= 26) {
            if (nm != null && nm.getNotificationChannel(CHANNEL) == null) {
                NotificationChannel channel = new NotificationChannel(CHANNEL,
                        getString(R.string.app_name),
                        NotificationManager.IMPORTANCE_LOW);
                channel.setShowBadge(false);
                nm.createNotificationChannel(channel);
            }
            notification = new Notification.Builder(this, CHANNEL)
                    .setSmallIcon(android.R.drawable.stat_sys_download_done)
                    .setContentTitle(getString(R.string.app_name))
                    .setContentText("测速进行中，请勿熄屏或切到后台")
                    .setOngoing(true)
                    .build();
        } else {
            notification = new Notification.Builder(this)
                    .setSmallIcon(android.R.drawable.stat_sys_download_done)
                    .setContentTitle(getString(R.string.app_name))
                    .setContentText("测速进行中，请勿熄屏或切到后台")
                    .setOngoing(true)
                    .setPriority(Notification.PRIORITY_LOW)
                    .build();
        }
        if (Build.VERSION.SDK_INT >= 29) {
            startForeground(NOTIFY_ID, notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC);
        } else {
            startForeground(NOTIFY_ID, notification);
        }
    }
}
