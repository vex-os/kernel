#ifndef USR_VEX_RANDOM_H
#define USR_VEX_RANDOM_H

#define __k_GRND_NONBLOCK 0x0001 /* Fail with EAGAIN while the generator has no seed */
#define __k_GRND_INSECURE 0x0002 /* Do not wait for the first seed; the bytes of an unseeded generator are weak */

#endif /* USR_VEX_RANDOM_H */
