#ifndef XNGEN_COMPILER_H
#define XNGEN_COMPILER_H

#include <stdint.h>

#include "jitee.h"

// Compiles one method for the zapper, or declines it with CORJIT_SKIPPED.
int32_t xngen_compile(ICorJitInfo *info, CORINFO_METHOD_INFO *method, uint32_t flags, uint8_t **entry,
	uint32_t *code_bytes);
// Logs how many methods were compiled and declined.
void xngen_report(void);

#endif
