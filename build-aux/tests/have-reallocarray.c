#include <stdlib.h>

int main(void)
{
	void* (*ptr)(void*, size_t, size_t) = reallocarray;
	return ptr ? 0 : 1;
}
#define HAVE_REALLOCARRAY 1
