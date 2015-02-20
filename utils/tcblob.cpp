#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

char* atcgetblob(int fd, const char* name, size_t* size_ptr)
{
	ssize_t size = tcgetblob(fd, name, NULL, 0);
	if ( size < 0 )
		return NULL;
	char* result = (char*) malloc((size_t) size + 1);
	if ( !result )
		return NULL;
	ssize_t second_size = tcgetblob(fd, name, result, (size_t) size);
	if ( second_size != size )
		return free(result), (char*) NULL;
	result[(size_t) size] = '\0';
	if ( size_ptr )
		*size_ptr = (size_t) size;
	return result;
}

int main(int argc, char* argv[])
{
	if ( argc < 2 )
		return 1;
	int fd = open(argv[1], O_RDONLY);
	if ( fd < 0 )
		error(1, errno, "%s", argv[1]);
	if ( argc < 3 )
	{
		size_t index_size;
		char* index = atcgetblob(fd, NULL, &index_size);
		if ( !index )
			error(1, errno, "tcgetblob: %s", argv[1]);
		for ( size_t i = 0; i < index_size; )
		{
			printf("%s", index + i);
			size_t value_size;
			char* value = atcgetblob(fd, index + i, &value_size);
			if ( value )
			{
				printf(" = ");
				fwrite(value, 1, value_size, stdout);
				printf("\n");
				free(value);
			}
			i += strlen(index + i) + 1;
		}
		free(index);
	}
	else
	{
		for ( int i = 2; i < argc; i++ )
		{
			size_t value_size;
			char* value = atcgetblob(fd, argv[i], &value_size);
			if ( !value )
				error(1, errno, "tcgetblob: %s: %s", argv[1], argv[i]);
			fwrite(value, 1, value_size, stdout);
			printf("\n");
			free(value);
		}
	}
	close(fd);
	return 0;
}
