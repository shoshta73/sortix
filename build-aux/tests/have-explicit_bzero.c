#include <string.h>

int main(void)
{
	void (*ptr)(void*, size_t) = explicit_bzero;
	return ptr ? 0 : 1;
}
#define HAVE_EXPLICIT_BZERO 1
