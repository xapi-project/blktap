#ifndef _BLKTAP_COMPILER_H
#define _BLKTAP_COMPILER_H

#ifdef __GNUC__
#define likely(_cond)           __builtin_expect(!!(_cond), 1)
#define unlikely(_cond)         __builtin_expect(!!(_cond), 0)
#endif

#ifndef likely
#define likely(_cond)           (_cond)
#endif

#ifndef unlikely
#define unlikely(_cond)         (_cond)
#endif

#ifdef __GNUC__
#define __printf(_f, _a)        __attribute__((format (printf, _f, _a)))
#define __scanf(_f, _a)         __attribute__((format (scanf, _f, _a)))
#define __noreturn              __attribute__((noreturn))
#endif

#ifndef __printf
#define __printf(_f, _a)
#endif

#ifndef __scanf
#define __scanf(_f, _a)
#endif

#ifndef __noreturn
#define __noreturn
#endif

#endif /* _BLKTAP_COMPILER_H */
