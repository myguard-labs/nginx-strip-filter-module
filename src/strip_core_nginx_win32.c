/* nginx's MSVC build requires this to be the first include in every source. */
#include <ngx_config.h>

/* Keep strip_core.c directly compilable without nginx on every platform. */
#include "strip_core.c"
