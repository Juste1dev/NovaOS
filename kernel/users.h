
#ifndef USERS_H
#define USERS_H

#include <stdint.h>

#define MAX_USERS     8
#define MAX_SESSIONS  8
#define MAX_GROUPS    12
#define USERNAME_MAX  64
#define FULLNAME_MAX  128
#define PASSWORD_MAX  128
#define HOME_MAX      256
#define TTY_MAX       32
#define SHELL_MAX     64
#define GROUPNAME_MAX 32
#define GROUPS_MAX    128

typedef enum {
    USER_ROLE_GUEST = 0,
    USER_ROLE_USER,
    USER_ROLE_ADMIN
} user_role_t;

typedef struct {
    int  gid;
    char name[GROUPNAME_MAX];
    char members[GROUPS_MAX];
    int  active;
} group_t;

typedef struct {
    int         uid;
    int         gid;
    char        username[USERNAME_MAX];
    char        fullname[FULLNAME_MAX];
    char        password_hash[PASSWORD_MAX];
    char        home_dir[HOME_MAX];
    char        groups[GROUPS_MAX];
    user_role_t role;
    int         active;
    uint32_t    last_login;
} user_t;

typedef struct {
    int      session_id;
    int      uid;
    int      gid;
    char     username[USERNAME_MAX];
    char     tty[TTY_MAX];
    char     shell[SHELL_MAX];
    uint32_t login_time;
    uint32_t last_seen;
    int      active;
} user_session_t;

typedef struct {
    user_t          users[MAX_USERS];
    int             user_count;
    int             current_uid;
    int             locked;
    uint32_t        lock_time;
    user_session_t  sessions[MAX_SESSIONS];
    int             session_count;
    int             current_session_id;
    group_t         groups[MAX_GROUPS];
    int             group_count;
} user_system_t;

extern user_system_t user_sys;

void users_init(void);
int  users_add(const char *username, const char *fullname, const char *password, user_role_t role);
int  users_add_group(const char *name, int gid, const char *members);
int  users_authenticate(const char *username, const char *password);
int  users_switch_user(const char *username, const char *password);
int  users_set_password(const char *username, const char *password);
int  users_group_gid(const char *name);
void users_groups_for(const char *username, char *buf, int max);
user_t* users_get_current(void);
user_t* users_find(const char *username);
void users_lock_screen(void);
void users_unlock_screen(void);
int  users_is_locked(void);
int  users_create_session(const char *username, const char *tty, const char *shell);
int  users_current_session_id(void);
const user_session_t* users_current_session(void);
void users_publish_runtime(void);

#endif
