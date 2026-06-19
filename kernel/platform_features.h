#ifndef NOVA_PLATFORM_FEATURES_H
#define NOVA_PLATFORM_FEATURES_H

void nova_platform_features_init(void);
void nova_platform_summary(char *buf, int max);
void nova_platform_kernel_report(char *buf, int max);
void nova_platform_memory_report(char *buf, int max);
void nova_platform_filesystem_report(char *buf, int max);
void nova_platform_network_report(char *buf, int max);

#endif
