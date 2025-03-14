/*******************************************************************************
* Copyright © 2019 TRINAMIC Motion Control GmbH & Co. KG
* (now owned by Analog Devices Inc.),
*
* Copyright © 2024 Analog Devices Inc. All Rights Reserved.
* This software is proprietary to Analog Devices, Inc. and its licensors.
*******************************************************************************/


#include "Board.h"
#include "tmc/ic/TMC2208/TMC2208.h"
#include "tmc/StepDir.h"


#undef  TMC2208_MAX_VELOCITY
#define TMC2208_MAX_VELOCITY  STEPDIR_MAX_VELOCITY

#define STEPDIR_PRECISION (1 << 17) // Stepdir precision: 2^17 -> 17 digits of precision
#define ERRORS_VM        (1<<0)
#define ERRORS_VM_UNDER  (1<<1)
#define ERRORS_VM_OVER   (1<<2)
#define VM_MIN  50   // VM[V/10] min
#define VM_MAX  390  // VM[V/10] max
#define MOTORS 1
#define VREF_FULLSCALE 2100 // mV
#define DEFAULT_ICID  0


static uint8_t nodeAddress = 0;
static UART_Config *TMC2208_UARTChannel;
static timer_channel timerChannel;
static uint16_t vref; // mV


// Usage note: use 1 TypeDef per IC
typedef struct {
    ConfigurationTypeDef *config;
} TMC2208TypeDef;
static TMC2208TypeDef TMC2208;

typedef struct
{
    IOPinTypeDef  *DRV_ENN;
    IOPinTypeDef  *STEP;
    IOPinTypeDef  *DIR;
    IOPinTypeDef  *MS1;
    IOPinTypeDef  *MS2;
    IOPinTypeDef  *DIAG;
    IOPinTypeDef  *INDEX;
    IOPinTypeDef  *UC_PWM;
} PinsTypeDef;
static PinsTypeDef Pins;


static uint32_t right(uint8_t motor, int32_t velocity);
static uint32_t left(uint8_t motor, int32_t velocity);
static uint32_t rotate(uint8_t motor, int32_t velocity);
static uint32_t stop(uint8_t motor);
static uint32_t moveTo(uint8_t motor, int32_t position);
static uint32_t moveBy(uint8_t motor, int32_t *ticks);
static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value);
static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value);
static void checkErrors (uint32_t tick);
static void deInit(void);
static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value);
static void periodicJob(uint32_t tick);
static uint8_t reset(void);
static void enableDriver(DriverState state);
static uint8_t restore(void);


static void writeConfiguration()
{
    uint8_t *ptr = &TMC2208.config->configIndex;
    const int32_t *settings;

    if(TMC2208.config->state == CONFIG_RESTORE)
    {
        settings = *(tmc2208_shadowRegister + 0);
        // Find the next restorable register
        while(*ptr < TMC2208_REGISTER_COUNT)
        {
            // If the register is writable and has been written to, restore it
            if (TMC_IS_WRITABLE(tmc2208_registerAccess[*ptr]) && tmc2208_getDirtyBit(DEFAULT_ICID, *ptr))
            {
                break;
            }

            // Otherwise, check next register
            (*ptr)++;
        }
    }
    else
    {
        settings = tmc2208_sampleRegisterPreset;
        // Find the next resettable register
        while((*ptr < TMC2208_REGISTER_COUNT) && !TMC_IS_RESETTABLE(tmc2208_registerAccess[*ptr]))
        {
            (*ptr)++;
        }
    }

    if(*ptr < TMC2208_REGISTER_COUNT)
    {
        tmc2208_writeRegister(DEFAULT_ICID, *ptr, settings[*ptr]);
        (*ptr)++;
    }
    else // Finished configuration
    {
        TMC2208.config->state = CONFIG_READY;
    }
}

bool tmc2208_readWriteUART(uint16_t icID, uint8_t *data, size_t writeLength, size_t readLength)
{
    UNUSED(icID);
    int32_t status = UART_readWrite(TMC2208_UARTChannel, data, writeLength, readLength);
    if(status == -1)
        return false;
    return true;
}

uint8_t tmc2208_getNodeAddress(uint16_t icID)
{
    UNUSED(icID);
    return nodeAddress;
}

static void writeRegister(uint8_t motor, uint16_t address, int32_t value)
{
    UNUSED(motor);
    tmc2208_writeRegister(DEFAULT_ICID, (uint8_t) address, value);
}

static void readRegister(uint8_t motor, uint16_t address, int32_t *value)
{
    UNUSED(motor);
    *value = tmc2208_readRegister(DEFAULT_ICID, (uint8_t) address);
}

static uint32_t rotate(uint8_t motor, int32_t velocity)
{
    if(motor >= MOTORS)
        return TMC_ERROR_MOTOR;

    StepDir_rotate(motor, velocity);

    return TMC_ERROR_NONE;
}

static uint32_t right(uint8_t motor, int32_t velocity)
{
    return rotate(motor, velocity);
}

