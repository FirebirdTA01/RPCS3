#pragma once

// Multi-process bring-up diagnostic logging.
// Gated at compile time: define RPCS3_MULTIPROC_DEBUG to enable.
// Format args are NOT evaluated when the flag is off, so hot-path
// callers pay zero cost in production builds.

#ifdef RPCS3_MULTIPROC_DEBUG
#define MPDBG_LOG(channel, ...)   channel.notice(__VA_ARGS__)
#else
#define MPDBG_LOG(channel, ...)   ((void)0)
#endif
