
/// Section: Kernel configuration
// Basic kernel options to change freely.
///

/// EndSection

/// Section: Experimental
// Experimental section. These configurations relate to features not completely
// implemented yet.
///
#define ENABLE_EXPERIMENTAL 0
#ifdef ENABLE_EXPERIMENTAL

/// Option: Use trap pointers
// If enabled, traps include a CODE and DATA reference that is stored
// into the registers CS and DS respectively. These must be updated
// before trap code can run.
///
#define C4IX_USE_TRAP_PTRS 1

#else
#define C4IX_USE_TRAP_PTRS 0
#endif
/// EndSection
