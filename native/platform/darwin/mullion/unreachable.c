/* Mullion references Wine handle operations only in event/listener entries
 * replaced by Raven's table adapter. Reaching either is an ABI mismatch. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
int32_t NtClose(void *handle)
{
    (void)handle;
    fputs("raven: unwrapped Mullion NtClose\n", stderr);
    abort();
}
int32_t NtDuplicateObject(void *source_process, void *source, void *target_process,
                          void **target, uint32_t access, uint32_t attributes, uint32_t options)
{
    (void)source_process; (void)source; (void)target_process; (void)target;
    (void)access; (void)attributes; (void)options;
    fputs("raven: unwrapped Mullion NtDuplicateObject\n", stderr);
    abort();
}
