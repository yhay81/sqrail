#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef RAIL_TARGET
#error "RAIL_TARGET must name the concealed executable"
#endif

#ifndef RAIL_HELP
#error "RAIL_HELP must name the sanitized help file"
#endif

#ifndef RAIL_LOG
#error "RAIL_LOG must name the private invocation log"
#endif

#ifndef RAIL_MODE
#error "RAIL_MODE must be 0 for sqrail or 1 for the DuckDB CLI"
#endif

static void write_all(int fd, const char *buffer, size_t size) {
	while (size > 0) {
		ssize_t written = write(fd, buffer, size);
		if (written < 0) {
			if (errno == EINTR) {
				continue;
			}
			return;
		}
		buffer += (size_t)written;
		size -= (size_t)written;
	}
}

static void write_help(void) {
	char buffer[4096];
	int fd = open(RAIL_HELP, O_RDONLY);
	if (fd < 0) {
		static const char message[] = "rail: help is unavailable\n";
		write_all(STDERR_FILENO, message, sizeof(message) - 1);
		_exit(70);
	}
	for (;;) {
		ssize_t count = read(fd, buffer, sizeof(buffer));
		if (count == 0) {
			break;
		}
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			close(fd);
			_exit(70);
		}
		write_all(STDOUT_FILENO, buffer, (size_t)count);
	}
	close(fd);
}

static char *append_hex(char *output, const char *value) {
	static const char digits[] = "0123456789abcdef";
	for (const unsigned char *cursor = (const unsigned char *)value; *cursor != '\0'; ++cursor) {
		*output++ = digits[*cursor >> 4];
		*output++ = digits[*cursor & 0x0f];
	}
	return output;
}

static int lock_log(int fd) {
	struct flock lock;
	memset(&lock, 0, sizeof(lock));
	lock.l_type = F_WRLCK;
	lock.l_whence = SEEK_SET;
	while (fcntl(fd, F_SETLKW, &lock) < 0) {
		if (errno != EINTR) {
			return -1;
		}
	}
	return 0;
}

static void log_invocation(int argc, char **argv) {
	char timestamp[64];
	int size = snprintf(timestamp, sizeof(timestamp), "%lld", (long long)time(NULL));
	if (size <= 0) {
		return;
	}

	size_t record_size = (size_t)size + 1;
	for (int index = 1; index < argc; ++index) {
		size_t argument_size = strlen(argv[index]);
		if (argument_size > (SIZE_MAX - record_size - 1) / 2) {
			return;
		}
		record_size += 1 + argument_size * 2;
	}
	char *record = malloc(record_size);
	if (record == NULL) {
		return;
	}
	char *cursor = record;
	memcpy(cursor, timestamp, (size_t)size);
	cursor += size;
	for (int index = 1; index < argc; ++index) {
		*cursor++ = '\t';
		cursor = append_hex(cursor, argv[index]);
	}
	*cursor++ = '\n';

	int fd = open(RAIL_LOG, O_WRONLY | O_APPEND | O_CREAT, 0600);
	if (fd < 0) {
		free(record);
		return;
	}
	if (lock_log(fd) == 0) {
		write_all(fd, record, record_size);
	}
	close(fd);
	free(record);
}

static int is_option(int argc, char **argv, const char *short_name, const char *long_name) {
	return argc == 2 && (strcmp(argv[1], short_name) == 0 || strcmp(argv[1], long_name) == 0);
}

int main(int argc, char **argv) {
	log_invocation(argc, argv);

	if (is_option(argc, argv, "-h", "--help") || is_option(argc, argv, "-help", "-help")) {
		write_help();
		return 0;
	}
	if (is_option(argc, argv, "-V", "--version") || is_option(argc, argv, "-version", "--version")) {
		static const char version[] = "rail 1.0\n";
		write_all(STDOUT_FILENO, version, sizeof(version) - 1);
		return 0;
	}

#if RAIL_MODE == 0
	argv[0] = (char *)"rail";
	execv(RAIL_TARGET, argv);
#elif RAIL_MODE == 1
	char **forwarded = calloc((size_t)argc + 3, sizeof(char *));
	if (forwarded == NULL) {
		return 70;
	}
	forwarded[0] = (char *)"rail";
	forwarded[1] = (char *)"-no-init";
	forwarded[2] = (char *)"-batch";
	for (int index = 1; index < argc; ++index) {
		forwarded[index + 2] = argv[index];
	}
	execv(RAIL_TARGET, forwarded);
#else
#error "RAIL_MODE must be 0 or 1"
#endif

	static const char prefix[] = "rail: cannot start concealed tool: ";
	write_all(STDERR_FILENO, prefix, sizeof(prefix) - 1);
	const char *message = strerror(errno);
	write_all(STDERR_FILENO, message, strlen(message));
	write_all(STDERR_FILENO, "\n", 1);
	return 70;
}
