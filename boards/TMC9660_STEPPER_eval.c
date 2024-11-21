/*******************************************************************************
* Copyright © 2024 Analog Devices Inc. All Rights Reserved.
* This software is proprietary to Analog Devices, Inc. and its licensors.
*******************************************************************************/


#include "Board.h"

static SPIChannelTypeDef *TMC9660_STEPPER_SPIChannel;
static UART_Config *TMC9660_STEPPER_UARTChannel;
static uint8_t lastStatus;

#ifdef TMC_API_EXTERNAL_CRC_TABLE
extern const uint8_t tmcCRCTable_Poly7Reflected[256];
#else
const uint8_t tmcCRCTable_Poly7Reflected[256] = {
			0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75, 0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
			0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69, 0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
			0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D, 0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
			0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51, 0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
			0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05, 0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
			0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19, 0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
			0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D, 0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
			0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21, 0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
			0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95, 0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
			0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89, 0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
			0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD, 0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
			0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1, 0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
			0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5, 0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
			0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9, 0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
			0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD, 0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
			0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1, 0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF,
};
#endif

typedef struct
{
    IOPinTypeDef  *SPI1_MOSI;
    IOPinTypeDef  *SPI1_MISO;
    IOPinTypeDef  *SPI1_SCK;
    IOPinTypeDef  *SPI1_CSN;
    IOPinTypeDef  *HOLDN_FLASH;
    IOPinTypeDef  *UART_RX;
    IOPinTypeDef  *UART_TX;
    IOPinTypeDef  *RESET_LB;
    IOPinTypeDef  *DRV_ENABLE;
    IOPinTypeDef  *WAKEN_LB;
    IOPinTypeDef  *FAULTN_LB;
    IOPinTypeDef  *GPIO18_LB;
    IOPinTypeDef  *GPIO17_LB;
} PinsTypeDef;

static PinsTypeDef Pins;

static uint8_t CRC8(uint8_t *data, uint32_t bytes)
{
	uint8_t result = 0;
	uint8_t *table;

	while(bytes--)
		result = tmcCRCTable_Poly7Reflected[result ^ *data++];

	// Flip the result around
	// swap odd and even bits
	result = ((result >> 1) & 0x55) | ((result & 0x55) << 1);
	// swap consecutive pairs
	result = ((result >> 2) & 0x33) | ((result & 0x33) << 2);
	// swap nibbles ...
	result = ((result >> 4) & 0x0F) | ((result & 0x0F) << 4);

	return result;
}

static int32_t processTunnelBL(uint8_t motor, int32_t value)
{
	uint8_t data[8] = { 0 };

	data[0] = 0x55; //Sync byte
	data[1] = 0x01; //Device Address
	data[2] = motor; //Command
	data[3] = (value >> 24) & 0xFF;
	data[4] = (value >> 16) & 0xFF;
	data[5] = (value >> 8 ) & 0xFF;
	data[6] = (value      ) & 0xFF;
	data[7] = CRC8(data, 7);

	UART_readWrite(TMC9660_STEPPER_UARTChannel, &data[0], 8, 8);

	lastStatus = data[2];

	return ((uint32_t)data[3] << 24) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 8) | data[6];
}

static uint8_t calcCheckSum(uint8_t *data, uint32_t bytes)
{
	uint8_t checkSum = 0;

	for(int i =0; i<bytes; i++)
	{
		checkSum += data[i];
	}
	return checkSum;
}

static int32_t processTunnelApp(uint8_t operation, uint8_t type, uint8_t motor, int32_t value, uint8_t *status)
{
	uint8_t data[9] = { 0 };

	data[0] = 0x01; // Module Address
	data[1] = operation; //Operation
	data[2] = type; //type
	data[3] = motor; //motor
	data[4] = (value >> 24) & 0xFF;
	data[5] = (value >> 16) & 0xFF;
	data[6] = (value >> 8 ) & 0xFF;
	data[7] = (value      ) & 0xFF;
	data[8] = calcCheckSum(data, 8);

	int32_t uartStatus = UART_readWrite(TMC9660_STEPPER_UARTChannel, &data[0], 9, 9);

	// Timeout?
	if(uartStatus == -1)
        return 0;

    // Byte 8: CRC correct?
    if (data[8] != calcCheckSum(data, 8))
        return 0;

	*status = data[2];
	return ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) | ((uint32_t)data[6] << 8) | data[7];
}

