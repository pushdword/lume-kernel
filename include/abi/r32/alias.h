/* Lume OS executable alias format v1. This is not an ELF/CPU ABI change.
 * Only this complete byte string, including both NULs, is accepted. There is
 * no arbitrary path, argument substitution, or recursive alias resolution. */
#ifndef LUME_ALIAS_H
#define LUME_ALIAS_H
#define LUME_BUSYBOX_ALIAS_MAGIC "LUMEAL1"
#define LUME_BUSYBOX_ALIAS_TARGET "/bin/busybox.x32"
#ifdef LUME_ARCH_R32
#undef LUME_BUSYBOX_ALIAS_TARGET
#define LUME_BUSYBOX_ALIAS_TARGET_R32 "/bin/busybox.r32"
#define LUME_BUSYBOX_ALIAS_TARGET LUME_BUSYBOX_ALIAS_TARGET_R32
#endif
#define LUME_BUSYBOX_ALIAS_CONTENT LUME_BUSYBOX_ALIAS_MAGIC "\0" LUME_BUSYBOX_ALIAS_TARGET
#define LUME_BUSYBOX_ALIAS_BYTES sizeof(LUME_BUSYBOX_ALIAS_CONTENT)
#endif
