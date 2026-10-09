/* Stand-in for PPSSPP's Common/Log.h: the decoder's messages (av_log in
 * compat.cpp) go to stderr, errors and warnings only. */
#pragma once
#include <cstdio>

namespace Log { enum { ME = 0 }; }

#define ERROR_LOG(cat, ...) do { (void)(cat); std::fprintf(stderr, "atrac: "); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); } while (0)
#define WARN_LOG(cat, ...)  ERROR_LOG(cat, __VA_ARGS__)
#define INFO_LOG(cat, ...)  do { (void)(cat); } while (0)
#define DEBUG_LOG(cat, ...) do { (void)(cat); } while (0)