static uint8_t ramDebug(uint8_t type, uint8_t motor, int32_t *value)
{
	uint8_t status;
	*value = processTunnelApp(142, type, motor, *value, &status);
	return status;
}

static uint32_t SIO(uint8_t type, uint8_t motor, int32_t value)
{
    UNUSED(motor);

    switch(type) {
    case 0: // HOLDN_FLASH
        HAL.IOs->config->setToState(Pins.HOLDN_FLASH, (value) ? IOS_HIGH : IOS_LOW);
        break;
    case 1: // RESET_LB
        HAL.IOs->config->setToState(Pins.RESET_LB, (value) ? IOS_HIGH : IOS_LOW);
        break;
    case 2: // DRV_ENABLE
        HAL.IOs->config->setToState(Pins.DRV_ENABLE, (value) ? IOS_HIGH : IOS_LOW);
        break;
    case 3: // WAKEN_LB
        HAL.IOs->config->setToState(Pins.WAKEN_LB, (value) ? IOS_HIGH : IOS_LOW);
        break;
    default:
        return TMC_ERROR_TYPE;
    }

    return TMC_ERROR_NONE;
}

static uint32_t GIO(uint8_t type, uint8_t motor, int32_t *value)
{
    UNUSED(motor);

    switch(type) {
    case 0: // HOLDN_FLASH
        *value = HAL.IOs->config->getState(Pins.HOLDN_FLASH);
        break;
    case 1: // RESET_LB
        *value = HAL.IOs->config->getState(Pins.RESET_LB);
        break;
    case 2: // DRV_ENABLE
        *value = HAL.IOs->config->getState(Pins.DRV_ENABLE);
        break;
    case 3: // WAKEN_LB
        *value = HAL.IOs->config->getState(Pins.WAKEN_LB);
        break;
    case 4: // FAULTN_LB
        *value = HAL.IOs->config->getState(Pins.FAULTN_LB);
        break;
    default:
        return TMC_ERROR_TYPE;
    }

    return TMC_ERROR_NONE;
}

static uint32_t SGP(uint8_t type, uint8_t motor, int32_t value)
{
    uint8_t status;
    processTunnelApp(9, type, motor, value, &status);
    return ((uint32_t)status);
}

static uint32_t GGP(uint8_t type, uint8_t motor, int32_t *value)
{
    uint8_t status;
    *value = processTunnelApp(10, type, motor, *value, &status);
    return ((uint32_t)status);
}

static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value)
{
    uint8_t status;
    processTunnelApp(5, type, motor, value, &status);
    return ((uint32_t)status);

}

static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value)
{
    uint8_t status;
    *value = processTunnelApp(6, type, motor, *value, &status);
    return ((uint32_t)status);
}

static uint32_t STAP(uint8_t type, uint8_t motor, int32_t value)
{
    uint8_t status;
    processTunnelApp(7, type, motor, value, &status);
    return ((uint32_t)status);
}

static uint32_t getInfo(uint8_t type, uint8_t motor, int32_t *value)
{
	uint8_t status;
	*value = processTunnelApp(157, type, motor, *value, &status);
	return ((uint32_t)status);
}

static void writeRegister(uint8_t motor, uint16_t type, int32_t value)
{
	processTunnelApp(146, (uint8_t)type, motor, value, 0);
}

static void readRegister(uint8_t motor, uint16_t type, int32_t *value)
{
	*value = processTunnelApp(148, (uint8_t)type, motor, *value, 0);
}

