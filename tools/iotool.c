/**
gcc -Wall -I../c-periphery/src iotool.c ../c-periphery/periphery.a -o iotool
**/

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <syslog.h>
#include <sys/select.h>

#include "i2c.h"
#include "gpio.h"

#define MAX_CLIENTS 5
#define SOCK_PATH "/var/run/iotool.sock"
#define PH17 241
#define I2C_ADDR 0x20

uint8_t inputs[]  = {0x01, 0x02, 0x04, 0x08};
volatile sig_atomic_t exit_flag = 0;

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

/* MCP12X017 registers when IOCON.BANK = 0 */

#define MCP23017_IODIRA    0x00
#define MCP23017_IODIRB    0x01
#define MCP23017_IPOLA     0x02
#define MCP23017_IPOLB     0x03
#define MCP23017_GPINTENA  0x04
#define MCP23017_GPINTENB  0x05
#define MCP23017_DEFVALA   0x06
#define MCP23017_DEFVALB   0x07
#define MCP23017_INTCONA   0x08
#define MCP23017_INTCONB   0x09
#define MCP23017_IOCONA    0x0A
#define MCP23017_IOCONB    0x0B
#define MCP23017_GPPUA     0x0C
#define MCP23017_GPPUB     0x0D
#define MCP23017_INTFA     0x0E
#define MCP23017_INTFB     0x0F
#define MCP23017_INTCAPA   0x10
#define MCP23017_INTCAPB   0x11
#define MCP23017_GPIOA     0x12
#define MCP23017_GPIOB     0x13
#define MCP23017_OLATA     0x14
#define MCP23017_OLATB     0x15

void
usage(const char *pname)
{
    fprintf(stderr, "Usage: %s <options> \n", pname);
    fprintf(stderr, "   Options:    -o <0-3|x,y>    Digital output number or comma separated list.\n");
    fprintf(stderr, "               -l <0|1>        Output level.\n");
    fprintf(stderr, "               -p <ms>         Polling inputs. Negative means infinite.\n");
    fprintf(stderr, "               -s              Read the inputs and exit.\n");
    fprintf(stderr, "               -i <ms>         Pulse mode. Time is the period time. Use with -o and -c\n");
    fprintf(stderr, "               -c <num>        Number of periods in pulse mode.\n");
    fprintf(stderr, "               -d              Daemon mode.\n");

    exit(0);
}

void
exit_program(int sig)
{
    exit_flag = 1;
}

