#include <time.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <sys/select.h>
#include "dialtone.h"

typedef enum {
    IDLE = 0,
    SENDING_DIALTONE,
    CLIENT_DIALING,
    CONNECTING,
    CONNECTED
} modem_state_t;

typedef struct {
    int fd;
    struct timeval lastDialtoneSend;
    int dialTonePos;
    modem_state_t state;
    pid_t pppd;
    int rate;
    char path[512];
} modem_t;

#define MAX_MODEMS 1
static char pppdPath[256] = "/usr/sbin/pppd";
static modem_t *modems[MAX_MODEMS];
static int numModems = 0;

bool send_string(int fd, char* str) {
    return write(fd, str, strlen(str)) == strlen(str);
}

void reset_modem(modem_t *modem) {
    send_string(modem->fd, "ATZ0\r\n");
    usleep(1000000);
	send_string(modem->fd, "ATE\r\n");
	send_string(modem->fd, "ATV\r\n");
    tcflush(modem->fd, TCIFLUSH);
    modem->state = IDLE;
}

int init_modem(modem_t *modem, char *path, unsigned int rate) {
    struct termios tty;
    speed_t speed;

    if (numModems < MAX_MODEMS) {
        switch (rate) {
            case 50:
                speed = B50;
                break;
            case 75:
                speed = B75;
                break;
            case 110:
                speed = B110;
                break;
            case 134:
                speed = B134;
                break;
            case 150:
                speed = B150;
                break;
            case 200:
                speed = B200;
                break;
            case 300:
                speed = B300;
                break;
            case 600:
                speed = B600;
                break;
            case 1200:
                speed = B1200;
                break;
            case 1800:
                speed = B1800;
                break;
            case 2400:
                speed = B2400;
                break;
            case 4800:
                speed = B4800;
                break;
            case 9600:
                speed = B9600;
                break;
            case 19200:
                speed = B19200;
                break;
            case 38400:
                speed = B38400;
                break;
            case 57600:
                speed = B57600;
                break;
            case 115200:
                speed = B115200;
                break;
            case 230400:
                speed = B230400;
                break;
            default:
                return -1;
        }

        /* Open the TTY device */
        if (modem == NULL) {
            return -2;
        }
        modem->fd = open(path, O_RDWR | O_NOCTTY);
        if (modem->fd < 0) {
            return -3;
        }

        /* Get the TTY device's attributes. */
        if (tcgetattr(modem->fd, &tty) != 0) {
            return -4;
        }

        /* Assuming defaults for reasonably modern modems. */
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
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
        modem->rate = rate;
        modems[numModems] = modem;
        numModems++;
        return 0;
    }
    return -5;
}

/* Get a response code from the modem. */
int get_response(modem_t *modem, unsigned int attempts) {
    char buf[1024] = {0};
    int res;
    int bytes;
    /* 0 attempts to read doesn't make sense. */
    if (attempts == 0) {
        attempts = 1;
    }

    for (int i = 0; (i < attempts) && ((bytes = read(modem->fd, buf, 1024)) <= 0); i++) {};

    if (bytes <= 0) {
        return -1;
    }

    if (bytes < 1024) {
        buf[bytes] = 0;
    } else {
        buf[1023] = 0;
    }

    if (sscanf(buf, "%u", &res) != 1) {
        return -1;
    }
    return res;
}

void send_escape(modem_t *modem) {
    send_string(modem->fd, "+++");
    usleep(1100000);
    assert(get_response(modem, 1) == 0);
}

