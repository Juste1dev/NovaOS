#ifndef NOVA_SSH_H
#define NOVA_SSH_H

#include <stdint.h>

#define SSH_MAX_FORWARDS 8
#define SSH_MAX_SESSIONS 8

typedef struct {
    int  local_port;
    char target_host[64];
    int  target_port;
    int  active;
} ssh_forward_t;

typedef struct {
    int  session_id;
    char target[64];
    char user[64];
    char auth_method[32];
    int  active;
} ssh_session_t;

void ssh_init(void);
void ssh_status_report(char *buf, int max);
void ssh_forwards_report(char *buf, int max);
int  ssh_set_daemon_enabled(int enabled);
int  ssh_client_connect(const char *target, const char *keyfile, const char *forward_spec, char *out, int max);
int  ssh_add_forward_spec(const char *spec, char *out, int max);

#endif
