// runtime implementations of _FORTIFY_SOURCE _chk functions (some with the checking removed)

#include <sys/select.h>

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

size_t bionic___fwrite_chk(const void * __restrict buf, size_t size, size_t count, FILE * __restrict stream, size_t buf_size)
{
	size_t total;
	if (__builtin_expect(__builtin_mul_overflow(size, count, &total), 0)) {
		// overflow: trigger the error path in fwrite
		return fwrite(buf, size, count, stream);
	}

	if (__builtin_expect(total > buf_size, 0)) {
		fprintf(stderr, "*** fwrite read overflow detected ***\n");
		abort();
	}

	return fwrite(buf, size, count, stream);
}

char* bionic___strchr_chk(const char* p, int ch, size_t s_len)
{
	for (;; ++p, s_len--) {
		if (__builtin_expect(s_len == 0, 0)) {
			fprintf(stderr, "*** strchr buffer overrun detected ***\n");
			abort();
		}

		if (*p == ch)
			return (char*)p;
		else if (!*p)
			return NULL;
	}
	assert(0 && "should not happen");
}

char* bionic___strrchr_chk(const char* p, int ch, size_t s_len)
{
	const char *save;
	for (save = NULL;; ++p, s_len--) {
		if (__builtin_expect(s_len == 0, 0)) {
			fprintf(stderr, "*** strchr buffer overrun detected ***\n");
			abort();
		}

		if (*p == ch)
			save = p;
		else if (!*p)
			return (char*)save;
	}
	assert(0 && "should not happen");
}

int bionic___FD_ISSET_chk(int fd, fd_set* set) {
	return FD_ISSET(fd, set);
}

void bionic___FD_CLR_chk(int fd, fd_set* set) {
	FD_CLR(fd, set);
}

void bionic___FD_SET_chk(int fd, fd_set* set) {
	FD_SET(fd, set);
}

/*
 * Runtime implementation of __builtin____strncpy_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This strncpy check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
char* bionic___strncpy_chk(char* __restrict dest, const char* __restrict src,
								size_t len, size_t dest_len) {
  return strncpy(dest, src, len);
}
/*
 * __strncpy_chk2
 *
 * This is a variant of __strncpy_chk, but it also checks to make
 * sure we don't read beyond the end of "src". The code for this is
 * based on the original version of strncpy, but modified to check
 * how much we read from "src" at the end of the copy operation.
 */
char* bionic___strncpy_chk2(char* __restrict dst, const char* __restrict src,
			  size_t n, size_t dest_len, size_t src_len)
{
  if (n != 0) {
	char* d = dst;
	const char* s = src;
	do {
	  if ((*d++ = *s++) == 0) {
		/* NUL pad the remaining n-1 bytes */
		while (--n != 0) {
		  *d++ = 0;
		}
		break;
	  }
	} while (--n != 0);
  }
  return dst;
}

/*
 * Runtime implementation of __builtin____vsnprintf_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This vsnprintf check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
int bionic___vsnprintf_chk(char* dest, size_t supplied_size, int /*flags*/,
								size_t dest_len_from_compiler, const char* format, va_list va) {
  return vsnprintf(dest, supplied_size, format, va);
}
/*
 * Runtime implementation of __builtin____snprintf_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This snprintf check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
int bionic___snprintf_chk(char* dest, size_t supplied_size, int flags,
							  size_t dest_len_from_compiler, const char* format, ...) {
  va_list va;
  va_start(va, format);
  int result = bionic___vsnprintf_chk(dest, supplied_size, flags, dest_len_from_compiler, format, va);
  va_end(va);
  return result;
}

/*
 * Runtime implementation of __builtin____vsprintf_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This vsprintf check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
int bionic___vsprintf_chk(char* dest, int /*flags*/,
							  size_t dest_len_from_compiler, const char* format, va_list va) {
  int result = vsnprintf(dest, dest_len_from_compiler, format, va);
  return result;
}
/*
 * Runtime implementation of __builtin____sprintf_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This sprintf check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
int bionic___sprintf_chk(char* dest, int flags,
							 size_t dest_len_from_compiler, const char* format, ...) {
  va_list va;
  va_start(va, format);
  int result = bionic___vsprintf_chk(dest, flags, dest_len_from_compiler, format, va);
  va_end(va);
  return result;
}

/*
 * Runtime implementation of __memcpy_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This memcpy check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
void* bionic___memcpy_chk(void* dest, const void* src,
							  size_t copy_amount, size_t dest_len) {
  return memcpy(dest, src, copy_amount);
}

/*
 * Runtime implementation of __builtin____memmove_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This memmove check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
void *bionic___memmove_chk (void *dest, const void *src,
			  size_t len, size_t dest_len)
{
	return memmove(dest, src, len);
}

ssize_t bionic___read_chk(int fd, void* buf, size_t count, size_t buf_size) {
  return read(fd, buf, count);
}

int __open_2(const char *pathname, int flags) {
	flags |= O_LARGEFILE;
	return open(pathname, flags, 0);
}

/*
 * Runtime implementation of __builtin____memset_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This memset check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
void *bionic___memset_chk (void *dest, int c, size_t n, size_t dest_len) {
	return memset(dest, c, n);
}

/*
 * Runtime implementation of __builtin____strcpy_chk.
 *
 * See
 *	http://gcc.gnu.org/onlinedocs/gcc/Object-Size-Checking.html
 *	http://gcc.gnu.org/ml/gcc-patches/2004-09/msg02055.html
 * for details.
 *
 * This strcpy check is called if _FORTIFY_SOURCE is defined and
 * greater than 0.
 */
char *bionic___strcpy_chk (char *dest, const char *src, size_t dest_len) {
	return strcpy(dest, src);
}
