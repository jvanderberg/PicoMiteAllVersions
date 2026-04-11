#include "MMBasic_Includes.h"
#include "Hardware_Includes.h"
#include "vm_sys_input.h"

#ifdef MMBASIC_HOST
extern int host_keydown(int n);
#elif defined(USBKEYBOARD)
extern int KeyDown[7];
#elif defined(PICOCALC)
extern int LocalKeyDown[7];
#endif

int vm_sys_input_keydown(int n) {
    if (n < 0 || n > 8) error("Number out of bounds");

#ifdef MMBASIC_HOST
    return host_keydown(n);
#elif defined(USBKEYBOARD)
    if (n == 8) return 0;
    if (n) return KeyDown[n - 1];
    int count = 0;
    for (int i = 0; i < 6; i++)
        if (KeyDown[i]) count++;
    return count;
#elif defined(PICOCALC)
    if (n == 8) return 0;
    if (n) return LocalKeyDown[n - 1];
    int count = 0;
    for (int i = 0; i < 6; i++)
        if (LocalKeyDown[i]) count++;
    return count;
#else
    return 0;
#endif
}

#if defined(PICOCALC) && !defined(USBKEYBOARD) && !defined(MMBASIC_HOST)
void fun_keydown(void) {
    error("KEYDOWN is VM-only");
}
#endif
