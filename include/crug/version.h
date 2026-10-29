/* CrugOS version. The VERSION file is the source of truth; the makefile
 * injects CRUG_VERSION_STR at compile time. Direct compiler invocations get
 * the development fallback so an unstamped build is never mistaken for a
 * release. See docs/versioning.md. */
#ifndef CRUG_VERSION_H
#define CRUG_VERSION_H

#ifndef CRUG_VERSION_STR
#define CRUG_VERSION_STR "0.0.0-dev"
#endif

/* Boot phase tag printed by the loader banner and by `ver` in the shell.
   Kept separate from the version so a version bump does not silently rewrite
   the phase the boot markers and the gate scripts match on. */
#ifndef CRUG_PHASE_STR
#define CRUG_PHASE_STR "u6"
#endif

/* Stringify a numeric macro (useful for generated build ids later). */
#define CRUG_STRINGIFY_RAW(x) #x
#define CRUG_STRINGIFY(x) CRUG_STRINGIFY_RAW(x)

#endif
