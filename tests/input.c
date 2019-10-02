#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include "i2c.h"

#define SYSFS_GPIO_DIR "/sys/class/gpio"
#define MAX_BUF 64
#define TEST_CYCLE_NUM 100

#define I2C_ADDR 0x20
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

/****************************************************************
 * gpio_export
 ****************************************************************/
int gpio_export(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];

	fd = open(SYSFS_GPIO_DIR "/export", O_WRONLY);
	if (fd < 0) {
		perror("gpio/export");
		return fd;
	}

	len = snprintf(buf, sizeof(buf), "%d", gpio);
	write(fd, buf, len);
	close(fd);

	return 0;
}

/****************************************************************
 * gpio_unexport
 ****************************************************************/
int gpio_unexport(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];

	fd = open(SYSFS_GPIO_DIR "/unexport", O_WRONLY);
	if (fd < 0) {
		perror("gpio/export");
		return fd;
	}

	len = snprintf(buf, sizeof(buf), "%d", gpio);
	write(fd, buf, len);
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_fd_open
 ****************************************************************/

int gpio_fd_open(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];

	len = snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR "/gpio%d/value", gpio);

	fd = open(buf, O_RDONLY | O_NONBLOCK );
	if (fd < 0) {
		perror("gpio/fd_open");
	}
	return fd;
}

/****************************************************************
 * gpio_set_dir
 ****************************************************************/
int gpio_set_dir(unsigned int gpio, unsigned int out_flag)
{
	int fd, len;
	char buf[MAX_BUF];

	len = snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR  "/gpio%d/direction", gpio);

	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		perror("gpio/direction");
		return fd;
	}

	if (out_flag)
		write(fd, "out", 4);
	else
		write(fd, "in", 3);

	close(fd);
	return 0;
}

/****************************************************************
 * gpio_set_edge
 ****************************************************************/

int gpio_set_edge(unsigned int gpio, char *edge)
{
	int fd, len;
	char buf[MAX_BUF];

	len = snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR "/gpio%d/edge", gpio);

	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		perror("gpio/set-edge");
		return fd;
	}

	write(fd, edge, strlen(edge) + 1);
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_fd_close
 ****************************************************************/

int gpio_fd_close(int fd)
{
	return close(fd);
}

int main(void) {
    i2c_t i2c;
    struct pollfd fdset;
    uint8_t data, v;
    int gpio_fd, rc;
    char *buf[MAX_BUF];
    int len, cnt;

    /* Open the i2c-0 bus */
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

    uint8_t AADR = 0x12;
    struct i2c_msg padata[] =
        {
            { .addr = I2C_ADDR, .flags = 0, .len = 1, .buf = &AADR },
            { .addr = I2C_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &data }
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

    gpio_export(241);
    gpio_set_dir(241, 0);
    gpio_set_edge(241, "falling");
    gpio_fd = gpio_fd_open(241);

    srand(time(NULL));

    fdset.fd = gpio_fd;
    fdset.events = POLLPRI;

    while (cnt < TEST_CYCLE_NUM) {
        unsigned char r = rand() % 4;
        if (v == r) {
            continue;
        }

        outp[1] = (0x00 | 1 << r);
        v = r;
        cnt++;
        if (i2c_transfer(&i2c, output, 1) < 0) {
            fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
            exit(1);
        }
        printf("O: 0x%.2x\n", outp[1]);

        rc = poll(&fdset, 1, -1);
        if (rc < 0) {
			printf("\npoll() failed!\n");
			return -1;
		}
        if (fdset.revents & POLLPRI) {
			lseek(fdset.fd, 0, SEEK_SET);
			len = read(fdset.fd, buf, MAX_BUF);

            for (size_t i = 0; i < 2; i++) {
                if (i2c_transfer(&i2c, &padata[i], 1) < 0) {
                    fprintf(stderr, "i2c_transfer(): %s\n", i2c_errmsg(&i2c));
                    exit(1);
                }
            }
            printf("I: 0x%.2x\n\n", data & 0x0F);
		}
    }

    i2c_close(&i2c);

    return 0;
}