static uint32_t left(uint8_t motor, int32_t velocity)
{
    return rotate(motor, -velocity);
}

static uint32_t stop(uint8_t motor)
{
    return rotate(motor, 0);
}

static uint32_t moveTo(uint8_t motor, int32_t position)
{
    if(motor >= MOTORS)
        return TMC_ERROR_MOTOR;

    StepDir_moveTo(motor, position);

    return TMC_ERROR_NONE;
}

static uint32_t moveBy(uint8_t motor, int32_t *ticks)
{
    if(motor >= MOTORS)
        return TMC_ERROR_MOTOR;

    // determine actual position and add numbers of ticks to move
    *ticks += StepDir_getActualPosition(motor);

    return moveTo(motor, *ticks);
}

static uint32_t handleParameter(uint8_t readWrite, uint8_t motor, uint8_t type, int32_t *value)
{
    uint32_t errors = TMC_ERROR_NONE;

    if(motor >= MOTORS)
        return TMC_ERROR_MOTOR;

    switch(type)
    {
    case 0:
        // Target position
        if(readWrite == READ) {
            *value = StepDir_getTargetPosition(motor);
        } else if(readWrite == WRITE) {
            StepDir_moveTo(motor, *value);
        }
        break;
    case 1:
        // Actual position
        if(readWrite == READ) {
            *value = StepDir_getActualPosition(motor);
        } else if(readWrite == WRITE) {
            StepDir_setActualPosition(motor, *value);
        }
        break;
    case 2:
        // Target speed
        if(readWrite == READ) {
            *value = StepDir_getTargetVelocity(motor);
        } else if(readWrite == WRITE) {
            StepDir_rotate(motor, *value);
        }
        break;
    case 3:
        // Actual speed
        if(readWrite == READ) {
            *value = StepDir_getActualVelocity(motor);
        } else if(readWrite == WRITE) {
            errors |= TMC_ERROR_TYPE;
        }
        break;
    case 4:
        // Maximum speed
        if(readWrite == READ) {
            *value = StepDir_getVelocityMax(motor);
        } else if(readWrite == WRITE) {
            StepDir_setVelocityMax(motor, abs(*value));
        }
        break;
    case 5:
        // Maximum acceleration
        if(readWrite == READ) {
            *value = StepDir_getAcceleration(motor);
        } else if(readWrite == WRITE) {
            StepDir_setAcceleration(motor, *value);
        }
        break;
    case 6:
        // UART slave address
        if(readWrite == READ) {
            // The TMC2208 has a hardcoded slave address 0
            *value = 0;
        } else if(readWrite == WRITE) {
            errors |= TMC_ERROR_TYPE;
        }
        break;
    case 9:
        // VREF
        if (readWrite == READ) {
            *value = vref;
        } else {
            if ((uint32_t) *value < VREF_FULLSCALE) {
                vref = *value;
                Timer.setDuty(timerChannel, ((float)vref) / VREF_FULLSCALE);
            } else {
                errors |= TMC_ERROR_VALUE;
            }
        }
        break;
    default:
        errors |= TMC_ERROR_TYPE;
        break;
    }

    return errors;
}

static uint32_t SAP(uint8_t type, uint8_t motor, int32_t value)
{
    return handleParameter(WRITE, motor, type, &value);
}

static uint32_t GAP(uint8_t type, uint8_t motor, int32_t *value)
{
    return handleParameter(READ, motor, type, value);
}

static void checkErrors(uint32_t tick)
{
    UNUSED(tick);
    Evalboards.ch2.errors = 0;
}

static uint32_t userFunction(uint8_t type, uint8_t motor, int32_t *value)
{
    uint32_t errors = 0;

    switch(type)
    {
    case 0:  // Read StepDir status bits
        *value = StepDir_getStatus(motor);
        break;
    default:
        errors |= TMC_ERROR_TYPE;
        break;
    }

    return errors;
}

static void deInit(void)
{
    enableDriver(DRIVER_DISABLE);
    HAL.IOs->config->reset(Pins.DRV_ENN);

    HAL.IOs->config->reset(Pins.STEP);
    HAL.IOs->config->reset(Pins.DIR);
    HAL.IOs->config->reset(Pins.MS1);
    HAL.IOs->config->reset(Pins.MS2);
    HAL.IOs->config->reset(Pins.DIAG);
    HAL.IOs->config->reset(Pins.INDEX);

    StepDir_deInit();
}

static uint8_t reset ()
{
    StepDir_init(STEPDIR_PRECISION);
    StepDir_setPins(0, Pins.STEP, Pins.DIR, NULL);

    //  return tmc2208_reset(&TMC2208);
    if (TMC2208.config->state != CONFIG_READY)
        return false;

    // Reset the dirty bits and wipe the shadow registers
    for (size_t i = 0; i < TMC2208_REGISTER_COUNT; i++)
    {
        tmc2208_setDirtyBit(DEFAULT_ICID, i, false);
        tmc2208_shadowRegister[DEFAULT_ICID][i] = 0;
    }

    TMC2208.config->state       = CONFIG_RESET;
    TMC2208.config->configIndex = 0;

    return true;
}

