/* Fixed BusyBox executable descriptor, including both terminating NULs. */
#ifndef LUME_ALIAS_H
#define LUME_ALIAS_H
#define LUME_BUSYBOX_ALIAS_MAGIC "LUMEAL1"
#define LUME_BUSYBOX_ALIAS_TARGET "/bin/busybox"
#define LUME_BUSYBOX_ALIAS_CONTENT LUME_BUSYBOX_ALIAS_MAGIC "\0" LUME_BUSYBOX_ALIAS_TARGET
#define LUME_BUSYBOX_ALIAS_BYTES sizeof(LUME_BUSYBOX_ALIAS_CONTENT)
#endif
