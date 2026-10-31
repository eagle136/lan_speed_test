package com.eagle.lanspeed;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.wifi.WifiInfo;
import android.net.wifi.WifiManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.Editable;
import android.text.InputFilter;
import android.text.Spanned;
import android.text.TextWatcher;
import android.util.Log;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONObject;

import java.util.Locale;

public class MainActivity extends Activity {
    private static final int PORT = 5201;
    private static final int STREAMS = 4;
    private static final int SECONDS = 10;
    private static final int TICK_MS = 100;
    private static final int DISC_PORT = 5310;
    private static final int DISC_SCAN_MS = 4000;
    private static final int DISC_POLL_MS = 1000;

    private EditText peerIpEdit;
    private TextView localInfo;
    private TextView phaseView;
    private TextView speedView;
    private TextView speedSub;
    private ProgressBar progress;
    private TextView resultUp;
    private TextView resultDown;
    private TextView hintView;
    private Button startBtn;
    private Button stopBtn;
    private Button scanBtn;
    private CheckBox pipelineCheck;
    private CheckBox logCheck;
    private WifiManager wifiManager;
    private WifiManager.WifiLock wifiLock;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final long[] sessions = new long[STREAMS];
    private boolean running;
    private boolean destroyed;
    private boolean logOn;
    private boolean scanning;
    private boolean settingText;
    private String peerIp = "";
    private String localIp = "";
    private String peerId = "";
    private String peerName = "";
    private String autoIp = "";
    private int role = SpeedCore.ROLE_PHONE;
    private long uiPolls;
    private long uiNativeNs;
    private long uiNativeMaxNs;
    private long uiTotalNs;
    private long uiTotalMaxNs;

    private final Runnable pollTask = new Runnable() {
        @Override
        public void run() {
            pollStatus();
        }
    };

    private final Runnable discPollTask = new Runnable() {
        @Override
        public void run() {
            if (destroyed)
                return;
            if (!running) {
                String res = SpeedCore.nativeDiscLatest();
                if (res != null)
                    applyPeer(res, false);
            }
            handler.postDelayed(discPollTask, DISC_POLL_MS);
        }
    };