static void initTunnel(void)
{
    //Deinit SPI
    HAL.IOs->config->reset(Pins.SPI1_SCK);
    HAL.IOs->config->reset(Pins.SPI1_MOSI);
    HAL.IOs->config->reset(Pins.SPI1_MISO);
    HAL.IOs->config->reset(Pins.SPI1_CSN);
    HAL.IOs->config->toInput(Pins.SPI1_SCK);
    HAL.IOs->config->toInput(Pins.SPI1_MOSI);
    HAL.IOs->config->toInput(Pins.SPI1_MISO);
    HAL.IOs->config->toInput(Pins.SPI1_CSN);

    TMC9660_STEPPER_UARTChannel = HAL.UART;
    TMC9660_STEPPER_UARTChannel->pinout = UART_PINS_2;
    TMC9660_STEPPER_UARTChannel->rxtx.init();

//    HAL.IOs->config->setHigh(Pins.SPI_EN);
//    HAL.IOs->config->setLow(Pins.I2C_EN);
//    HAL.IOs->config->setHigh(Pins.HOLD_FLASHN);
//    HAL.IOs->config->setLow(Pins.RESETN);
}

static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value)
{
    UNUSED(motor);
    uint32_t errors = TMC_ERROR_NONE;

    switch(type)
    {
    case 0:
    	// Process Tunnel Commands
    	*value = processTunnelBL(motor, *value);
    	break;
    case 1:
    	// Return status byte
    	*value = lastStatus;
        break;
    case 2:
        // Get Module ID of App

        *value = processTunnelApp(157, 0, 0, 0, 0);
        break;
    default:
        errors |= TMC_ERROR_TYPE;
        break;
    }
    return errors;
}

void TMC9660_STEPPER_init(void)
{
    Pins.SPI1_SCK              = &HAL.IOs->pins->SPI1_SCK;
    Pins.SPI1_MOSI             = &HAL.IOs->pins->SPI1_SDI;
    Pins.SPI1_MISO             = &HAL.IOs->pins->SPI1_SDO;
    Pins.SPI1_CSN              = &HAL.IOs->pins->SPI1_CSN;
    Pins.HOLDN_FLASH              = &HAL.IOs->pins->DIO12;
    Pins.WAKEN_LB              = &HAL.IOs->pins->DIO9;
    Pins.RESET_LB              = &HAL.IOs->pins->DIO8;
    Pins.FAULTN_LB              = &HAL.IOs->pins->DIO7;
    Pins.DRV_ENABLE             = &HAL.IOs->pins->DIO6;


#if defined(LandungsbrueckeV3)
    Pins.UART_RX               = &HAL.IOs->pins->DIO10_UART_TX; //Pin21
    Pins.UART_TX               = &HAL.IOs->pins->DIO11_UART_RX; //Pin22

    //Set MUX_1 and MUX_2 to zero to connect DIO10 and DIO11 to UART pins DIO10_UART_TX and DIO11_UART_RX respectively.
    *HAL.IOs->pins->SW_UART_PWM.resetBitRegister     = HAL.IOs->pins->SW_UART_PWM.bitWeight;
#else
    Pins.UART_RX               = &HAL.IOs->pins->DIO10; //Pin21
    Pins.UART_TX               = &HAL.IOs->pins->DIO11; //Pin22
#endif

//    HAL.IOs->config->toOutput(Pins.HOLDN_FLASH);
//    HAL.IOs->config->toOutput(Pins.WAKEN_LB);
//    HAL.IOs->config->toOutput(Pins.RESET_LB);
//    HAL.IOs->config->toOutput(Pins.DRV_ENABLE);

//    SPI.init();
//    TMC9660_STEPPER_SPIChannel = &HAL.SPI->ch1;
//    TMC9660_STEPPER_SPIChannel->CSN = &HAL.IOs->pins->SPI1_CSN;

    initTunnel();

	Evalboards.ch1.GAP                  = GAP;
	Evalboards.ch1.SAP                  = SAP;
	Evalboards.ch1.STAP                 = STAP;
	Evalboards.ch1.GGP                  = GGP;
	Evalboards.ch1.SGP                  = SGP;
    Evalboards.ch1.userFunction         = userFunction;
    Evalboards.ch1.ramDebug             = ramDebug;
	Evalboards.ch1.writeRegister        = writeRegister;
	Evalboards.ch1.readRegister         = readRegister;
	Evalboards.ch1.getInfo              = getInfo;
    Evalboards.ch1.SIO                  = SIO;
    Evalboards.ch1.GIO                  = GIO;



}
