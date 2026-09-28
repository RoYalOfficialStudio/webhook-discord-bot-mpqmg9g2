/* Single translation unit that compiles the vendored miniaudio implementation.
   Only the pieces RoY Studio needs are enabled. */
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
