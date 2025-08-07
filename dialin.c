#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <termios.h>
#include <sys/time.h>
#include <inttypes.h>
#include <sys/select.h>
#include "dialtone.h"

typedef struct {
    int fd;
    bool sendingDialTone;
    struct timeval lastDialtoneSend;
    int dialTonePos;
    bool clientDialing;
    struct timeval lastDigitRecieved;
    pid_t pppd;
    char path[512];
} Modem;

bool send_string(int fd, char* str) {
    return write(fd, str, strlen(str)) == strlen(str);
}

void reset_modem(Modem *modem) {
    send_string(modem->fd, "ATZ0\r\n");
    usleep(1000000);
	send_string(modem->fd, "ATE\r\n");
	send_string(modem->fd, "ATV\r\n");
    tcflush(modem->fd, TCIFLUSH);
}

int init_modem(Modem *modem, char *path) {
    struct termios tty;

    /* Open the TTY device */
    if (modem == NULL) {
        return -1;
    }
    modem->fd = open(path, O_RDWR | O_NOCTTY);
    if (modem->fd < 0) {
        return -2;
    }

    /* Get the TTY device's attributes. */
    if (tcgetattr(modem->fd, &tty) != 0) {
        return -3;
    }
    
    /* Assuming defaults for reasonably modern modems. */
    cfsetispeed(&tty, B115200);
    cfsetospeed(&tty, B115200);
    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8 | CREAD | CLOCAL;
    tty.c_lflag = 0;
    tty.c_oflag = 0;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 10;

    /* Apply configuration. */
    if (tcsetattr(modem->fd, TCSANOW, &tty) != 0) {
        return -4;
    }
    reset_modem(modem);
    strncpy(modem->path, path, sizeof(modem->path));
    modem->path[sizeof(modem->path) - 1] = 0;
    return 0;
}

/* Get a response code from the modem. */
int get_response(Modem *modem) {
    char buf[1024] = {0};
    int res;
    int bytes = read(modem->fd, buf, 1024);

    if (bytes <= 0) {
        return -1;
    }

    if (bytes < 1024) {
        buf[bytes] = 0;
    } else {
        buf[1023] = 0;
    }

    if (sscanf(buf, "%d", &res) != 1) {
        return -1;
    }
    return res;
}

void send_escape(Modem *modem) {
    send_string(modem->fd, "+++");
    usleep(1100000);
    assert(get_response(modem) == 0);
}

void send_dialtone(Modem *modem) {
    if (modem->sendingDialTone) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t usecSinceLastSend = (now.tv_sec - modem->lastDialtoneSend.tv_sec)*1000000 + (now.tv_usec - modem->lastDialtoneSend.tv_usec);
        int bytesToSend = (usecSinceLastSend*8001)/1000000;
        while (bytesToSend > 0) {
            int size = bytesToSend;
            unsigned char* buf = dialtone + modem->dialTonePos;
            assert(modem->dialTonePos < sizeof(dialtone));
            if (modem->dialTonePos + bytesToSend >= sizeof(dialtone)) {
                size = sizeof(dialtone) - modem->dialTonePos;
                modem->dialTonePos = 0;
            } else {
                modem->dialTonePos += size;
            }
            assert(write(modem->fd, buf, size) == size);
            bytesToSend -= size;
        }
        modem->lastDialtoneSend = now;
    }
}

bool start_dialtone(Modem *modem) {
    int res;
    /*send_string(modem->fd, "ATH\r\n");
    if ((res = get_response(modem)) != 0) {
        printf("%d\n", res);
        return false;
    }*/
	send_string(modem->fd, "AT+FCLASS=8\r\n");
    if ((res = get_response(modem)) != 0) {
        printf("%d\n", res);
        return false;
    }
	send_string(modem->fd, "AT+VLS=1\r\n");
    if ((res = get_response(modem)) != 0) {
        printf("%d\n", res);
        return false;
    }
	send_string(modem->fd, "AT+VSM=1,8000\r\n");
    if ((res = get_response(modem)) != 0) {
        printf("%d\n", res);
        return false;
    }
	send_string(modem->fd, "AT+VTX\r\n");
    if (get_response(modem) != 1) {
        return false;
    }
    modem->sendingDialTone = true;
    modem->clientDialing = false;
    modem->dialTonePos = 0;
    gettimeofday(&modem->lastDialtoneSend, NULL);
    modem->lastDialtoneSend.tv_sec -= 1;
    send_dialtone(modem);
    return true;
}

