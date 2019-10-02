#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "spi.h"

#define TRUE 1
#define FALSE 0

int
MAX31855(uint8_t *auchReadBuffer, float *fTemp)
{
    int nTemporaryValue=0;
    int nTemporaryValue2=0;
    int i;
    int nReturnVal=TRUE;
    float fTempValue=0.0f;
    // Check for various error codes

    if(((auchReadBuffer[3] & 0x01)==0x01) ||			// Open Circuit
	    ((auchReadBuffer[3] & 0x02)==0x02) ||		// Short to GND
	    ((auchReadBuffer[3] & 0x04)==0x04) ||		// Short to VCC
	    ((auchReadBuffer[1] & 0x01)==0x01))			// Fault
    {
	// Set all data to zero
	   nReturnVal=FALSE;
    }
    else {
    	// Internal Temp

    	nTemporaryValue = auchReadBuffer[0];  		// bits 11..4
    	nTemporaryValue = nTemporaryValue << 6;
    	nTemporaryValue2 = auchReadBuffer[1];
    	nTemporaryValue2 = nTemporaryValue2 >> 2;
    	nTemporaryValue |= nTemporaryValue2;

    	if((auchReadBuffer[0] & 0x80)==0x80)		// Check the sign bit and sign-extend if need be
    	    nTemporaryValue |= 0xFFFFE000;

    	fTempValue = (float)nTemporaryValue;
    	fTempValue = fTempValue / 4.0f;
    	*fTemp = fTempValue;
    	nReturnVal = TRUE;
    }

    return(nReturnVal);

}
int
main(void)
{
    spi_t spi;
    uint8_t buf[4];

    /* Open spidev1.0 with mode 0 and max speed 1MHz */
    if (spi_open(&spi, "/dev/spidev1.1", 0, 1000000) < 0) {
        fprintf(stderr, "spi_open(): %s\n", spi_errmsg(&spi));
        exit(1);
    }

    /* Shift out and in 4 bytes */
    if (spi_transfer(&spi, NULL, buf, sizeof(buf)) < 0) {
        fprintf(stderr, "spi_transfer(): %s\n", spi_errmsg(&spi));
        exit(1);
    }

    float val;
    int i=MAX31855(buf, &val);

    if (i==FALSE)
	   printf("fault\n");
    else
	   printf("temp: %f Celsius\n", val);

    spi_close(&spi);

    return 0;
}
