#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <termios.h>
#include "dialtone.h"

bool send_string(int fd, char* str) {
    return write(fd, str, strlen(str)) == strlen(str);
}

unsigned char get_response(int fd) {
    unsigned char response;
    assert(read(fd, &response, 1) != 0);
    return response;
}

void reset_modem(int fd) {
    send_string(fd, "ATZ0\r\n");
    //assert(get_response(fd) == 0);
    usleep(1000000);
	send_string(fd, "ATE\r\n");
	send_string(fd, "ATV\r\n");
    //assert(get_response(fd) == 0);
}

void send_escape(int fd) {
    send_string(fd, "+++");
    usleep(1500000);
    assert(get_response(fd) == 0);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return -1;
    }

    struct termios tty;

    /* Open the TTY device */
    char* path = argv[1];
    int ttyDev = open(path, O_RDWR | O_NOCTTY);
    if (ttyDev < 0) {
        printf("failed to open %s: error: %s\n", path, strerror(errno));
        return -1;
    }

    /* Get the TTY device's attributes. */
    if (tcgetattr(ttyDev, &tty) != 0) {
        printf("failed to get attributes for %s: error: %s\n", path, strerror(errno));
        return -1;
    }
    
    /* Assuming defaults for reasonably modern modems. */
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 5;

    /* Apply configuration. */
    if (tcsetattr(ttyDev, TCSANOW, &tty) != 0) {
        printf("failed to set TTY configuration for %s\n: error %s", path, strerror(errno));
        return -1;
    }

    /* Test writing. */
    reset_modem(ttyDev);
	tcflush(ttyDev, TCIFLUSH);
    send_string(ttyDev, "AT+FCLASS=8\r\n");
	send_string(ttyDev, "AT+VLS=1\r\n");
	send_string(ttyDev, "AT+VSM=1,8000\r\n");
	send_string(ttyDev, "AT+VTX\r\n");

    /* Test Reading. */
    char buf[1024] = {0};
    int bytes = read(ttyDev, buf, 1024);
    if (bytes > 0) {
		buf[1024] = 0;
		printf("%s", buf);
	}
	printf("\n");

    /* Clean up. */
    close(ttyDev);
}