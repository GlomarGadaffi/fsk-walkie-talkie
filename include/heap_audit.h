#pragma once
#include <stdint.h>
#ifdef HEAP_AUDIT
extern "C" {
extern volatile uint32_t heap_audit_count;
extern volatile bool     heap_audit_armed;
}
#define HEAP_AUDIT_ARM()   (heap_audit_armed = true)
#define HEAP_AUDIT_COUNT() (heap_audit_count)
#else
#define HEAP_AUDIT_ARM()   ((void)0)
#define HEAP_AUDIT_COUNT() (0u)
#endif
