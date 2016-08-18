#include <string.h>

int main(void)
{
	size_t (*ptr)(char*, const char*, size_t) = strlcat;
	return ptr ? 0 : 1;
}
#define HAVE_STRLCAT 1
