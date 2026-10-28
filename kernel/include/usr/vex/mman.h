#ifndef USR_VEX_MMAN_H
#define USR_VEX_MMAN_H

#define __k_PROT_NONE  0x0000
#define __k_PROT_READ  0x0001
#define __k_PROT_WRITE 0x0002
#define __k_PROT_EXEC  0x0004

#define __k_MAP_SHARED          0x000001
#define __k_MAP_PRIVATE         0x000002
#define __k_MAP_FIXED           0x000010
#define __k_MAP_ANONYMOUS       0x000020
#define __k_MAP_NORESERVE       0x004000
#define __k_MAP_POPULATE        0x008000
#define __k_MAP_STACK           0x020000
#define __k_MAP_FIXED_NOREPLACE 0x100000

#define __k_MS_ASYNC      0x0001
#define __k_MS_INVALIDATE 0x0002
#define __k_MS_SYNC       0x0004

#endif /* USR_VEX_MMAN_H */
