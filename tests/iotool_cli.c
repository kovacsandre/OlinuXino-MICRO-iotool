#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <linux/can.h>
#include <stdint.h>

#define SOCK_PATH "/var/run/iotool.sock"

uint8_t inputs[]  = {0x01, 0x02, 0x04, 0x08};
uint8_t scout[]   = {0x10, 0x20, 0x40, 0x80};

typedef struct iotool {
    uint8_t command;
    uint8_t input_bits;
    uint8_t output_bits;
} io_t;

enum {
    INPUT_INFO,
    OUTPUT_INFO,
    SHORT_CIRCUIT,
    SET_OUTPUT_BIT,
    SET_ALL_OUTPUT_BIT,
    CLEAR_OUTPUT_BIT,
    CLEAR_ALL_OUTPUT_BIT
};

volatile sig_atomic_t exit_flag = 0;

void
signal_handler(int s)
{
    exit_flag = 1;
}

int
main(void)
{
    int s, t, len;
    struct sockaddr_un remote;
    io_t iotool_data;

    signal(SIGINT, signal_handler);

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }

    printf("Trying to connect...\n");

    remote.sun_family = AF_UNIX;
    strcpy(remote.sun_path, SOCK_PATH);
    len = strlen(remote.sun_path) + sizeof(remote.sun_family);
    if (connect(s, (struct sockaddr *)&remote, len) == -1) {
        perror("connect");
        exit(1);
    }

    printf("Connected.\n");

    int ret_poll;
    struct pollfd input[1]; input[0].fd = s; input[0].events = POLLIN;

    while(!exit_flag) {
        ret_poll = poll(input, 1, -1);
        if (ret_poll < 0) {
            exit_flag = 1;
            continue;
        }

        t=recv(s, &iotool_data, sizeof(struct iotool), 0);

        if (t > 0) {
            switch (iotool_data.command) {
                case INPUT_INFO :
                    for (size_t i = 0; i < 4; i++) {
                        printf("DI%zu -> %d\n", i, (iotool_data.input_bits & inputs[i]) ? 1 : 0);
                    }
                    printf("\n");
                break;
                case OUTPUT_INFO :
                break;
                case SHORT_CIRCUIT :
                    for (size_t i = 0; i < 4; i++) {
                        if (iotool_data.input_bits & inputs[i])
                            printf("DO%zu was in short circuit\n");
                    }
                break;
                default : break;
            }
        } else {
            if (t < 0) perror("recv");
            else printf("Server closed connection\n");
            exit(1);
        }
    }

    close(s);

    return 0;
}
