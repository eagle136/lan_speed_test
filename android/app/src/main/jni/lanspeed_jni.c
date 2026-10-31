#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ls_core.h"
#include "ls_disc.h"

#define TAG "LanSpeed"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t o = 0;
    if (cap == 0)
        return;
    for (; src && *src && o + 3 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

JNIEXPORT jlong JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeCreate(JNIEnv *env, jclass cls,
                                               jint role, jstring peer,
                                               jint port, jint seconds)
{
    const char *p;
    char err[192] = "";
    ls_session *s;
    (void)cls;

    if (!peer)
        return 0;
    p = (*env)->GetStringUTFChars(env, peer, NULL);
    if (!p)
        return 0;
    s = ls_session_create((int)role, p, (int)port, (int)seconds,
                          err, sizeof(err));
    (*env)->ReleaseStringUTFChars(env, peer, p);
    if (!s) {
        LOGE("create failed: %s", err);
        return 0;
    }
    return (jlong)(uintptr_t)s;
}

JNIEXPORT void JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeSetPipeline(JNIEnv *env, jclass cls,
                                                    jlong handle, jboolean enabled)
{
    (void)env;
    (void)cls;
    if (handle)
        ls_session_set_pipeline((ls_session *)(uintptr_t)handle,
                                enabled ? 1 : 0);
}

JNIEXPORT jint JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeStart(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env;
    (void)cls;
    if (!handle)
        return -1;
    return ls_session_start((ls_session *)(uintptr_t)handle) == 0 ? 0 : -1;
}

JNIEXPORT void JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeStop(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env;
    (void)cls;
    if (handle)
        ls_session_stop((ls_session *)(uintptr_t)handle);
}

JNIEXPORT void JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDestroy(JNIEnv *env, jclass cls, jlong handle)
{
    (void)env;
    (void)cls;
    if (handle)
        ls_session_destroy((ls_session *)(uintptr_t)handle);
}

JNIEXPORT jstring JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeStatus(JNIEnv *env, jclass cls, jlong handle)
{
    ls_status st;
    char err[400];
    char peer[100];
    char local[100];
    char out[1400];

    (void)cls;
    if (!handle)
        return NULL;
    ls_session_get_status((ls_session *)(uintptr_t)handle, &st);
    json_escape(st.err, err, sizeof(err));
    json_escape(st.peer_ip, peer, sizeof(peer));
    json_escape(st.local_ip, local, sizeof(local));

    snprintf(out, sizeof(out),
        "{\"running\":%d,\"phase\":%d,\"role\":%d,\"connected\":%d,"
        "\"finished\":%d,\"error\":%d,\"port\":%d,\"seconds\":%d,"
        "\"cur\":%.3f,\"progress\":%.4f,"
        "\"hasUp\":%d,\"hasDown\":%d,\"upMbps\":%.3f,\"downMbps\":%.3f,"
        "\"upBytes\":%llu,\"downBytes\":%llu,\"upSecs\":%.6f,\"downSecs\":%.6f,"
        "\"err\":\"%s\",\"peer\":\"%s\",\"local\":\"%s\"}",
        st.running, st.phase, st.role, st.connected,
        st.finished, st.error, st.port, st.seconds,
        st.cur_mbps, st.progress,
        st.has_up, st.has_down, st.up_mbps, st.down_mbps,
        (unsigned long long)st.up_bytes,
        (unsigned long long)st.down_bytes,
        st.up_secs, st.down_secs,
        err, peer, local);
    return (*env)->NewStringUTF(env, out);
}

/* Returns "ip\tname\tid" for the most recent peer, or NULL. */
static jstring peer_to_string(JNIEnv *env, const ls_peer_info *p)
{
    char out[200];
    snprintf(out, sizeof(out), "%s\t%s\t%s", p->ip, p->name, p->id);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jint JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDiscStart(JNIEnv *env, jclass cls,
                                                  jint udp_port, jint tcp_port,
                                                  jstring name)
{
    const char *n;
    jint rc;
    (void)cls;
    if (!name)
        return -1;
    n = (*env)->GetStringUTFChars(env, name, NULL);
    if (!n)
        return -1;
    rc = ls_disc_start((int)udp_port, (int)tcp_port, n) == 0 ? 0 : -1;
    (*env)->ReleaseStringUTFChars(env, name, n);
    return rc;
}

JNIEXPORT void JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeSetLog(JNIEnv *env, jclass cls,
                                               jboolean enabled)
{
    (void)env;
    (void)cls;
    ls_set_log_enabled(enabled ? 1 : 0);
}

JNIEXPORT void JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDiscStop(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    ls_disc_stop();
}

JNIEXPORT jstring JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDiscScan(JNIEnv *env, jclass cls,
                                                 jint timeout_ms)
{
    ls_peer_info p;
    (void)cls;
    if (ls_disc_scan((int)timeout_ms, &p) != 0)
        return NULL;
    return peer_to_string(env, &p);
}

JNIEXPORT jstring JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDiscLatest(JNIEnv *env, jclass cls)
{
    ls_peer_info p;
    (void)cls;
    if (ls_disc_latest(&p) != 0)
        return NULL;
    return peer_to_string(env, &p);
}

JNIEXPORT jstring JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeDiscSelfId(JNIEnv *env, jclass cls)
{
    char id[40];
    (void)cls;
    if (ls_disc_self_id(id, sizeof(id)) != 0)
        return NULL;
    return (*env)->NewStringUTF(env, id);
}

JNIEXPORT jstring JNICALL
Java_com_eagle_lanspeed_SpeedCore_nativeLocalIp(JNIEnv *env, jclass cls,
                                                jstring peer, jint port)
{
    const char *p;
    char out[64];
    (void)cls;

    if (!peer)
        return NULL;
    p = (*env)->GetStringUTFChars(env, peer, NULL);
    if (!p)
        return NULL;
    if (ls_local_ip_for_peer(p, (int)port, out, sizeof(out)) != 0)
        out[0] = '\0';
    (*env)->ReleaseStringUTFChars(env, peer, p);
    if (!out[0])
        return NULL;
    return (*env)->NewStringUTF(env, out);
}
