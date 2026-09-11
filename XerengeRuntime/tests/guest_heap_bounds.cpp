#include "guest_heap_bounds.h"

// The malformed audio-effect request previously underflowed limit - size
// and allowed a memset across the mapped game image.
static_assert(!guestHeapAllocationSize(0x79f9a000, 0x80000000, 0x81ff00b8));
static_assert(!guestHeapAllocationSize(0x60000000, 0x80000000, 0xffffffff));
static_assert(!guestHeapAllocationSize(0x80000010, 0x80000000, 16));
static_assert(!guestHeapAllocationSize(0x7ffffff0, 0x80000000, 17));
static_assert(guestHeapAllocationSize(0x7ffffff0, 0x80000000, 16) == 16);
static_assert(guestHeapAllocationSize(0x60000000, 0x80000000, 1) == 16);
int main() {}
