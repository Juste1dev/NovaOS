#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SIZE     4096ULL
#define PAGE_PRESENT  0x001ULL
#define PAGE_WRITABLE 0x002ULL
#define PAGE_USER     0x004ULL
#define PAGE_HUGE     0x080ULL

#define NOVA_HEAP_START 0x01000000ULL
#define NOVA_HEAP_SIZE  (64ULL * 1024ULL * 1024ULL)

void     pmm_init(uint64_t mem_upper);
void     pmm_init_region(uint64_t base, uint64_t size);
void     pmm_reserve_region(uint64_t base, uint64_t size);
uint64_t pmm_alloc_block(void);
void     pmm_free_block(uint64_t addr);
uint64_t pmm_total_blocks(void);
uint64_t pmm_used_blocks(void);
uint64_t pmm_available_blocks(void);
void     pmm_report(char *buf, int max);

void     paging_init(void);
void     paging_map_region(uint64_t base, uint64_t size, uint64_t flags);
void     vmm_init(void);
void     vmm_map_region(uint64_t base, uint64_t size, uint64_t flags);
uint64_t vmm_mapped_bytes(void);
uint64_t paging_flags_default(void);
uint64_t vmm_region_count(void);
void     vmm_report(char *buf, int max);

void     heap_init(void);
void*    kmalloc(size_t size);
void*    kcalloc(size_t count, size_t size);
void*    krealloc(void *ptr, size_t size);
void     kfree(void *ptr);
uint64_t kmalloc_early(uint64_t size);
uint64_t heap_used(void);
uint64_t heap_total(void);
uint64_t heap_free_bytes(void);
uint64_t heap_allocation_count(void);
uint64_t heap_free_count(void);
uint64_t heap_failed_allocations(void);
uint64_t heap_active_allocations(void);
uint64_t heap_corruption_events(void);
void     heap_debug_report(char *buf, int max);

extern uint64_t _kernel_end;

#endif
