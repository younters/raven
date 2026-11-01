// Exercise releases that bypass the unix table without requiring a GPU.
#include "unix_adapter.m"
#include <assert.h>
void raven_dxmt_wrap_presentation(void **table) { (void)table; }
void raven_mullion_wrap_presentation(void **table) { (void)table; }
@interface TestBuffer : NSObject { unsigned char bytes[16]; }
- (void *)contents;
- (NSUInteger)length;
@end
@implementation TestBuffer
- (void *)contents { return bytes; }
- (NSUInteger)length { return sizeof(bytes); }
@end
int main(void)
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    TestBuffer *buffer = [[TestBuffer alloc] init];
    uint64_t address = (uint64_t)[buffer contents];
    uint64_t info[] = {16, 0, address};
    register_buffer((uint64_t)buffer, info);
    memset((void *)address, 42, 16);
    unsigned char copy[16];
    assert(raven_dxmt_read_memory(address, copy, sizeof(copy)) && copy[15] == 42);
    assert(!raven_dxmt_read_memory(address + 1, copy, sizeof(copy)));
    assert(!raven_dxmt_read_memory(address, (void *)1, 16));
    assert(raven_dxmt_read_memory(address, copy, 16)); // lock and pool survived bad destination
    // allObjects returns an autoreleased array; drain it before the external release.
    [pool drain];
    pool = [[NSAutoreleasePool alloc] init];
    [buffer release];
    assert(!raven_dxmt_read_memory(address, copy, 1));
    [pool drain];
    return 0;
}
