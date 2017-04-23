/*
 * Copyright (c) 2017 Jonas 'Sortie' Termansen.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * test-unix-socket-fd-passing.c
 * Tests whether passing a file descriptor over an Unix socket works.
 */

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <stdalign.h>
#include <stdio.h>
#include <unistd.h>

#include "test.h"

int main(void)
{
	int fds[2];
	test_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

	FILE* file;
	test_assert((file = tmpfile()));

	struct stat expected_st;
	test_assert(fstat(fileno(file), &expected_st) == 0);

	struct msghdr mhdr = { 0 };
	char buf[1] = { 0 };
	struct iovec iov;
	iov.iov_base = buf;
	iov.iov_len = sizeof(buf);
	mhdr.msg_iov = &iov;
	mhdr.msg_iovlen = 1;
	alignas(struct cmsghdr) char cmsgdata[CMSG_SPACE(sizeof(int))];
	mhdr.msg_control = cmsgdata;
	mhdr.msg_controllen = sizeof(cmsgdata);

	pid_t child_pid;
	test_assert(0 <= (child_pid = fork()));

	if ( child_pid == 0 )
	{
		close(fds[0]);
		buf[0] = 'X';
		struct cmsghdr* cmsg = CMSG_FIRSTHDR(&mhdr);
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		*(int*) CMSG_DATA(cmsg) = fileno(file);
		ssize_t amount = sendmsg(fds[1], &mhdr, 0);
		test_assert(0 <= amount);
		test_assertx(amount == 1);
		_exit(0);
	}

	close(fds[1]);
	fclose(file);

	ssize_t amount = recvmsg(fds[0], &mhdr, 0);
	test_assert(0 <= amount);
	test_assertx(amount == 1);
	test_assertx(buf[0] == 'X');
	test_assertx(!(mhdr.msg_flags & MSG_CTRUNC));
	struct cmsghdr* cmsg = CMSG_FIRSTHDR(&mhdr);
	test_assertx(cmsg);
	test_assertx(cmsg->cmsg_level == SOL_SOCKET);
	test_assertx(cmsg->cmsg_type == SCM_RIGHTS);
	test_assertx(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
	int file_fd = *(int*) CMSG_DATA(cmsg);
	test_assertx(0 <= file_fd);
	struct stat gotten_st;
	test_assert(fstat(file_fd, &gotten_st) == 0);
	test_assertx(gotten_st.st_ino == expected_st.st_ino);
	test_assertx(gotten_st.st_dev == expected_st.st_dev);
	test_assertx(!CMSG_NXTHDR(&mhdr, cmsg));

	int code;
	test_assert(waitpid(child_pid, &code, 0) == child_pid);
	test_assert(WIFEXITED(code) && WEXITSTATUS(code) == 0);

	return 0;
}
