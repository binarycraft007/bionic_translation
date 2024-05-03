#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef __GLIBC__

/* musl has 64bit off_t on 32bit platforms. This is reasonable, but incompatible with bionic/glibc */

#ifndef __LP64__

typedef uint32_t off32_t;

void * bionic_mmap(void *address, size_t length, int protect, int flags, int filedes, off32_t offset)
{
	return mmap(address, length, protect, flags, filedes, offset);
}

int bionic_posix_fallocate(int fd, off32_t offset, off32_t length)
{
	return posix_fallocate(fd, offset, length);
}

off32_t bionic_lseek(int filedes, off32_t offset, int whence)
{
	return (off32_t)lseek(filedes, offset, whence);
}

ssize_t bionic_pread(int filedes, void *buffer, size_t size, off32_t offset)
{
	return pread(filedes, buffer, size, offset);
}

ssize_t bionic_pwrite(int filedes, const void *buffer, size_t size, off32_t offset)
{
	return pwrite(filedes, buffer, size, offset);
}

int bionic_truncate(const char *filename, off32_t length)
{
	return truncate(filename, length);
}

int bionic_ftruncate(int fd, off32_t length)
{
	return ftruncate(fd, length);
}

off32_t bionic_ftello(FILE *stream)
{
	return (off32_t)ftello(stream);
}

int bionic_fseeko(FILE *stream, off32_t offset, int whence)
{
	return fseeko(stream, offset, whence);
}

#endif

#endif