    private final Runnable wifiTask = new Runnable() {
        @Override
        public void run() {
            if (!running || destroyed)
                return;
            try {
                WifiInfo wi = wifiManager == null ? null : wifiManager.getConnectionInfo();
                if (wi != null) {
                    String extra = "";
                    if (Build.VERSION.SDK_INT >= 30) {
                        extra = " txLink=" + wi.getTxLinkSpeedMbps()
                                + " rxLink=" + wi.getRxLinkSpeedMbps();
                    }
                    logi("LanSpeedWifi", "rssi=" + wi.getRssi()
                            + " link=" + wi.getLinkSpeed()
                            + " freq=" + wi.getFrequency() + extra
                            + " lock=" + (wifiLock != null && wifiLock.isHeld()));
                }
            } catch (Exception e) {
                if (logOn)
                    Log.w("LanSpeedWifi", "wifi info failed", e);
            }
            handler.postDelayed(this, 1000);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        peerIpEdit = findViewById(R.id.peer_ip);
        localInfo = findViewById(R.id.local_info);
        phaseView = findViewById(R.id.phase);
        speedView = findViewById(R.id.speed);
        speedSub = findViewById(R.id.speed_sub);
        progress = findViewById(R.id.progress);
        resultUp = findViewById(R.id.result_up);
        resultDown = findViewById(R.id.result_down);
        hintView = findViewById(R.id.hint);
        startBtn = findViewById(R.id.btn_start);
        stopBtn = findViewById(R.id.btn_stop);
        scanBtn = findViewById(R.id.btn_scan);
        pipelineCheck = findViewById(R.id.pipeline);
        logCheck = findViewById(R.id.logging);
        wifiManager = (WifiManager) getApplicationContext()
                .getSystemService(Context.WIFI_SERVICE);

        installIpFilter(peerIpEdit);
        peerIpEdit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                if (!settingText)
                    autoIp = "";
            }
        });

        logCheck.setChecked(false);
        SpeedCore.nativeSetLog(false);
        logCheck.setOnCheckedChangeListener((b, checked) -> {
            logOn = checked;
            SpeedCore.nativeSetLog(checked);
        });
        startBtn.setOnClickListener(v -> startTest());
        stopBtn.setOnClickListener(v -> stopTest());
        scanBtn.setOnClickListener(v -> startScan());

        int rc = SpeedCore.nativeDiscStart(DISC_PORT, PORT, Build.MODEL);
        logi("LanSpeedDisc", "discovery start rc=" + rc);
        showIdle();

        handler.postDelayed(discPollTask, DISC_POLL_MS);
        handler.postDelayed(this::startScan, 500);
    }

    private void installIpFilter(EditText edit) {
        InputFilter length = new InputFilter.LengthFilter(15);
        InputFilter chars = new InputFilter() {
            @Override
            public CharSequence filter(CharSequence source, int start, int end,
                                       Spanned dest, int dstart, int dend) {
                for (int i = start; i < end; i++) {
                    char c = source.charAt(i);
                    if (!(Character.isDigit(c) || c == '.'))
                        return "";
                }
                return null;
            }
        };
        edit.setFilters(new InputFilter[]{length, chars});
    }

    private static int compareIp(String a, String b) {
        String[] pa = a.split("\\.");
        String[] pb = b.split("\\.");
        if (pa.length != 4 || pb.length != 4)
            return a.compareTo(b);
        for (int i = 0; i < 4; i++) {
            int va = Integer.parseInt(pa[i]);
            int vb = Integer.parseInt(pb[i]);
            if (va != vb)
                return va < vb ? -1 : 1;
        }
        return 0;
    }

    private static String parseIpv4(String text) {
        if (text == null)
            return null;
        String[] p = text.trim().split("\\.");
        if (p.length != 4)
            return null;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 4; i++) {
            String s = p[i];
            if (s.isEmpty() || s.length() > 3)
                return null;
            int v;
            try {
                v = Integer.parseInt(s);
            } catch (NumberFormatException e) {
                return null;
            }
            if (v < 0 || v > 255)
                return null;
            if (i > 0)
                sb.append('.');
            sb.append(v);
        }
        return sb.toString();
    }

    /* Both sides order (ip, id) the same way: the smaller one listens. */
    private int decideRole(String ownIp, String otherIp, String ownId, String otherId) {
        int c = compareIp(ownIp, otherIp);
        if (c == 0) {
            String o = ownId == null ? "" : ownId;
            String t = otherId == null ? "" : otherId;
            c = o.compareTo(t);
            if (c == 0)
                c = 1;
        }
        return c < 0 ? SpeedCore.ROLE_PHONE : SpeedCore.ROLE_PC;
    }

    private String roleName() {
        return role == SpeedCore.ROLE_PHONE ? "监听端" : "连接端";
    }

    private String dirUp() {
        return role == SpeedCore.ROLE_PHONE ? "本机 → 对方" : "对方 → 本机";
    }

    private String dirDown() {
        return role == SpeedCore.ROLE_PHONE ? "对方 → 本机" : "本机 → 对方";
    }

    private void startScan() {
        if (running || scanning || destroyed)
            return;
        scanning = true;
        scanBtn.setEnabled(false);
        scanBtn.setAlpha(0.42f);
        scanBtn.setText("扫描中");
        hintView.setText("正在扫描局域网内的测速设备…");
        new Thread(() -> {
            final String res = SpeedCore.nativeDiscScan(DISC_SCAN_MS);
            runOnUiThread(() -> {
                if (destroyed)
                    return;
                scanning = false;
                scanBtn.setEnabled(!running);
                scanBtn.setAlpha(running ? 0.42f : 1.0f);
                scanBtn.setText("扫描");
                if (res != null) {
                    applyPeer(res, true);
                    hintView.setText("已发现「" + (peerName.isEmpty() ? peerIp : peerName)
                            + "」；两台设备各点一次“开始测速”。");
                } else if (!running) {
                    hintView.setText("未发现其他设备：请确认对端也已打开本应用并处于同一局域网。");
                }
            });
        }, "lan-disc-scan").start();
    }

    /* res = "ip\tname\tid" */
    private void applyPeer(String res, boolean force) {
        String[] parts = res.split("\t", -1);
        if (parts.length < 1 || parts[0].isEmpty())
            return;
        String ip = parts[0];
        String name = parts.length > 1 ? parts[1] : "";
        String id = parts.length > 2 ? parts[2] : "";
        if (!localIp.isEmpty() && ip.equals(localIp))
            return;
        String cur = peerIpEdit.getText().toString().trim();
        boolean changed = !name.equals(peerName) || !id.equals(peerId);
        if (force || cur.isEmpty() || cur.equals(autoIp)) {
            if (!cur.equals(ip)) {
                settingText = true;
                peerIpEdit.setText(ip);
                settingText = false;
                autoIp = ip;
                changed = true;
            }
        }
        peerName = name;
        peerId = id;
        if (changed)
            logi("LanSpeedDisc", "peer ip=" + ip + " name=" + name + " id=" + id);
        if (!running && changed) {
            String lip = SpeedCore.nativeLocalIp(ip, PORT);
            localIp = lip == null ? "" : lip;
            updateLocalInfo(ip, localIp);
            hintView.setText("已发现「" + (name.isEmpty() ? ip : name)
                    + "」；两台设备各点一次“开始测速”。");
        }
    }

    private void showIdle() {
        phaseView.setText("就绪");
        phaseView.setTextColor(getColor(R.color.text));
        speedView.setText("0.00");
        speedSub.setText("≈ 0.00 MB/s");
        progress.setProgress(0);
        resultUp.setText(dirUp() + "    --");
        resultDown.setText(dirDown() + "    --");
        hintView.setText("两台设备启动后自动互相发现并填入 IP；各自点击一次“开始测速”。");
        updateLocalInfo(peerIpEdit.getText().toString().trim(), localIp);
        setVisualEnabled(startBtn, true);
        setVisualEnabled(stopBtn, false);
        setVisualEnabled(peerIpEdit, true);
        setVisualEnabled(scanBtn, true);
        setVisualEnabled(pipelineCheck, true);
    }

    private void updateLocalInfo(String peer, String local) {
        String p = peer == null || peer.isEmpty() ? "--" : peer;
        String l = local == null || local.isEmpty() ? "--" : local;
        String tail = "--".equals(p) || peerName.isEmpty() ? p : p + "（" + peerName + "）";
        String text = "本机 " + l + ":" + PORT + "-" + (PORT + STREAMS - 1)
                + (running ? " · " + roleName() : "")
                + "    对方 " + tail;
        if (!text.equals(localInfo.getText().toString()))
            localInfo.setText(text);
    }

    private void logi(String tag, String msg) {
        if (logOn)
            Log.i(tag, msg);
    }

    private void setVisualEnabled(View view, boolean enabled) {
        view.setEnabled(enabled);
        view.setAlpha(enabled ? 1.0f : 0.42f);
    }

    /* The OEM power managers freeze a backgrounded or screen-off app, which kills
     * the transfer; the foreground service plus its wakelock keeps the process running. */
    private void startKeepAwake() {
        startService(new Intent(this, TestService.class));
    }

    private void stopKeepAwake() {
        stopService(new Intent(this, TestService.class));
    }

    private void acquireWifiLock() {
        try {
            if (wifiManager == null)
                return;
            if (wifiLock == null) {
                wifiLock = wifiManager.createWifiLock(
                        WifiManager.WIFI_MODE_FULL_HIGH_PERF, "LanSpeed:wifi");
                wifiLock.setReferenceCounted(false);
            }
            if (!wifiLock.isHeld()) {
                wifiLock.acquire();
                logi("LanSpeedWifi", "wifi lock acquired");
            }
        } catch (Exception e) {
            if (logOn)
                Log.w("LanSpeedWifi", "wifi lock acquire failed", e);
        }
    }

    private void releaseWifiLock() {
        try {
            if (wifiLock != null && wifiLock.isHeld()) {
                wifiLock.release();
                logi("LanSpeedWifi", "wifi lock released");
            }
        } catch (Exception e) {
            if (logOn)
                Log.w("LanSpeedWifi", "wifi lock release failed", e);
        }
    }

    private void startWifiMonitor() {
        handler.removeCallbacks(wifiTask);
        handler.post(wifiTask);
    }

    private void stopWifiMonitor() {
        handler.removeCallbacks(wifiTask);
    }

    private void resetUiStats() {
        uiPolls = 0;
        uiNativeNs = 0;
        uiNativeMaxNs = 0;
        uiTotalNs = 0;
        uiTotalMaxNs = 0;
    }

    private void recordUiPoll(long t0, long t1, long t2) {
        long nativeNs = t1 - t0;
        long totalNs = t2 - t0;
        uiPolls++;
        uiNativeNs += nativeNs;
        uiTotalNs += totalNs;
        if (nativeNs > uiNativeMaxNs)
            uiNativeMaxNs = nativeNs;
        if (totalNs > uiTotalMaxNs)
            uiTotalMaxNs = totalNs;
        if ((uiPolls % 10) == 0) {
            logi("LanSpeedUi", String.format(Locale.US,
                    "polls=%d native_avg_us=%.1f native_max_us=%.1f "
                            + "total_avg_us=%.1f total_max_us=%.1f",
                    uiPolls,
                    uiNativeNs / (double) uiPolls / 1000.0,
                    uiNativeMaxNs / 1000.0,
                    uiTotalNs / (double) uiPolls / 1000.0,
                    uiTotalMaxNs / 1000.0));
        }
    }

    private boolean anySession() {
        for (int i = 0; i < STREAMS; i++) {
            if (sessions[i] != 0)
                return true;
        }
        return false;
    }

    private void destroySessions() {
        for (int i = 0; i < STREAMS; i++) {
            if (sessions[i] != 0) {
                SpeedCore.nativeStop(sessions[i]);
                SpeedCore.nativeDestroy(sessions[i]);
                sessions[i] = 0;
            }
        }
    }

    private void startTest() {
        if (running || destroyed)
            return;
        String ip = parseIpv4(peerIpEdit.getText().toString());
        if (ip == null) {
            Toast.makeText(this, "请输入完整 IPv4 地址，例如 192.168.5.12",
                    Toast.LENGTH_SHORT).show();
            return;
        }

        for (int i = 0; i < STREAMS; i++) {
            if (sessions[i] != 0) {
                SpeedCore.nativeDestroy(sessions[i]);
                sessions[i] = 0;
            }
        }
        peerIp = ip;
        String lip = SpeedCore.nativeLocalIp(peerIp, PORT);
        localIp = lip == null ? "" : lip;
        if (localIp.isEmpty()) {
            Toast.makeText(this, "无法确定本机到对端的路由地址，请检查 Wi-Fi 连接",
                    Toast.LENGTH_LONG).show();
            return;
        }
        if (localIp.equals(peerIp)) {
            if (peerId.isEmpty()) {
                Toast.makeText(this, "对方与本机地址相同，请先让两端自动发现一次",
                        Toast.LENGTH_LONG).show();
                return;
            }
        }
        String selfId = SpeedCore.nativeDiscSelfId();
        role = decideRole(localIp, peerIp, selfId, peerId);
        logi("LanSpeedRole", "local=" + localIp + " peer=" + peerIp
                + " role=" + roleName()
                + " selfId=" + selfId + " peerId=" + peerId);

        boolean pipeline = pipelineCheck.isChecked();
        for (int i = 0; i < STREAMS; i++) {
            sessions[i] = SpeedCore.nativeCreate(role, peerIp, PORT + i, SECONDS);
            if (sessions[i] == 0) {
                destroySessions();
                Toast.makeText(this, "创建会话失败，请查看 logcat", Toast.LENGTH_LONG).show();
                return;
            }
            SpeedCore.nativeSetPipeline(sessions[i], pipeline);
        }
        for (int i = 0; i < STREAMS; i++) {
            if (SpeedCore.nativeStart(sessions[i]) != 0) {
                destroySessions();
                Toast.makeText(this, "启动测速失败", Toast.LENGTH_LONG).show();
                return;
            }
        }

        running = true;
        setVisualEnabled(startBtn, false);
        setVisualEnabled(stopBtn, true);
        setVisualEnabled(peerIpEdit, false);
        setVisualEnabled(scanBtn, false);
        setVisualEnabled(pipelineCheck, false);
        resetUiStats();
        acquireWifiLock();
        startKeepAwake();
        startWifiMonitor();
        phaseView.setText(role == SpeedCore.ROLE_PHONE
                ? "等待对方连接…" : "正在连接对方…");
        phaseView.setTextColor(getColor(R.color.accent));
        speedView.setText("0.00");
        speedSub.setText("≈ 0.00 MB/s");
        progress.setProgress(0);
        resultUp.setText(dirUp() + "    --");
        resultDown.setText(dirDown() + "    --");
        hintView.setText(role == SpeedCore.ROLE_PHONE
                ? "本机为监听端，正在监听 5201-5204，等待对方连接。"
                : "本机为连接端，正在连接对方 5201-5204。");
        updateLocalInfo(peerIp, localIp);
        handler.removeCallbacks(pollTask);
        handler.postDelayed(pollTask, TICK_MS);
    }

    private void stopTest() {
        if (!running || !anySession())
            return;
        running = false;
        handler.removeCallbacks(pollTask);
        stopWifiMonitor();
        releaseWifiLock();
        stopKeepAwake();
        setVisualEnabled(startBtn, false);
        setVisualEnabled(stopBtn, false);

        final long[] hs = new long[STREAMS];
        for (int i = 0; i < STREAMS; i++) {
            hs[i] = sessions[i];
            sessions[i] = 0;
        }
        new Thread(() -> {
            for (long h : hs) {
                if (h != 0)
                    SpeedCore.nativeStop(h);
            }
            for (long h : hs) {
                if (h != 0)
                    SpeedCore.nativeDestroy(h);
            }
            runOnUiThread(() -> {
                if (destroyed)
                    return;
                phaseView.setText("就绪");
                phaseView.setTextColor(getColor(R.color.text));
                speedView.setText("0.00");
                speedSub.setText("≈ 0.00 MB/s");
                progress.setProgress(0);
                hintView.setText("已停止。可再次点击开始测速。");
                setVisualEnabled(startBtn, true);
                setVisualEnabled(stopBtn, false);
                setVisualEnabled(peerIpEdit, true);
                setVisualEnabled(scanBtn, true);
                setVisualEnabled(pipelineCheck, true);
            });
        }, "stop-test").start();
    }

    /* One stream runs two half-tests, so its share of the bar is [0,0.5) for the
     * uplink and [0.5,1] for the downlink. Averaging the raw per-phase values
     * makes the bar step back every time a stream resets for its next phase or
     * finishes and drops out of the average. */
    private static double wholeTestProgress(int phase, double phaseProgress) {
        if (phase == 3)
            return 0.5 * phaseProgress;
        if (phase == 4)
            return 0.5 + 0.5 * phaseProgress;
        if (phase == 5 || phase == 6)
            return 1.0;   /* an errored stream is finished too: nothing more arrives */
        return 0.0;
    }

    private void pollStatus() {
        if (destroyed || !anySession())
            return;
        long t0 = System.nanoTime();
        int n = 0, minPhase = 99;
        boolean anyRunning = false, anyErr = false, allDone = true;
        int upCount = 0, downCount = 0, errCount = 0, doneCount = 0;
        double cur = 0, progSum = 0;
        long upBytes = 0, downBytes = 0;
        double upSecs = 0, downSecs = 0;
        String err = "";
        String peer = "";
        String local = "";

        for (int i = 0; i < STREAMS; i++) {
            long h = sessions[i];
            if (h == 0)
                continue;
            String json = SpeedCore.nativeStatus(h);
            if (json == null)
                continue;
            try {
                JSONObject o = new JSONObject(json);
                int phase = o.optInt("phase", 0);
                n++;
                if (o.optInt("running", 0) != 0)
                    anyRunning = true;
                if (phase != 5)
                    allDone = false;
                else
                    doneCount++;
                if (phase == 6) {
                    anyErr = true;
                    errCount++;
                    String e = o.optString("err", "");
                    if (err.isEmpty())
                        err = e;
                }
                if (phase == 3 || phase == 4)
                    cur += o.optDouble("cur", 0);
                progSum += wholeTestProgress(phase, o.optDouble("progress", 0));
                if (phase != 5 && phase != 6 && phase != 0 && phase < minPhase)
                    minPhase = phase;
                if (o.optInt("hasUp", 0) != 0) {
                    upCount++;
                    upBytes += o.optLong("upBytes", 0);
                    upSecs = Math.max(upSecs, o.optDouble("upSecs", 0));
                }
                if (o.optInt("hasDown", 0) != 0) {
                    downCount++;
                    downBytes += o.optLong("downBytes", 0);
                    downSecs = Math.max(downSecs, o.optDouble("downSecs", 0));
                }
                if (peer.isEmpty())
                    peer = o.optString("peer", "");
                if (local.isEmpty())
                    local = o.optString("local", "");
            } catch (Exception ignored) {
            }
        }
        long t1 = System.nanoTime();
        if (n == 0)
            return;

        /* A stream can be starved to nothing for 10 s by a congested radio leg while
         * the other three still move data; failing that one must not throw away the
         * streams that are still transferring. */
        boolean settled = doneCount + errCount == n;
        int phase;
        if (!settled && anyRunning)
            phase = minPhase < 99 ? minPhase : 1;
        else if (anyErr && errCount == n)
            phase = 6;
        else if (allDone || (settled && anyErr))
            phase = 5;
        else if (minPhase < 99)
            phase = minPhase;
        else
            phase = 1;

        boolean hasUp = upCount > 0;
        boolean hasDown = downCount > 0;
        double upMbps = upSecs > 0 ? upBytes * 8.0 / upSecs / 1e6 : 0;
        double downMbps = downSecs > 0 ? downBytes * 8.0 / downSecs / 1e6 : 0;
        double prog = allDone ? 1.0 : progSum / n;

        setPhaseUi(phase);
        double shown = 0;
        if (phase == 3 || phase == 4)
            shown = cur;
        else if (phase == 5)
            shown = hasDown ? downMbps : (hasUp ? upMbps : 0);
        speedView.setText(String.format(Locale.US, "%.2f", shown));
        speedSub.setText(String.format(Locale.US, "≈ %.2f MB/s", shown / 8.0));
        progress.setProgress((int) Math.round(prog * 1000));

        resultUp.setText(formatResult(dirUp(), hasUp, upMbps, upBytes, upSecs));
        resultDown.setText(formatResult(dirDown(), hasDown, downMbps, downBytes, downSecs));
        updateLocalInfo(peer.isEmpty() ? peerIp : peer,
                local.isEmpty() ? localIp : local);
        String hintErr = err;
        if (phase == 5) {
            hintErr = errCount > 0
                    ? "仅 " + (n - errCount) + "/" + n + " 条流完成，其余中断（空口拥塞会把某条流完全饿死）"
                    : (upCount > 0 && upCount < n ? "仅 " + upCount + "/" + n + " 条流完成" : "");
        }
        hintView.setText(hintFor(phase, hintErr));

        recordUiPoll(t0, t1, System.nanoTime());

        if (!anyRunning) {
            running = false;
            handler.removeCallbacks(pollTask);
            stopWifiMonitor();
            releaseWifiLock();
            stopKeepAwake();
            setVisualEnabled(startBtn, true);
            setVisualEnabled(stopBtn, false);
            setVisualEnabled(peerIpEdit, true);
            setVisualEnabled(scanBtn, true);
            setVisualEnabled(pipelineCheck, true);
        } else {
            handler.postDelayed(pollTask, TICK_MS);
        }
    }

    private void setPhaseUi(int phase) {
        switch (phase) {
            case 1:
                phaseView.setText(role == SpeedCore.ROLE_PHONE
                        ? "等待对方连接…" : "正在连接对方…");
                phaseView.setTextColor(getColor(R.color.accent));
                break;
            case 2:
                phaseView.setText("已连接，握手中…");
                phaseView.setTextColor(getColor(R.color.accent));
                break;
            case 3:
                phaseView.setText(dirUp());
                phaseView.setTextColor(getColor(R.color.accent));
                break;
            case 4:
                phaseView.setText(dirDown());
                phaseView.setTextColor(getColor(R.color.accent));
                break;
            case 5:
                phaseView.setText("测速完成");
                phaseView.setTextColor(getColor(R.color.green));
                break;
            case 6:
                phaseView.setText("出错");
                phaseView.setTextColor(getColor(R.color.red));
                break;
            default:
                phaseView.setText("就绪");
                phaseView.setTextColor(getColor(R.color.text));
                break;
        }
    }

    private String formatResult(String label, boolean valid, double mbps,
                                long bytes, double secs) {
        if (!valid)
            return label + "    --";
        return String.format(Locale.US,
                "%s    %.2f Mbps    %.2f MB/s    %.2f s    %.2f GB",
                label, mbps, mbps / 8.0, secs, bytes / 1e9);
    }

    private String hintFor(int phase, String err) {
        switch (phase) {
            case 1:
                if (err.contains("rejected"))
                    return err;
                return role == SpeedCore.ROLE_PHONE
                        ? "本机为监听端，正在监听 5201-5204；请确认对方已开始。"
                        : "本机为连接端，正在连接对方 5201-5204；请确认对方已开始。";
            case 2:
                return "已连接，正在交换参数。";
            case 3:
                return role == SpeedCore.ROLE_PHONE
                        ? "第一阶段：本机持续发送 1 MiB 数据块。"
                        : "第一阶段：对方持续发送 1 MiB 数据块（本机接收）。";
            case 4:
                return role == SpeedCore.ROLE_PHONE
                        ? "第二阶段：对方持续发送 1 MiB 数据块（本机接收）。"
                        : "第二阶段：本机持续发送 1 MiB 数据块。";
            case 5:
                return err.isEmpty() ? "测速完成；结果以接收端实测为准。"
                        : "测速完成；" + err;
            case 6: {
                if (err.isEmpty())
                    return "测速失败";
                if (err.contains("errno 104") || err.contains("connection closed")
                        || err.contains("stalled") || err.contains("send failed")
                        || err.contains("recv failed") || err.contains("control frame")
                        || err.contains("handshake timeout"))
                    return err + "；对端可能熄屏或被系统冻结，请两台设备都保持亮屏并在前台。";
                return err;
            }
            default:
                return "两台设备启动后自动互相发现并填入 IP；各自点击一次“开始测速”。";
        }
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        handler.removeCallbacks(pollTask);
        handler.removeCallbacks(discPollTask);
        stopWifiMonitor();
        releaseWifiLock();
        stopKeepAwake();
        SpeedCore.nativeDiscStop();
        final long[] hs = new long[STREAMS];
        for (int i = 0; i < STREAMS; i++) {
            hs[i] = sessions[i];
            sessions[i] = 0;
        }
        new Thread(() -> {
            for (long h : hs) {
                if (h != 0)
                    SpeedCore.nativeStop(h);
            }
            for (long h : hs) {
                if (h != 0)
                    SpeedCore.nativeDestroy(h);
            }
        }, "destroy-test").start();
        super.onDestroy();
    }
}
