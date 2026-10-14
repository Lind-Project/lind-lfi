// Panics abort in a sandbox: there is no unwinder, so backtraces are empty.
struct _Unwind_Context;
int _Unwind_Backtrace(int (*fn)(struct _Unwind_Context *, void *), void *arg) { (void) fn; (void) arg; return 5; }
unsigned long _Unwind_GetIP(struct _Unwind_Context *c) { (void) c; return 0; }