void stop_dialtone(Modem *modem) {
    char buf[] = {0x10, 0x03};
    write(modem->fd, buf, sizeof(buf));
    send_escape(modem);
    send_string(modem->fd, "AT+FCLASS=0\r\n");
    assert(get_response(modem) == 0);
    modem->sendingDialTone = false;
}

bool answer_call(Modem *modem) {
    struct timeval timeout = {60, 0};
    fd_set modemSet;
	/* Don't know if this is needed and takes forever to execute on my G4 modem. */
	/* send_string(modem->fd, "ATH1\r\n");
    assert(get_response(modem) == 0); */
    send_string(modem->fd, "ATM1\r\n");
    assert(get_response(modem) == 0);
    send_string(modem->fd, "ATA\r\n");
    /* Give a second for the modem to respond (for modems that send OK after ATA). */
    usleep(1000000);
    tcflush(modem->fd, TCIFLUSH);
    /* Wait 60s for the modem to respond. */
    FD_ZERO(&modemSet);
    FD_SET(modem->fd, &modemSet);
    if (select(modem->fd + 1, &modemSet, NULL, NULL, &timeout) == 1 && FD_ISSET(modem->fd, &modemSet)) {
        int res;
        /* Don't know rn what reponse codes modems will return. */
        if ((res = get_response(modem)) == 4) {
            printf("%d\n", res);
            return false;
        }
        /* Start PPPD. */
		usleep(100000);
        pid_t id = fork();
        if (id == 0) {
            assert(execl("/usr/sbin/pppd", "-detach", modem->path, "115200", "file", "options.modem", NULL) != -1);
        } else {
            modem->pppd = id;
        }
        return true;
    }
    /* If the modem doesn't respond after 60s, something's wrong. */
    return false;
}

void modem_loop(Modem *modem) {
	while (true) {
		if (!start_dialtone(modem)) {
			break;
		}
		puts("Listening for dial...\n");
		while (true) {
            fd_set modemSet;
            struct timeval timeout = {0, 50000};
            send_dialtone(modem);
            FD_ZERO(&modemSet);
            FD_SET(modem->fd, &modemSet);
            /* Wait 50ms to see if the modem has any data for us. */
            if (select(modem->fd + 1, &modemSet, NULL, NULL, &timeout) == 1 && FD_ISSET(modem->fd, &modemSet)) {
                /* See if we recieved a DTMF num. */
                char buf[2];
                int bytes = read(modem->fd, buf, 2);
                /* The client is dialing a number. */
                if (bytes > 1 && buf[0] == 0x10 && isdigit(buf[1])) {
                    modem->clientDialing = true;
                    gettimeofday(&modem->lastDigitRecieved, NULL);
                }
            } else if (modem->clientDialing) {
                struct timeval now;
                gettimeofday(&now, NULL);
                if ((now.tv_sec - modem->lastDigitRecieved.tv_sec)*1000 + (now.tv_usec - modem->lastDigitRecieved.tv_usec)/1000 > 500) {
                    /* The client is done dialing*/
                    modem->clientDialing = false;
                    break;
                }
            }
        }
        stop_dialtone(modem);
		puts("Client dialed! Picking up...\n");
        if (answer_call(modem)) {
			int res;
			puts("Client connected!\n");
			waitpid(modem->pppd, &res, 0);
		} else {
			puts("Client failed to connect. :(\n");
		}
	}
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return -1;
    }
    /* Init the modem */
    Modem modem = {0};
    int res;
    if ((res = init_modem(&modem, argv[1])) != 0) {
        printf("Initializing modem %s failed! Return val: %i; Error: %s\n", argv[1], res, strerror(errno));
        return res;
    }

    /* Test answering a call. */
	puts("Answering client.\n");
    answer_call(&modem);
	waitpid(modem.pppd, &res, 0);

    /* Clean up. */
    close(modem.fd);
}