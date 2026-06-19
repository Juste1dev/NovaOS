#include "users.h"
#include "../fs/vfs.h"
#include "../libc.h"
#include "../kernel/timer.h"
#include <stdint.h>
#include <stddef.h>

user_system_t user_sys;

static int u_strlen(const char *s) { int n = 0; while (s && s[n]) n++; return n; }
static void u_strcpy(char *d, const char *s, int m) {
    int i = 0;
    if (!d || m <= 0) return;
    while (s && s[i] && i < m - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static void u_strcat(char *d, const char *s, int m) {
    int i = u_strlen(d), j = 0;
    if (!d || !s || m <= 0 || i >= m - 1) return;
    while (s[j] && i + j < m - 1) { d[i + j] = s[j]; j++; }
    d[i + j] = 0;
}
static int u_streq(const char *a, const char *b) {
    return k_strcmp(a ? a : "", b ? b : "") == 0;
}

static void hash_password(const char *pass, char *out, int maxlen) {
    int len = u_strlen(pass);
    if (len >= maxlen) len = maxlen - 1;
    for (int i = 0; i < len; i++) out[i] = (char)((uint8_t)pass[i] ^ (uint8_t)(0xA5 + i * 17));
    out[len] = 0;
}

static int hash_compare(const char *pass, const char *hash) {
    char test[PASSWORD_MAX];
    hash_password(pass, test, PASSWORD_MAX);
    return !k_strcmp(test, hash);
}

static group_t *group_find(const char *name) {
    for (int i = 0; i < user_sys.group_count; i++) {
        if (user_sys.groups[i].active && u_streq(user_sys.groups[i].name, name)) return &user_sys.groups[i];
    }
    return NULL;
}

int users_group_gid(const char *name) {
    group_t *g = group_find(name);
    return g ? g->gid : -1;
}

int users_add_group(const char *name, int gid, const char *members) {
    group_t *g;
    if (user_sys.group_count >= MAX_GROUPS) return -1;
    g = &user_sys.groups[user_sys.group_count++];
    k_memset(g, 0, sizeof(*g));
    g->gid = gid;
    g->active = 1;
    u_strcpy(g->name, name, GROUPNAME_MAX);
    u_strcpy(g->members, members ? members : "", GROUPS_MAX);
    return gid;
}

static void users_apply_vfs_metadata(const char *path, uint32_t mode, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = vfs_get_node(path);
    if (!n) return;
    n->permissions = mode;
    n->owner_uid = uid;
    n->owner_gid = gid;
}

void users_groups_for(const char *username, char *buf, int max) {
    int first = 1;
    if (!buf || max <= 0) return;
    buf[0] = 0;
    for (int i = 0; i < user_sys.group_count; i++) {
        group_t *g = &user_sys.groups[i];
        if (!g->active) continue;
        if (u_streq(username, "root") && g->gid == 0) {
            if (!first) u_strcat(buf, " ", max);
            u_strcat(buf, g->name, max);
            first = 0;
            continue;
        }
        if (k_strstr(g->members, username)) {
            if (!first) u_strcat(buf, " ", max);
            u_strcat(buf, g->name, max);
            first = 0;
        }
    }
}

void users_publish_runtime(void) {
    char passwd[1024];
    char shadow[1024];
    char groups[1024];
    char sessions[1024];
    char current[256];
    char secdoc[2048];

    if (!vfs_exists("/var") || !vfs_is_dir("/var")) (void)vfs_mkdir("/var");
    if (!vfs_exists("/var/run") || !vfs_is_dir("/var/run")) (void)vfs_mkdir("/var/run");
    if (!vfs_exists("/var/run/sessions") || !vfs_is_dir("/var/run/sessions")) (void)vfs_mkdir("/var/run/sessions");
    if (!vfs_exists("/etc") || !vfs_is_dir("/etc")) (void)vfs_mkdir("/etc");
    if (!vfs_exists("/home/admin") || !vfs_is_dir("/home/admin")) (void)vfs_mkdir("/home/admin");
    if (!vfs_exists("/home/guest") || !vfs_is_dir("/home/guest")) (void)vfs_mkdir("/home/guest");
    if (!vfs_exists("/home/user/.ssh") || !vfs_is_dir("/home/user/.ssh")) (void)vfs_mkdir("/home/user/.ssh");

    k_memset(passwd, 0, sizeof(passwd));
    k_memset(shadow, 0, sizeof(shadow));
    k_memset(groups, 0, sizeof(groups));
    k_memset(sessions, 0, sizeof(sessions));
    k_memset(current, 0, sizeof(current));
    k_memset(secdoc, 0, sizeof(secdoc));

    for (int i = 0; i < user_sys.user_count; i++) {
        char line[256];
        char nb_uid[16];
        char nb_gid[16];
        char membuf[GROUPS_MAX];
        int uid = user_sys.users[i].uid;
        int gid = user_sys.users[i].gid;
        int pos = 0;
        char tmp[16];

        k_memset(line, 0, sizeof(line));
        k_memset(nb_uid, 0, sizeof(nb_uid));
        k_memset(nb_gid, 0, sizeof(nb_gid));
        k_memset(membuf, 0, sizeof(membuf));

        if (uid == 0) { nb_uid[0] = '0'; nb_uid[1] = 0; }
        else {
            while (uid > 0 && pos < 15) { tmp[pos++] = (char)('0' + (uid % 10)); uid /= 10; }
            for (int j = 0; j < pos; j++) nb_uid[j] = tmp[pos - 1 - j];
            nb_uid[pos] = 0;
        }
        pos = 0;
        if (gid == 0) { nb_gid[0] = '0'; nb_gid[1] = 0; }
        else {
            while (gid > 0 && pos < 15) { tmp[pos++] = (char)('0' + (gid % 10)); gid /= 10; }
            for (int j = 0; j < pos; j++) nb_gid[j] = tmp[pos - 1 - j];
            nb_gid[pos] = 0;
        }

        users_groups_for(user_sys.users[i].username, membuf, sizeof(membuf));

        u_strcat(line, user_sys.users[i].username, sizeof(line));
        u_strcat(line, ":x:", sizeof(line));
        u_strcat(line, nb_uid, sizeof(line));
        u_strcat(line, ":", sizeof(line));
        u_strcat(line, nb_gid, sizeof(line));
        u_strcat(line, ":", sizeof(line));
        u_strcat(line, user_sys.users[i].fullname, sizeof(line));
        u_strcat(line, ":", sizeof(line));
        u_strcat(line, user_sys.users[i].home_dir, sizeof(line));
        u_strcat(line, ":/usr/bin/nova-shell\n", sizeof(line));
        u_strcat(passwd, line, sizeof(passwd));

        k_memset(line, 0, sizeof(line));
        u_strcat(line, user_sys.users[i].username, sizeof(line));
        u_strcat(line, ":", sizeof(line));
        u_strcat(line, user_sys.users[i].password_hash, sizeof(line));
        u_strcat(line, ":19000:0:99999:7:::\n", sizeof(line));
        u_strcat(shadow, line, sizeof(shadow));

        k_memset(line, 0, sizeof(line));
        u_strcat(line, user_sys.users[i].username, sizeof(line));
        u_strcat(line, " -> ", sizeof(line));
        u_strcat(line, membuf, sizeof(line));
        u_strcat(line, "\n", sizeof(line));
        u_strcat(secdoc, line, sizeof(secdoc));
    }

    for (int i = 0; i < user_sys.group_count; i++) {
        char line[256];
        char nb_gid[16];
        int gid = user_sys.groups[i].gid;
        int pos = 0;
        char tmp[16];
        if (!user_sys.groups[i].active) continue;
        k_memset(line, 0, sizeof(line));
        k_memset(nb_gid, 0, sizeof(nb_gid));
        if (gid == 0) { nb_gid[0] = '0'; nb_gid[1] = 0; }
        else {
            while (gid > 0 && pos < 15) { tmp[pos++] = (char)('0' + (gid % 10)); gid /= 10; }
            for (int j = 0; j < pos; j++) nb_gid[j] = tmp[pos - 1 - j];
            nb_gid[pos] = 0;
        }
        u_strcat(line, user_sys.groups[i].name, sizeof(line));
        u_strcat(line, ":x:", sizeof(line));
        u_strcat(line, nb_gid, sizeof(line));
        u_strcat(line, ":", sizeof(line));
        u_strcat(line, user_sys.groups[i].members, sizeof(line));
        u_strcat(line, "\n", sizeof(line));
        u_strcat(groups, line, sizeof(groups));
    }

    for (int i = 0; i < user_sys.session_count; i++) {
        char line[256];
        char nb[16];
        int value = user_sys.sessions[i].session_id;
        int pos = 0;
        char tmp[16];
        k_memset(line, 0, sizeof(line));
        if (value == 0) { nb[0] = '0'; nb[1] = 0; }
        else {
            while (value > 0 && pos < 15) { tmp[pos++] = (char)('0' + (value % 10)); value /= 10; }
            for (int j = 0; j < pos; j++) nb[j] = tmp[pos - 1 - j];
            nb[pos] = 0;
        }
        u_strcat(line, "sid=", sizeof(line)); u_strcat(line, nb, sizeof(line));
        u_strcat(line, " user=", sizeof(line)); u_strcat(line, user_sys.sessions[i].username, sizeof(line));
        u_strcat(line, " tty=", sizeof(line)); u_strcat(line, user_sys.sessions[i].tty, sizeof(line));
        u_strcat(line, " shell=", sizeof(line)); u_strcat(line, user_sys.sessions[i].shell, sizeof(line));
        u_strcat(line, "\n", sizeof(line));
        u_strcat(sessions, line, sizeof(sessions));
    }

    if (user_sys.current_session_id >= 0) {
        const user_session_t *cur = users_current_session();
        if (cur) {
            u_strcat(current, "sid=", sizeof(current));
            {
                int value = cur->session_id;
                int pos = 0; char tmp[16]; char nb[16];
                if (value == 0) { nb[0] = '0'; nb[1] = 0; }
                else {
                    while (value > 0 && pos < 15) { tmp[pos++] = (char)('0' + (value % 10)); value /= 10; }
                    for (int j = 0; j < pos; j++) nb[j] = tmp[pos - 1 - j];
                    nb[pos] = 0;
                }
                u_strcat(current, nb, sizeof(current));
            }
            u_strcat(current, "\nuser=", sizeof(current));
            u_strcat(current, cur->username, sizeof(current));
            u_strcat(current, "\nshell=", sizeof(current));
            u_strcat(current, cur->shell, sizeof(current));
            u_strcat(current, "\n", sizeof(current));
        }
    }

    u_strcat(secdoc,
        "\nPermissions clefs\n"
        "- /etc/passwd 0644 root:root\n"
        "- /etc/shadow 0600 root:root\n"
        "- /home/user/.ssh 0700 user:users\n"
        "- /home/user/.ssh/authorized_keys 0600 user:users\n"
        "- /usr/bin/passwd 4755 root:root (SUID)\n"
        "- /usr/bin/remote-shell 2755 root:ssh (SGID)\n", sizeof(secdoc));

    (void)vfs_write_file("/etc/passwd", passwd, (uint32_t)u_strlen(passwd));
    (void)vfs_write_file("/etc/shadow", shadow, (uint32_t)u_strlen(shadow));
    (void)vfs_write_file("/etc/group", groups, (uint32_t)u_strlen(groups));
    (void)vfs_write_file("/etc/shells", "/usr/bin/nova-shell\n/usr/bin/shell\n", 33);
    (void)vfs_write_file("/usr/bin/passwd", "#!/nova/passwd\nmode=setuid\n", 28);
    (void)vfs_write_file("/usr/bin/remote-shell", "#!/nova/remote-shell\nmode=setgid-ssh\n", 39);
    (void)vfs_write_file("/var/run/sessions/list", sessions, (uint32_t)u_strlen(sessions));
    (void)vfs_write_file("/var/run/sessions/current", current, (uint32_t)u_strlen(current));
    (void)vfs_write_file("/home/user/Documents/Accounts.txt", secdoc, (uint32_t)u_strlen(secdoc));

    users_apply_vfs_metadata("/etc/passwd", 0644u, 0u, 0u);
    users_apply_vfs_metadata("/etc/group", 0644u, 0u, 0u);
    users_apply_vfs_metadata("/etc/shadow", 0600u, 0u, 0u);
    users_apply_vfs_metadata("/home/user", 0750u, 1000u, 1000u);
    users_apply_vfs_metadata("/home/user/.ssh", 0700u, 1000u, 1000u);
    users_apply_vfs_metadata("/home/user/.ssh/authorized_keys", 0600u, 1000u, 1000u);
    users_apply_vfs_metadata("/usr/bin/passwd", 04755u, 0u, 0u);
    users_apply_vfs_metadata("/usr/bin/remote-shell", 02755u, 0u, 1003u);
}

void users_init(void) {
    k_memset(&user_sys, 0, sizeof(user_system_t));
    user_sys.current_uid = -1;
    user_sys.current_session_id = -1;
    user_sys.locked = 0;

    users_add_group("root", 0, "admin");
    users_add_group("users", 1000, "user");
    users_add_group("video", 1001, "admin,user");
    users_add_group("audio", 1002, "admin,user");
    users_add_group("ssh", 1003, "admin,user");
    users_add_group("guest", 65534, "guest");

    users_add("admin", "Administrateur", "admin", USER_ROLE_ADMIN);
    users_add("user", "Utilisateur", "1234", USER_ROLE_USER);
    users_add("guest", "Invité", "guest", USER_ROLE_GUEST);
    users_publish_runtime();
}

int users_add(const char *username, const char *fullname, const char *password, user_role_t role) {
    user_t *u;
    if (user_sys.user_count >= MAX_USERS) return -1;
    u = &user_sys.users[user_sys.user_count];
    k_memset(u, 0, sizeof(user_t));
    u->uid = user_sys.user_count + 1000;
    u->gid = 1000;
    if (role == USER_ROLE_ADMIN) { u->uid = 0; u->gid = 0; }
    else if (role == USER_ROLE_GUEST) { u->uid = 65534; u->gid = 65534; }
    u_strcpy(u->username, username, USERNAME_MAX);
    u_strcpy(u->fullname, fullname, FULLNAME_MAX);
    hash_password(password, u->password_hash, PASSWORD_MAX);
    u->role = role;
    u->active = 1;
    u_strcpy(u->home_dir, "/home/", HOME_MAX);
    u_strcat(u->home_dir, username, HOME_MAX);
    if (role == USER_ROLE_ADMIN) u_strcpy(u->groups, "root video audio ssh", GROUPS_MAX);
    else if (role == USER_ROLE_GUEST) u_strcpy(u->groups, "guest", GROUPS_MAX);
    else u_strcpy(u->groups, "users video audio ssh", GROUPS_MAX);
    user_sys.user_count++;
    return u->uid;
}

int users_create_session(const char *username, const char *tty, const char *shell) {
    user_t *u = users_find(username);
    user_session_t *s;
    if (!u || user_sys.session_count >= MAX_SESSIONS) return -1;
    s = &user_sys.sessions[user_sys.session_count];
    k_memset(s, 0, sizeof(*s));
    s->session_id = user_sys.session_count + 1;
    s->uid = u->uid;
    s->gid = u->gid;
    u_strcpy(s->username, u->username, USERNAME_MAX);
    u_strcpy(s->tty, tty && tty[0] ? tty : "/dev/tty0", TTY_MAX);
    u_strcpy(s->shell, shell && shell[0] ? shell : "/usr/bin/nova-shell", SHELL_MAX);
    s->login_time = timer_ms();
    s->last_seen = s->login_time;
    s->active = 1;
    user_sys.session_count++;
    user_sys.current_session_id = s->session_id;
    users_publish_runtime();
    return s->session_id;
}

int users_authenticate(const char *username, const char *password) {
    for (int i = 0; i < user_sys.user_count; i++) {
        user_t *u = &user_sys.users[i];
        if (!u->active) continue;
        if (k_strcmp(u->username, username)) continue;
        if (u->role == USER_ROLE_GUEST || hash_compare(password, u->password_hash)) {
            user_sys.current_uid = u->uid;
            u->last_login = timer_ms();
            users_create_session(u->username, "/dev/tty0", "/usr/bin/nova-shell");
            return 1;
        }
        return 0;
    }
    return 0;
}

int users_switch_user(const char *username, const char *password) {
    for (int i = 0; i < user_sys.user_count; i++) {
        user_t *u = &user_sys.users[i];
        if (!u->active || k_strcmp(u->username, username)) continue;
        if (u->role == USER_ROLE_GUEST || !password || !password[0] || hash_compare(password, u->password_hash)) {
            user_sys.current_uid = u->uid;
            u->last_login = timer_ms();
            users_create_session(u->username, "/dev/tty0", "/usr/bin/nova-shell");
            return 1;
        }
        return 0;
    }
    return 0;
}

int users_set_password(const char *username, const char *password) {
    user_t *u = users_find(username);
    if (!u || !password) return 0;
    hash_password(password, u->password_hash, PASSWORD_MAX);
    users_publish_runtime();
    return 1;
}

user_t* users_get_current(void) {
    if (user_sys.current_uid < 0) return NULL;
    for (int i = 0; i < user_sys.user_count; i++) {
        if (user_sys.users[i].uid == user_sys.current_uid) return &user_sys.users[i];
    }
    return NULL;
}

user_t* users_find(const char *username) {
    for (int i = 0; i < user_sys.user_count; i++) if (!k_strcmp(user_sys.users[i].username, username)) return &user_sys.users[i];
    return NULL;
}

void users_lock_screen(void) {
    user_sys.locked = 1;
    user_sys.lock_time = timer_ms();
    users_publish_runtime();
}

void users_unlock_screen(void) {
    user_sys.locked = 0;
    users_publish_runtime();
}

int users_is_locked(void) {
    return user_sys.locked || user_sys.current_uid < 0;
}

int users_current_session_id(void) {
    return user_sys.current_session_id;
}

const user_session_t* users_current_session(void) {
    for (int i = 0; i < user_sys.session_count; ++i) if (user_sys.sessions[i].session_id == user_sys.current_session_id) return &user_sys.sessions[i];
    return NULL;
}
