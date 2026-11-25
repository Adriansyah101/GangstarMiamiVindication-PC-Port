#ifdef _WIN32
#include "layout.h"

    .section .gmvimg,"bwx"
    .globl _gmv_reserve_start
    .globl _gmv_reserve_end
_gmv_reserve_start:
    .space GMV_RESERVE_END - GMV_RESERVE_START
_gmv_reserve_end:
#else
    .section .note.GNU-stack,"",@progbits
#endif