static uint8_t restore()
{
    if (TMC2208.config->state != CONFIG_READY)
        return false;

    TMC2208.config->state       = CONFIG_RESTORE;
    TMC2208.config->configIndex = 0;

    return true;
}

static void enableDriver(DriverState state)
{
    if (state == DRIVER_USE_GLOBAL_ENABLE)
        state = Evalboards.driverEnable;

    if (state == DRIVER_DISABLE)
        HAL.IOs->config->setHigh (Pins.DRV_ENN);
    else if ((state == DRIVER_ENABLE)
            && (Evalboards.driverEnable == DRIVER_ENABLE))
        HAL.IOs->config->setLow (Pins.DRV_ENN);
}

static void periodicJob(uint32_t tick)
{
    UNUSED(tick);
    if(TMC2208.config->state != CONFIG_READY)
        writeConfiguration();

    StepDir_periodicJob (0);
}

void TMC2208_init(void)
{
#if defined(Landungsbruecke) || defined(LandungsbrueckeSmall)
    timerChannel = TIMER_CHANNEL_3;

#elif defined(LandungsbrueckeV3)
    timerChannel = TIMER_CHANNEL_4;
#endif

    Pins.DRV_ENN  = &HAL.IOs->pins->DIO0;
    Pins.STEP     = &HAL.IOs->pins->DIO6;
    Pins.DIR      = &HAL.IOs->pins->DIO7;
    Pins.MS1      = &HAL.IOs->pins->DIO3;
    Pins.MS2      = &HAL.IOs->pins->DIO4;
    Pins.DIAG     = &HAL.IOs->pins->DIO1;
    Pins.INDEX    = &HAL.IOs->pins->DIO2;
    Pins.UC_PWM   = &HAL.IOs->pins->DIO9;

    HAL.IOs->config->toOutput(Pins.DRV_ENN);
    HAL.IOs->config->toOutput(Pins.STEP);
    HAL.IOs->config->toOutput(Pins.DIR);
    HAL.IOs->config->toOutput(Pins.MS1);
    HAL.IOs->config->toOutput(Pins.MS2);
    HAL.IOs->config->toInput(Pins.DIAG);
    HAL.IOs->config->toInput(Pins.INDEX);

    TMC2208_UARTChannel = HAL.UART;
    TMC2208_UARTChannel->pinout = UART_PINS_1;
    TMC2208_UARTChannel->rxtx.init();

    Evalboards.ch2.config->reset        = reset;
    Evalboards.ch2.config->restore      = restore;

    Evalboards.ch2.rotate               = rotate;
    Evalboards.ch2.right                = right;
    Evalboards.ch2.left                 = left;
    Evalboards.ch2.stop                 = stop;
    Evalboards.ch2.GAP                  = GAP;
    Evalboards.ch2.SAP                  = SAP;
    Evalboards.ch2.moveTo               = moveTo;
    Evalboards.ch2.moveBy               = moveBy;
    Evalboards.ch2.writeRegister        = writeRegister;
    Evalboards.ch2.readRegister         = readRegister;
    Evalboards.ch2.userFunction         = userFunction;
    Evalboards.ch2.enableDriver         = enableDriver;
    Evalboards.ch2.checkErrors          = checkErrors;
    Evalboards.ch2.numberOfMotors       = MOTORS;
    Evalboards.ch2.VMMin                = VM_MIN;
    Evalboards.ch2.VMMax                = VM_MAX;
    Evalboards.ch2.deInit               = deInit;
    Evalboards.ch2.periodicJob          = periodicJob;

    TMC2208.config               = Evalboards.ch2.config;
    TMC2208.config->callback     = NULL;
    TMC2208.config->channel      = 0;
    TMC2208.config->configIndex  = 0;
    TMC2208.config->state        = CONFIG_READY;

    StepDir_init(STEPDIR_PRECISION);
    StepDir_setPins(0, Pins.STEP, Pins.DIR, NULL);
    StepDir_setVelocityMax(0, 51200);
    StepDir_setAcceleration(0, 51200);

    HAL.IOs->config->toOutput(Pins.UC_PWM);

#if defined(Landungsbruecke) || defined(LandungsbrueckeSmall)
    Pins.UC_PWM->configuration.GPIO_Mode = GPIO_Mode_AF4;
#elif defined(LandungsbrueckeV3)
    Pins.UC_PWM->configuration.GPIO_Mode  = GPIO_MODE_AF;
    gpio_af_set(Pins.UC_PWM->port, GPIO_AF_1, Pins.UC_PWM->bitWeight);
#endif

    vref = 2000;
    HAL.IOs->config->set(Pins.UC_PWM);
    Timer.init();
    Timer.setDuty(timerChannel, ((float)vref) / VREF_FULLSCALE);

    enableDriver(DRIVER_ENABLE);
};
