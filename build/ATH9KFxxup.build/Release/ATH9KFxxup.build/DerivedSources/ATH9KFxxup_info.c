#include <mach/mach_types.h>
 
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t ATH9KFxxup_kern_start(kmod_info_t *ki, void *data);
__private_extern__ kern_return_t ATH9KFxxup_kern_stop(kmod_info_t *ki, void *data);
 
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.inecr.ATH9KFxxup, "1.7.2", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = ATH9KFxxup_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = ATH9KFxxup_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__ ;