int
main(int argc, char *argv[])
{
    i2c_t i2c;
    uint8_t pbst = 0, past = 0, outc = 0;
    int opt, level = 0, timeout = 0, q = 0, interrupt_fd;
    int p = 0, seto = 0;
    gpio_t interrupt;
    bool dummy;
    io_t iotool_data, iotool_data_req;
    /* Variables for unix sockets */
    int client_socket[MAX_CLIENTS], new_socket, master_socket,
        sd, max_sd, len;
    socklen_t t;
    struct sockaddr_un local, remote;
    fd_set rdfs, exfs;
    /* Pulse mode */
    int pulse = 0;
    unsigned long periodcnt;
    unsigned int halfperiod;

    /* Install signal for ^C */
    struct sigaction sa_exit;

    nice(-20);

    while ((opt = getopt(argc, argv, "o:l:p:si:c:d?")) != -1) {
        switch (opt) {
            case 'o' :
                if (strlen(optarg) > 1) {
                    int i = 0;
                    char *token;
                    const char s[] = ",";
                    token = strtok(optarg, s);

                    while (token != NULL) {
                        i = atoi(token);
                        if (i < 0 || i > 3) {
                            fprintf(stderr, "Output must be 0-3\n");
                            usage(argv[0]);
                        }
                        outc = (outc | (1 << (uint8_t) i));
                        token = strtok(NULL, s);
                    }
                }
                else {
                    int i = atoi(optarg);
                    if (i < 0 || i > 3) {
                        fprintf(stderr, "Output must be 0-3\n");
                        usage(argv[0]);
                    }
                    outc = (1 << (uint8_t) i);
                }
                seto = 1;
            break;

            case 'l' :
                level = atoi(optarg);
                if (level < 0 || level > 1) {
                    fprintf(stderr, "Level must be 0 or 1\n");
                    usage(argv[0]);
                }
            break;

            case 'p' :
                p = 1;
                timeout = atoi(optarg);
            break;

            case 's' :
                p = 1;
                timeout = 0;
            break;

            case 'i' :
                pulse = 1;
                if (atoi(optarg) < 1) {
                    fprintf(stderr, "Number must be a positive, non zero integer.\n");
                    usage(argv[0]);
                }
                halfperiod = (atoi(optarg) * 1000) / 2;
            break;

            case 'c' :
                periodcnt = atol(optarg);
            break;

            case 'd' :
                q = 1;
            break;

            default :
                usage(argv[0]);
            break;
        }
    }

    sa_exit.sa_handler = exit_program;
    sa_exit.sa_flags = 0;
    sigemptyset(&sa_exit.sa_mask);

    if (sigaction(SIGINT, &sa_exit, NULL) < 0) {
        perror("sigaction() error");
        exit(EXIT_FAILURE);
    }

    /* Open the i2c-1 bus */
    if (i2c_open(&i2c, "/dev/i2c-1") < 0) {
        fprintf(stderr, "i2c_open(): %s\n", i2c_errmsg(&i2c));
        exit(1);
    }

    /* Enable interrupts on Port A */
    uint8_t inta[] = { MCP23017_GPINTENA, 0xFF };
    /* Port B 0-3 to output */
    uint8_t portb_dir[] = { MCP23017_IODIRB, 0xF0 };
    uint8_t porta_dir[] = { MCP23017_IODIRA, 0xFF };
    /* Enable pull-ups Port A 4-7 */
    uint8_t pulla[] = { MCP23017_GPPUA, 0xF0 };
    uint8_t outp[] = { MCP23017_GPIOB, 0x00 };

    struct i2c_msg conf[] =
        {
            { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = portb_dir },
            { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = porta_dir },
            { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = pulla },
            { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = inta }
        };

    uint8_t PADR = MCP23017_GPIOA;
    struct i2c_msg padata[] =
        {
            { .addr = I2C_ADDR, .flags = 0, .len = 1, .buf = &PADR },
            { .addr = I2C_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &past }
        };

    uint8_t PBDR = MCP23017_GPIOB;
    struct i2c_msg pbdata[] =
        {
            { .addr = I2C_ADDR, .flags = 0, .len = 1, .buf = &PBDR },
            { .addr = I2C_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &pbst }
        };

    struct i2c_msg output[] =
        {
            { .addr = I2C_ADDR, .flags = 0, .len = 2, .buf = outp }
        };

    /* Transfer I2C messages */
    for (size_t i = 0; i < 4; i++) {
        if (i2c_transfer(&i2c, &conf[i], 1) < 0) {
            fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
            exit(1);
        }
    }

    if (q) {
        openlog("iotool-server", LOG_CONS | LOG_PID, LOG_USER);
        syslog(LOG_INFO, "Entering iotool-server daemon...");

        if (daemon(0, 0) < 0) {
            syslog(LOG_CRIT, "daemon(): %s", strerror(errno));
            return -2;
        }

        for (size_t i = 0; i < MAX_CLIENTS; i++)
            client_socket[i] = 0;

        if ((master_socket = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
            syslog(LOG_CRIT, "socket(): %s", strerror(errno));
            exit(1);
        }

        local.sun_family = AF_UNIX;
        strcpy(local.sun_path, SOCK_PATH);
        unlink(local.sun_path);
        len = strlen(local.sun_path) + sizeof(local.sun_family);
        if (bind(master_socket, (struct sockaddr *)&local, len) == -1) {
            syslog(LOG_CRIT, "bind(): %s", strerror(errno));
            exit(1);
        }

        if (listen(master_socket, 5) == -1) {
            syslog(LOG_CRIT, "listen(): %s", strerror(errno));
            exit(1);
        }

        t = sizeof(remote);

        if (gpio_open(&interrupt, PH17, GPIO_DIR_IN) < 0) {
            syslog(LOG_CRIT, "gpio_open(): %s\n", gpio_errmsg(&interrupt));
            exit(1);
        }

        if (gpio_set_edge(&interrupt, GPIO_EDGE_FALLING) < 0) {
            syslog(LOG_CRIT, "gpio_set_edge(): %s\n", gpio_errmsg(&interrupt));
            exit(1);
        }

        if ((interrupt_fd = gpio_fd(&interrupt)) < 0) {
            syslog(LOG_CRIT, "gpio_fd(): %s\n", gpio_errmsg(&interrupt));
            exit(1);
        }

        if (gpio_read(&interrupt, &dummy) < 0) {
            syslog(LOG_CRIT, "gpio_read(): %s\n", gpio_errmsg(&interrupt));
            exit(EXIT_FAILURE);
        }
        /* Dummy read */
        for (size_t i = 0; i < 2; i++) {
            if (i2c_transfer(&i2c, &padata[i], 1) < 0) {
                syslog(LOG_ERR, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                exit(EXIT_FAILURE);
            }
        }

        syslog(LOG_INFO, "Init success!");

        while (!exit_flag) {
            FD_ZERO(&rdfs);
            FD_ZERO(&exfs);
            FD_SET(master_socket, &rdfs);
            FD_SET(interrupt_fd, &exfs);
            max_sd = interrupt_fd;

            /* add child sockets to set */
            for (size_t i = 0; i < MAX_CLIENTS; i++) {
                sd = client_socket[i];

                /* if valid socket descriptor then add to read list */
                if(sd > 0)
                    FD_SET(sd , &rdfs);

                /* highest file descriptor number, need it for the select function */
                if(sd > max_sd)
                    max_sd = sd;
            }

            if (select(max_sd + 1, &rdfs, NULL, &exfs, NULL) < 0) {
                syslog(LOG_CRIT, "select(): %s", strerror(errno));
                exit_flag = 1;
                continue;
            }
            /* INTA */
            if (FD_ISSET(interrupt_fd, &exfs)) {
                if (gpio_read(&interrupt, &dummy) < 0) {
                    syslog(LOG_CRIT, "gpio_read(): %s\n", gpio_errmsg(&interrupt));
                    exit(EXIT_FAILURE);
                }
                /* Getting inputs */
                /* Possible inrush current */
                usleep(1000);
                for (size_t i = 0; i < 2; i++) {
                    if (i2c_transfer(&i2c, &padata[i], 1) < 0) {
                        syslog(LOG_ERR, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                        exit(EXIT_FAILURE);
                    }
                }
                /* Check short circuit */
                /* Short circuit data is the last 4 bits active low */
                uint8_t scdata = (past >> 4);
                scdata = (scdata | 0xF0);
                scdata = ~scdata;
                if (scdata) {
                    syslog(LOG_DEBUG, "Short circuit");
                    /* Short circuit */
                    /* Getting outputs */
                    for (size_t i = 0; i < 2; i++) {
                        if (i2c_transfer(&i2c, &pbdata[i], 1) < 0) {
                            syslog(LOG_ERR, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                            exit(EXIT_FAILURE);
                        }
                    }
                    /* Turn off corresponding output(s) */
                    outp[1] = (pbst ^ scdata);
                    if (i2c_transfer(&i2c, output, 1) < 0) {
                        syslog(LOG_ERR, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                        exit(EXIT_FAILURE);
                    }
                    /* Inform clients */
                    iotool_data.command = SHORT_CIRCUIT;
                    iotool_data.input_bits = scdata;
                }
                else {
                    syslog(LOG_DEBUG, "Input");
                    iotool_data.command = INPUT_INFO;
                    iotool_data.input_bits = past;
                }
                /* Send data to clients */
                for (size_t i = 0; i < MAX_CLIENTS; i++) {
                    if (client_socket[i] != 0) {
                        int ret = write(client_socket[i], &iotool_data, sizeof(struct iotool));
                        if (ret < 0) {
                            syslog(LOG_ERR, "Failed to send data. write(): %s", strerror(errno));
                            exit(EXIT_FAILURE);
                        }
                    }
                }
            }
            /* Unix socket new client */
            if (FD_ISSET(master_socket, &rdfs)) {
                syslog(LOG_DEBUG, "New client.");
                if ((new_socket = accept(master_socket,
                    (struct sockaddr *)&remote, (socklen_t*)&t)) < 0) {
                    syslog(LOG_CRIT, "accept(): %s", strerror(errno));
                    return -1;
                }
                /* Add new socket to array of sockets */
                for (size_t i = 0; i < MAX_CLIENTS; i++) {
                    if(client_socket[i] == 0) {
                        client_socket[i] = new_socket;
                        break;
                    }
                }
            }
            /* Unix socket client request */
            else {
                for (size_t i = 0; i < MAX_CLIENTS; i++) {
                    sd = client_socket[i];

                    if (FD_ISSET(sd, &rdfs)) {
                        /* Somebody disconnected */
                        int ret = read(sd, &iotool_data_req, sizeof(struct iotool));
                        if (ret == 0) {
                            /* Close the socket and mark as 0 in list for reuse */
                            syslog(LOG_DEBUG, "Client disconnected.");
                            close(sd);
                            client_socket[i] = 0;
                        }
                        else if (ret < 0) {
                            close(sd);
                            client_socket[i] = 0;
                            syslog(LOG_ERR, "Failed to read client on socket. read(): %s", strerror(errno));
                        }
                        else {
                            switch (iotool_data_req.command) {
                                case SET_OUTPUT_BIT :
                                break;
                                case SET_ALL_OUTPUT_BIT :
                                break;
                                case CLEAR_OUTPUT_BIT :
                                break;
                                case CLEAR_ALL_OUTPUT_BIT :
                                break;
                                default : break;
                            }
                        }
                    }
                }
            }
        }
    }
    else if (pulse) {
        if (outc == -1) {
            fprintf(stderr, "You need to specify an output!\n");
            exit(1);
        }
        /* Getting outputs */
        for (size_t i = 0; i < 2; i++) {
            if (i2c_transfer(&i2c, &pbdata[i], 1) < 0) {
                fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                exit(1);
            }
        }

        for (size_t i = 0; i < periodcnt; i++) {
            outp[1] = (pbst | (uint8_t)outc);
            if (i2c_transfer(&i2c, output, 1) < 0) {
                fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                exit(1);
            }
            usleep(halfperiod-200);
            outp[1] = (pbst & ~(uint8_t)outc);
            if (i2c_transfer(&i2c, output, 1) < 0) {
                fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                exit(1);
            }
            usleep(halfperiod-200);
        }
    }
    else if (seto) {
        /* Getting outputs */
        for (size_t i = 0; i < 2; i++) {
            if (i2c_transfer(&i2c, &pbdata[i], 1) < 0) {
                fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                exit(1);
            }
        }

        if (level == 1)
            outp[1] = (pbst | (uint8_t)outc);
        else
            outp[1] = (pbst & ~(uint8_t)outc);

        if (i2c_transfer(&i2c, output, 1) < 0) {
            fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
            exit(1);
        }
    }

    else if (p) {
        if (gpio_open(&interrupt, PH17, GPIO_DIR_IN) < 0) {
            fprintf(stderr, "gpio_open(): %s\n", gpio_errmsg(&interrupt));
            exit(1);
        }

        if (gpio_set_edge(&interrupt, GPIO_EDGE_FALLING) < 0) {
            fprintf(stderr, "gpio_set_edge(): %s\n", gpio_errmsg(&interrupt));
            exit(1);
        }

        while (!exit_flag) {
            int ret = gpio_poll(&interrupt, timeout);
            if (ret < 0)
                break;
            else if (ret == 0 && timeout != 0) {
                printf("Poll timed out\n");
                break;
            }

            for (size_t i = 0; i < 2; i++) {
                if (i2c_transfer(&i2c, &padata[i], 1) < 0) {
                    fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                    exit(1);
                }
            }
            for (size_t i = 0; i < 4; i++) {
                printf("DI0%zu -> %d\n", i, (past & inputs[i]) ? 1 : 0);
            }
            printf("\n");

            if (timeout == 0)
                break;
        }
    }
    else {
        usage(argv[0]);
    }

    i2c_close(&i2c);
    gpio_close(&interrupt);

    return 0;
}
