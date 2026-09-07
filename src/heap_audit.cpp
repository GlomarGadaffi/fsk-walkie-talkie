// Optional link-time heap audit. Build env t3s3_sx1262_heapaudit wraps
// malloc/calloc/realloc so every allocation made after setup() finishes is
// counted. The count is printed with the RX stats each time the radio
// returns to listen. A steady 0 means the talk/listen hot path never
// touches the heap; anything else names the offender by making it loud.
#include <Arduino.h>
#include "heap_audit.h"

#ifdef HEAP_AUDIT
extern "C" {
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);

volatile uint32_t heap_audit_count = 0;
volatile bool     heap_audit_armed = false;

void* __wrap_malloc(size_t n) {
    if (heap_audit_armed) heap_audit_count++;
    return __real_malloc(n);
}
void* __wrap_calloc(size_t a, size_t b) {
    if (heap_audit_armed) heap_audit_count++;
    return __real_calloc(a, b);
}
void* __wrap_realloc(void* p, size_t n) {
    if (heap_audit_armed) heap_audit_count++;
    return __real_realloc(p, n);
}
}
#endif
