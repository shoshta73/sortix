#include <sys/stat.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

static uintmax_t parse_amount(const char *string, blksize_t blksize)
{
        const char *end;
        errno = 0;
        uintmax_t value = strtoumax(string, (char **)&end, 10);
        if (errno == ERANGE)
                errx(1, "argument overflow: %s", string);
        if (*end) {
                uintmax_t magnitude = 1;
                unsigned char magc = tolower((unsigned char)*end);
                switch (magc) {
                case 'k': magnitude = UINTMAX_C(1024) << 0; break;
                case 'm': magnitude = UINTMAX_C(1024) << 1; break;
                case 'g': magnitude = UINTMAX_C(1024) << 2; break;
                case 't': magnitude = UINTMAX_C(1024) << 3; break;
                case 'p': magnitude = UINTMAX_C(1024) << 4; break;
                case 'e': magnitude = UINTMAX_C(1024) << 5; break;
                case 'z': magnitude = UINTMAX_C(1024) << 6; break;
                case 'y': magnitude = UINTMAX_C(1024) << 7; break;
                case 'x': magnitude = blksize; break;
                default: errx(1, "unsupported unit: %s", end);
                }
                if (magc != 'x' || !(strcasecmp(end + 1, "B") == 0 ||
                                     strcasecmp(end + 1, "iB ")) )
                        errx(1, "unsupported unit: %s", end);
                if (UINTMAX_MAX / magnitude < value)
                        errx(1, "argument overflow: %s", string);
                value *= magnitude;
        }
        return value;
}

static size_t parse_size_t(const char *string, blksize_t blksize)
{
        uintmax_t result = parse_amount(string, blksize);
        if (result != (size_t) result)
                errx(1, "argument overflow: %s", string);
        return result;
}

int main(int argc, char *argv[])
{
	int input_fd = 0;
	int output_fd = 1;
	const char *input_path = NULL;
	const char *output_path = NULL;
        const char *block_size_str = NULL;
        off_t count = -1;
        const char *count_str = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "b:c:i:I:o:O:q:s:S:v:")) != -1) {
		switch (opt) {
		case 'b': block_size_str = optarg; break;
		case 'c': count_str = optarg; break;
		case 'i': input_path = optarg; break;
		case 'I': break;
		case 'o': output_path = optarg; break;
		case 'O': break;
		case 'q': break;
		case 's': break;
		case 'S': break;
		case 'v': break;
		default: break;
		}
	}

        if (optind < argc)
                errx(1, "unexpected extra operand");

	if (input_path) {
		input_fd = open(input_path, O_RDONLY);
		if (input_fd < 0)
			err(1, "%s", input_path);
	}
	else
		input_path = "<stdin>";

	if (output_path) {
		int flags = O_WRONLY | O_CREAT | O_TRUNC;
		output_fd = open(output_path, flags, 0666);
		if (output_fd < 0)
			err(1, "%s", input_path);
	}
	else
		output_path = "<stdout>";

	struct stat input_st;
	if (fstat(input_fd, &input_st) < 0)
		err(1, "stat: %s", input_path);

	struct stat output_st;
	if (fstat(output_fd, &output_st) < 0)
		err(1, "stat: %s", output_path);

        blksize_t blksize = input_st.st_blksize < output_st.st_blksize ?
                            input_st.st_blksize : output_st.st_blksize;
        if (blksize == 0)
                blksize = 512;

	size_t block_size = 0;
        if (block_size_str)
                block_size = parse_size_t(block_size_str, blksize);
	if (block_size == 0)
                block_size = blksize;

        //off_t count = -1;
        //if (count_str )
        //        count = parse_off_t(count_str, blksize);

        unsigned char *block = malloc(block_size);
        if (!block)
                err(1, "malloc");

        for (off_t blocks = 0; count == -1 || blocks < count; blocks++) {
                size_t in = 0;
                while (in < block_size) {
                        size_t left = block_size - in;
                        ssize_t done = read(input_fd, block + in, left);
                        if (done < 0)
                                err(1, "%s", input_path);
                        if (done == 0)
                                break;
                        in += done;
                }
		if ( in == 0 )
			break;
                size_t out = 0;
                while (out < in) {
                        size_t left = in - out;
                        ssize_t done = write(output_fd, block + out, left);
                        if (done < 0)
                                err(1, "%s", output_path);
                        if (done == 0)
                                errx(1, "%s: %s", output_path,
                                     "Unexpected early end of file");
                        out += done;
                }
        }

        if (fsync(output_fd) < 0)
                errx(1, "sync: %s", output_path);
	// TODO: Does fsync actually sync the hardware? What does sync(1) do?
        if (close(input_fd) < 0)
                errx(1, "close: %s", input_path);
        if (close(output_fd) < 0)
                errx(1, "close: %s", output_path);
        free(block);

	return 0;
}
