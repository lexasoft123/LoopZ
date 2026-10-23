#pragma once
// Keep each bounded reader workspace in its own stack frame. Inlining two
// mutually exclusive large readers into their caller can multiply stack usage.
#if defined(_MSC_VER)
#define LOOPZ_READER_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define LOOPZ_READER_NOINLINE __attribute__((noinline))
#else
#define LOOPZ_READER_NOINLINE
#endif