void send_dialtone(modem_t *modem) {
    /* Probably an unnecessary check. */
    if (modem->state = SENDING_DIALTONE) {
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

bool start_dialtone(modem_t *modem) {
    if (modem->state == IDLE) {
        int res;
        send_string(modem->fd, "ATH\r\n");
        if ((res = get_response(modem, 5)) != 0) {
            printf("%d\n", res);
            return false;
        }
        send_string(modem->fd, "AT+FCLASS=8\r\n");
        if ((res = get_response(modem, 1)) != 0) {
            printf("%d\n", res);
            return false;
        }
        send_string(modem->fd, "AT+VLS=1\r\n");
        if ((res = get_response(modem, 1)) != 0) {
            printf("%d\n", res);
            return false;
        }
        send_string(modem->fd, "AT+VSM=1,8000\r\n");
        if ((res = get_response(modem, 1)) != 0) {
            printf("%d\n", res);
            return false;
        }
        send_string(modem->fd, "AT+VTX\r\n");
        if (get_response(modem, 1) != 1) {
            return false;
        }
        modem->state = SENDING_DIALTONE;
        modem->dialTonePos = 0;
        gettimeofday(&modem->lastDialtoneSend, NULL);
        modem->lastDialtoneSend.tv_sec -= 1;
        send_dialtone(modem);
        return true;
    }
    return false;
}

void stop_dialtone(modem_t *modem) {
    if (modem->state == SENDING_DIALTONE) {
        char buf[] = {0x10, 0x03};
        modem->state = IDLE;
        write(modem->fd, buf, sizeof(buf));
        send_escape(modem);
        send_string(modem->fd, "AT+FCLASS=0\r\n");
        assert(get_response(modem, 1) == 0);
    }
}

bool answer_call(modem_t *modem) {
    if (modem->state == IDLE) {
        struct timeval timeout = {60, 0};
        int res;
        /* Don't know if this is needed and takes forever to execute on my G4 modem. */
        send_string(modem->fd, "ATH1\r\n");
        assert(get_response(modem, 10) == 0);
        tcflush(modem->fd, TCIFLUSH);
        send_string(modem->fd, "ATM1\r\n");
        assert(get_response(modem, 1) == 0);
        send_string(modem->fd, "ATA\r\n");
        res = get_response(modem, 1);
        /* If we get some kind of error answering the call, return. */
        if (res != 0 && res != -1) {
            return false;
        }
        /* Handle modems with respond with OK after ATA */
        /* Wait 60s for the modem to respond. */
        res = get_response(modem, 60);
        /* Don't know rn what reponse codes modems will return. */
        if (res == 3 || res == 4 || res == -1) {
            puts("Modem failed to connect!");
            modem->state = IDLE;
            return false;
        }
        printf("Modem returned code %d.\n", res);
        /* Start PPPD. */
        usleep(100000);
        pid_t id = fork();
        if (id == 0) {
            char buf[16];
            sprintf(buf, "%d", modem->rate);
            assert(execl(pppdPath, "-detach", modem->path, buf, "file", "options.modem", NULL) != -1);
        } else {
            modem->pppd = id;
            modem->state = CONNECTED;
        }
        return true;
    }
    return false;
}

void modem_loop(modem_t *modem) {
	while (true) {
		if (!start_dialtone(modem)) {
			break;
		}
		puts("Listening for dial...");
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
                    break;
                }
            }
        }
        stop_dialtone(modem);
        puts("Client dialed! Picking up...");
        /* Wait 5 secs for the client to finish dialing. */
        usleep(5000000);
        if (answer_call(modem)) {
			int res;
			puts("Client connected!");
			waitpid(modem->pppd, &res, 0);
            printf("PPPd exited. Code: %d\n", res);
            reset_modem(modem);
		} else {
			puts("Client failed to connect. :(");
		}
	}
}

void sig_handler(int sig) {
    /* Stop PPPd */
    int res;
    for (int i = 0; i < numModems; i++) {
        if (modems[i]->state == CONNECTED) {
            kill(modems[i]->pppd, SIGINT);
        }
    }
    waitpid(-1, &res, 0);
    if (sig == SIGINT) {
        for (int i = 0; i < numModems; i++) {
            close(modems[i]->fd);
        }
        exit(0);
    }
}

int main(int argc, char **argv) {
    char* tty;
    int rate = 115200;
    bool nodial = false;
    int opt;
    if (argc < 2) {
        puts("You must specify a modem TTY to use! Run with -h for help.");
        return -1;
    }
    while ((opt = getopt(argc, argv, "b:p:nh")) != -1) {
        switch (opt) {
            case 'b':
                if (sscanf(optarg, "%u", &rate) != 1) {
                    fputs("Invalid baud rate specified.\n", stderr);
                }
                break;
            case 'p':
                strncpy(pppdPath, optarg, 256);
                pppdPath[255] = 0;
                break;
            case 'n':
                nodial = true;
                break;
            case 'h':
                printf(
                    "DialIn v0.1a\n\n"
                    "Usage:\n"
                    "%s <modem TTY> [optional args...]\n\n"
                    "Optional args:\n"
                    "-b <baud rate> : The TTY speed to use (in bits/s). [Default: 115200 bits/s]\n"
                    "-p <path to pppd> : The path to the pppd executable to use. [Default: \"/usr/sbin/pppd\"]\n"
                    "-n : No dial. Don't wait for the client to dial: immediately tell the modem to answer.\n"
                    "-h : Display this help.\n\n"
                    "Copyright (C) 2025 Logan C. GPLv3.\n"
                    "Have fun!\n"
                    "-Loganius. :)\n\n",
                    argv[0]);
                return 0;
            case '?':
                if (optopt == 'b') {
                    fputs("Usage: -b <baud rate> (in bits/s not baud)", stderr);
                } else if (optopt == 'p') {
                    fputs("Usage: -p <path to pppd>", stderr);
                } else if (isprint(optopt)) {
                    fprintf(stderr, "Option -%c unknown", optopt);
                } else {
                    fputs("idk what happened. check your command line.", stderr);
                }
                return 1;
        }
    }
    /* Initialize signal handlers. */
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    /* Init the modem */
    modem_t modem = {0};
    int res;
    if ((res = init_modem(&modem, argv[1], rate)) != 0) {
        printf("Initializing modem %s failed! Return val: %i; Error: %s\n", argv[1], res, strerror(errno));
        return res;
    }

    /* Start the modem loop */
    if (nodial) {
        if (answer_call(&modem)) {
            waitpid(modem.pppd, &res, 0);
        } else {
            puts("Client failed to connect. :(");
        }
    } else {
        modem_loop(&modem);
        puts("Something went wrong. The modem loop ended.");
    }

    /* Clean up. */
    close(modem.fd);
}