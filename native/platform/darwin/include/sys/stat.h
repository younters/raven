#pragma once
#include_next <sys/stat.h>

// Linux's names for the timestamp fields.
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec
