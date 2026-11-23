.PHONY: check-credentials check-file-access
check: check-credentials check-file-access
check-credentials: $(OUT)/tests/credential$(HOSTEXE) $(OUT)/tests/credential-vfs$(HOSTEXE)
	env "$(OUT)/tests/credential$(HOSTEXE)"
	env "$(OUT)/tests/credential-vfs$(HOSTEXE)"

$(OUT)/tests/credential$(HOSTEXE): tests/unit/credential_test.c src/security/credential.c include/kernel/lume/credentials.h include/abi/native/lume/credential.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"

CREDENTIAL_VFS_SOURCES := tests/unit/credential_vfs_test.c src/security/credential.c tests/fixtures/vfs_memory_host.c tests/fixtures/vfs_unrestricted_services.c src/fs/vfs/vfs.c src/fs/path.c src/security/file_modes.c src/security/file_access.c src/fs/fat/fat.c src/storage/block.c vendor/fatfs/ffunicode.c
$(OUT)/tests/credential-vfs$(HOSTEXE): $(CREDENTIAL_VFS_SOURCES) tests/unit/vfs_test.c include/kernel/lume/credentials.h include/abi/native/lume/credential.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG -DLUME_ARCH_AMD64=1 -DLUME_VFS_HOST_ALLOCATOR_PROVIDED=1 -DLUME_VFS_HOST_ALLOCATION_LIMIT=2048 $(TEST_CPPFLAGS) -Ivendor/fatfs $(CREDENTIAL_VFS_SOURCES) -o "$@"

check-file-access: $(OUT)/tests/file-access$(HOSTEXE)
	env "$<"

$(OUT)/tests/file-access$(HOSTEXE): tests/unit/file_access_test.c src/security/file_access.c include/kernel/lume/file_access.h
	mkdir -p "$(@D)"
	$(HOSTCC) $(HOSTCFLAGS) -UNDEBUG $(TEST_CPPFLAGS) $(filter %.c,$^) -o "$@"
