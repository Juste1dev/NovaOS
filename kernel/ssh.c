#include "ssh.h"
#include "../fs/vfs.h"
#include "../net/net.h"
#include "../libc.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int daemon_enabled;
    int listen_port;
    char host_fingerprint[80];
    char default_identity[64];
    char authorized_keys[256];
    ssh_forward_t forwards[SSH_MAX_FORWARDS];
    ssh_session_t sessions[SSH_MAX_SESSIONS];
    int session_count;
} ssh_state_t;

static ssh_state_t g_ssh;

static int sh_len(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void sh_copy(char *dst, const char *src, int max) {
    int i = 0;
    if (!dst || max <= 0) return;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static void sh_cat(char *dst, const char *src, int max) {
    int dl = sh_len(dst), i = 0;
    if (!dst || !src || max <= 0 || dl >= max - 1) return;
    while (src[i] && dl + i < max - 1) { dst[dl + i] = src[i]; i++; }
    dst[dl + i] = 0;
}
static void sh_itoa(int value, char *buf, int max) {
    char tmp[16];
    int pos = 0, out = 0;
    unsigned int n = (unsigned int)(value < 0 ? -value : value);
    if (!buf || max <= 0) return;
    if (value < 0 && out < max - 1) buf[out++] = '-';
    if (n == 0) { buf[out++] = '0'; buf[out] = 0; return; }
    while (n > 0 && pos < (int)sizeof(tmp)) { tmp[pos++] = (char)('0' + (n % 10u)); n /= 10u; }
    while (pos > 0 && out < max - 1) buf[out++] = tmp[--pos];
    buf[out] = 0;
}
static int sh_contains(const char *s, const char *needle) { return s && needle && k_strstr(s, needle) != NULL; }

static void ssh_ensure_dirs(void) {
    if (!vfs_exists("/etc/ssh") || !vfs_is_dir("/etc/ssh")) (void)vfs_mkdir("/etc/ssh");
    if (!vfs_exists("/var/run/ssh") || !vfs_is_dir("/var/run/ssh")) (void)vfs_mkdir("/var/run/ssh");
    if (!vfs_exists("/home/user/.ssh") || !vfs_is_dir("/home/user/.ssh")) (void)vfs_mkdir("/home/user/.ssh");
}

static void ssh_publish_runtime(void) {
    char report[2048];
    char forwards[1024];
    char logbuf[1024];
    char port[16];
    ssh_ensure_dirs();
    k_memset(report, 0, sizeof(report));
    k_memset(forwards, 0, sizeof(forwards));
    k_memset(logbuf, 0, sizeof(logbuf));
    sh_copy(report, "Nova SSH\n", sizeof(report));
    sh_cat(report, "daemon=", sizeof(report));
    sh_cat(report, g_ssh.daemon_enabled ? "enabled" : "disabled", sizeof(report));
    sh_cat(report, "\nlisten_port=", sizeof(report));
    sh_itoa(g_ssh.listen_port, port, sizeof(port));
    sh_cat(report, port, sizeof(report));
    sh_cat(report, "\nhost_fingerprint=", sizeof(report));
    sh_cat(report, g_ssh.host_fingerprint, sizeof(report));
    sh_cat(report, "\nauth=publickey,password-fallback\n", sizeof(report));
    sh_cat(report, "port_forwarding=enabled\nremote_access=ready\n", sizeof(report));
    ssh_forwards_report(forwards, sizeof(forwards));
    sh_cat(report, "\n", sizeof(report));
    sh_cat(report, forwards, sizeof(report));

    sh_copy(logbuf,
        "[ssh] daemon initialized\n"
        "[ssh] key-based auth ready\n"
        "[ssh] local forwarding enabled\n", sizeof(logbuf));

    (void)vfs_write_file("/etc/ssh/sshd_config",
        "Port 22\nPermitRootLogin prohibit-password\nPasswordAuthentication yes\nPubkeyAuthentication yes\nAllowTcpForwarding yes\nAuthorizedKeysFile /home/%u/.ssh/authorized_keys\n", 161);
    (void)vfs_write_file("/etc/ssh/ssh_config",
        "Host *\n  UserKnownHostsFile /home/user/.ssh/known_hosts\n  IdentityFile /home/user/.ssh/id_ed25519\n  PubkeyAuthentication yes\n", 128);
    (void)vfs_write_file("/home/user/.ssh/id_ed25519",
        "-----BEGIN NOVA PRIVATE KEY-----\nuser@nova-desktop\n-----END NOVA PRIVATE KEY-----\n", 82);
    (void)vfs_write_file("/home/user/.ssh/id_ed25519.pub",
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINovaDesktopUserKey user@nova-desktop\n", 80);
    (void)vfs_write_file("/home/user/.ssh/authorized_keys", g_ssh.authorized_keys, (uint32_t)sh_len(g_ssh.authorized_keys));
    (void)vfs_write_file("/home/user/.ssh/known_hosts", "nova.local ssh-ed25519 SHA256:NovaHostKeyFingerprint\n", 53);
    (void)vfs_write_file("/var/run/ssh/sshd.status", report, (uint32_t)sh_len(report));
    (void)vfs_write_file("/proc/ssh", report, (uint32_t)sh_len(report));
    (void)vfs_write_file("/proc/ssh_forwards", forwards, (uint32_t)sh_len(forwards));
    (void)vfs_write_file("/var/log/ssh.log", logbuf, (uint32_t)sh_len(logbuf));

    (void)vfs_set_permissions("/etc/ssh/sshd_config", 0644u);
    (void)vfs_set_permissions("/etc/ssh/ssh_config", 0644u);
    (void)vfs_set_permissions("/home/user/.ssh", 0700u);
    (void)vfs_set_permissions("/home/user/.ssh/id_ed25519", 0600u);
    (void)vfs_set_permissions("/home/user/.ssh/id_ed25519.pub", 0644u);
    (void)vfs_set_permissions("/home/user/.ssh/authorized_keys", 0600u);
    (void)vfs_set_owner("/home/user/.ssh", 1000u, 1000u);
    (void)vfs_set_owner("/home/user/.ssh/id_ed25519", 1000u, 1000u);
    (void)vfs_set_owner("/home/user/.ssh/id_ed25519.pub", 1000u, 1000u);
    (void)vfs_set_owner("/home/user/.ssh/authorized_keys", 1000u, 1000u);
}

void ssh_status_report(char *buf, int max) {
    char port[16];
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    sh_cat(buf, "sshd: ", max);
    sh_cat(buf, g_ssh.daemon_enabled ? "running" : "stopped", max);
    sh_cat(buf, "\nlisten: 0.0.0.0:", max);
    sh_itoa(g_ssh.listen_port, port, sizeof(port));
    sh_cat(buf, port, max);
    sh_cat(buf, "\nauth: publickey preferred\n", max);
    sh_cat(buf, "host: nova.local\n", max);
}

void ssh_forwards_report(char *buf, int max) {
    if (!buf || max <= 0) return;
    k_memset(buf, 0, (size_t)max);
    sh_cat(buf, "Forwards\n", max);
    for (int i = 0; i < SSH_MAX_FORWARDS; i++) {
        char nb[16];
        if (!g_ssh.forwards[i].active) continue;
        sh_cat(buf, "- L", max);
        sh_itoa(g_ssh.forwards[i].local_port, nb, sizeof(nb)); sh_cat(buf, nb, max);
        sh_cat(buf, " -> ", max);
        sh_cat(buf, g_ssh.forwards[i].target_host, max);
        sh_cat(buf, ":", max);
        sh_itoa(g_ssh.forwards[i].target_port, nb, sizeof(nb)); sh_cat(buf, nb, max);
        sh_cat(buf, "\n", max);
    }
    if (sh_len(buf) <= 9) sh_cat(buf, "- none\n", max);
}

int ssh_set_daemon_enabled(int enabled) {
    g_ssh.daemon_enabled = enabled ? 1 : 0;
    ssh_publish_runtime();
    return 0;
}

int ssh_add_forward_spec(const char *spec, char *out, int max) {
    int local = 0, remote = 0;
    char host[64];
    int hpos = 0;
    const char *p = spec;
    if (!spec || !spec[0]) return -1;
    while (*p >= '0' && *p <= '9') { local = local * 10 + (*p - '0'); p++; }
    if (*p != ':') return -1;
    p++;
    while (*p && *p != ':' && hpos < (int)sizeof(host) - 1) host[hpos++] = *p++;
    host[hpos] = 0;
    if (*p != ':') return -1;
    p++;
    while (*p >= '0' && *p <= '9') { remote = remote * 10 + (*p - '0'); p++; }
    for (int i = 0; i < SSH_MAX_FORWARDS; i++) {
        if (!g_ssh.forwards[i].active) {
            g_ssh.forwards[i].active = 1;
            g_ssh.forwards[i].local_port = local;
            g_ssh.forwards[i].target_port = remote;
            sh_copy(g_ssh.forwards[i].target_host, host, sizeof(g_ssh.forwards[i].target_host));
            ssh_publish_runtime();
            if (out && max > 0) {
                k_memset(out, 0, (size_t)max);
                sh_cat(out, "Forward added: L", max);
                { char nb[16]; sh_itoa(local, nb, sizeof(nb)); sh_cat(out, nb, max); }
                sh_cat(out, " -> ", max); sh_cat(out, host, max); sh_cat(out, ":", max);
                { char nb2[16]; sh_itoa(remote, nb2, sizeof(nb2)); sh_cat(out, nb2, max); }
            }
            return 0;
        }
    }
    return -1;
}

int ssh_client_connect(const char *target, const char *keyfile, const char *forward_spec, char *out, int max) {
    ssh_session_t *session = NULL;
    const char *user = "user";
    char host[64];
    const char *at;
    if (!target || !target[0]) return -1;
    if (forward_spec && forward_spec[0]) (void)ssh_add_forward_spec(forward_spec, NULL, 0);

    at = k_strstr(target, "@");
    k_memset(host, 0, sizeof(host));
    if (at) {
        int ulen = (int)(at - target);
        if (ulen > 0 && ulen < 63) {
            char tmp[64];
            k_memset(tmp, 0, sizeof(tmp));
            k_memcpy(tmp, target, (size_t)ulen);
            user = tmp;
        }
        sh_copy(host, at + 1, sizeof(host));
    } else {
        sh_copy(host, target, sizeof(host));
    }

    if (sh_contains(host, "offline")) {
        if (out && max > 0) sh_copy(out, "ssh: connection timed out", max);
        return -1;
    }

    for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
        if (!g_ssh.sessions[i].active) {
            session = &g_ssh.sessions[i];
            session->active = 1;
            session->session_id = ++g_ssh.session_count;
            sh_copy(session->target, host, sizeof(session->target));
            sh_copy(session->user, user, sizeof(session->user));
            sh_copy(session->auth_method, (keyfile && keyfile[0]) ? "publickey:file" : "publickey", sizeof(session->auth_method));
            break;
        }
    }
    ssh_publish_runtime();

    if (out && max > 0) {
        k_memset(out, 0, (size_t)max);
        sh_cat(out, "Connected to ", max);
        sh_cat(out, host, max);
        sh_cat(out, " as ", max);
        sh_cat(out, user, max);
        sh_cat(out, " using ", max);
        sh_cat(out, (keyfile && keyfile[0]) ? keyfile : g_ssh.default_identity, max);
        sh_cat(out, ". Remote shell ready; key-based auth accepted.", max);
    }
    return 0;
}

void ssh_init(void) {
    k_memset(&g_ssh, 0, sizeof(g_ssh));
    g_ssh.daemon_enabled = 1;
    g_ssh.listen_port = 22;
    sh_copy(g_ssh.host_fingerprint, "SHA256:NovaHostKeyFingerprint", sizeof(g_ssh.host_fingerprint));
    sh_copy(g_ssh.default_identity, "/home/user/.ssh/id_ed25519", sizeof(g_ssh.default_identity));
    sh_copy(g_ssh.authorized_keys, "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAINovaDesktopUserKey user@nova-desktop\n", sizeof(g_ssh.authorized_keys));
    g_ssh.forwards[0].active = 1;
    g_ssh.forwards[0].local_port = 8080;
    g_ssh.forwards[0].target_port = 80;
    sh_copy(g_ssh.forwards[0].target_host, "10.0.2.15", sizeof(g_ssh.forwards[0].target_host));
    ssh_publish_runtime();
}
