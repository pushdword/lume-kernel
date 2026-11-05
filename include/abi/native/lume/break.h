#ifndef LUME_NATIVE_BREAK_H
#define LUME_NATIVE_BREAK_H

/* BRK takes the requested end, flags and zero. A zero end queries the current break.
 * Zero flags retain the selected heap. EXTENDED selects the high AArch64 heap if
 * the low heap is empty. A nonempty low heap returns EBUSY. Growth allocates physical
 * pages. A failed request retains the previous break and heap selection. */
#define LUME_BRK_EXTENDED 1u

#endif
