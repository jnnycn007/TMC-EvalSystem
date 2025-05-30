/*******************************************************************************
* Copyright © 2020 TRINAMIC Motion Control GmbH & Co. KG
* (now owned by Analog Devices Inc.),
*
* Copyright © 2023 Analog Devices Inc. All Rights Reserved.
* This software is proprietary to Analog Devices, Inc. and its licensors.
*******************************************************************************/


#include "RAMDebug.h"

#include "boards/Board.h"
#include "hal/SysTick.h"
#include "hal/HAL.h"

#include <string.h>

// === RAM debugging ===========================================================

// Debug parameters
#define RAMDEBUG_MAX_CHANNELS     4
#define RAMDEBUG_BUFFER_SIZE      32768
#define RAMDEBUG_BUFFER_ELEMENTS  (RAMDEBUG_BUFFER_SIZE / 4)

bool captureEnabled = false;

// Sample Buffer
uint32_t debug_buffer[RAMDEBUG_BUFFER_ELEMENTS] = { 0 };
uint32_t debug_write_index = 0;
uint32_t debug_start_index = 0;


RAMDebugState state = RAMDEBUG_IDLE;

static bool global_enable = false;
static bool processing = false;
static bool use_next_process = true;
static bool next_process = false;

// Sampling options
static uint32_t prescaler   = 1;
static uint32_t frequency   = RAMDEBUG_FREQUENCY;
static uint32_t sampleCount = RAMDEBUG_BUFFER_ELEMENTS;
static uint32_t sampleCountPre = 0;

typedef struct {
    RAMDebugSource type;
    uint8_t eval_channel;
    uint32_t address;
} Channel;

Channel channels[RAMDEBUG_MAX_CHANNELS];

typedef struct {
    Channel          channel;
    RAMDebugTrigger  type;
    uint32_t         threshold;
    uint32_t         mask;
    uint8_t          shift;
} Trigger;

Trigger trigger;

// Store whether the last sampling point was above or below the trigger threshold
static bool wasAboveSigned   = 0;
static bool wasAboveUnsigned = 0;

// Function declarations
static uint32_t readChannel(Channel channel);

// === Capture and trigger logic ===============================================

// This function only gets called by the interrupt handler.
void handleTriggering()
{
    // Abort if not in the right state
    if (state != RAMDEBUG_TRIGGER)
        return;

    // Read the trigger channel value and apply mask/shift values
    uint32_t value_raw = readChannel(trigger.channel);
    value_raw = (value_raw & trigger.mask) >> trigger.shift;

    // Create a signed version of the trigger value
    int32_t value = value_raw;
    // Create a mask with only the highest bit of the trigger channel mask set
    uint32_t msbMask = (trigger.mask>>trigger.shift) ^ (trigger.mask>>(trigger.shift+1));
    // Check if our value has that bit set.
    if (value_raw & msbMask)
    {
        // If yes, sign-extend it
        value |= ~(trigger.mask>>trigger.shift);
    }

    bool isAboveSigned   = value > (int32_t)  trigger.threshold;
    bool isAboveUnsigned = value_raw > (uint32_t) trigger.threshold;

    switch(trigger.type)
    {
    case TRIGGER_UNCONDITIONAL:
        state = RAMDEBUG_CAPTURE;
        break;
    case TRIGGER_RISING_EDGE_SIGNED:
        if (!wasAboveSigned && isAboveSigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    case TRIGGER_FALLING_EDGE_SIGNED:
        if (wasAboveSigned && !isAboveSigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    case TRIGGER_DUAL_EDGE_SIGNED:
        if (wasAboveSigned != isAboveSigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    case TRIGGER_RISING_EDGE_UNSIGNED:
        if (!wasAboveUnsigned && isAboveUnsigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    case TRIGGER_FALLING_EDGE_UNSIGNED:
        if (wasAboveUnsigned && !isAboveUnsigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    case TRIGGER_DUAL_EDGE_UNSIGNED:
        if (wasAboveUnsigned != isAboveUnsigned)
        {
            state = RAMDEBUG_CAPTURE;
        }
        break;
    default:
        break;
    }

    if (state == RAMDEBUG_CAPTURE)
    {
        // Store the buffer index where we started capturing
        debug_start_index = (debug_write_index - sampleCountPre) % RAMDEBUG_BUFFER_ELEMENTS;
    }

    // Store the last threshold comparison value
    wasAboveSigned   = isAboveSigned;
    wasAboveUnsigned = isAboveUnsigned;
}

// This function only gets called by the interrupt handler.
void handleDebugging()
{
    int32_t i;

    if (state == RAMDEBUG_IDLE)
        return;

    if (state == RAMDEBUG_COMPLETE)
        return;

    for (i = 0; i < RAMDEBUG_MAX_CHANNELS; i++)
    {
        if (channels[i].type == CAPTURE_DISABLED)
            continue;

        // Add the sample value to the buffer
        debug_buffer[debug_write_index] = readChannel(channels[i]);

        if (++debug_write_index == RAMDEBUG_BUFFER_ELEMENTS)
        {
            debug_write_index = 0;

            // If we filled the entire buffer, the pretrigger phase is finished
            if (state == RAMDEBUG_PRETRIGGER)
            {
                state = RAMDEBUG_TRIGGER;
            }
        }

        if (state == RAMDEBUG_CAPTURE)
        {
            uint32_t samplesWritten = (debug_write_index - debug_start_index + RAMDEBUG_BUFFER_ELEMENTS) % RAMDEBUG_BUFFER_ELEMENTS;
            if (samplesWritten == 0 || samplesWritten >= sampleCount)
            {
                // End the capture
                state = RAMDEBUG_COMPLETE;
                captureEnabled = false;
                break;
            }
        }
    }
}

void debug_process()
{
    static uint32_t prescalerCount = 0;

    if(!global_enable)
        return;

    if(processing)
        return;

    if(use_next_process && (!next_process))
        return;

    next_process = false;

    if (captureEnabled == false)
        return;

    if (state == RAMDEBUG_PRETRIGGER)
    {
        // Check if the requested pretrigger data has been captured
        // Note that this does not cover the case when the amount of pretrigger
        // samples equals the amount of possible samples. In that case the write
        // index wraps back to zero and does not allow this check to succeed.
        // For that case the moving write pointer logic takes care of updating
        // the state.
        if (debug_write_index >= sampleCountPre)
        {
            state = RAMDEBUG_TRIGGER;
        }
    }

    handleTriggering();

    // Increment and check the prescaler counter
    if (++prescalerCount < prescaler)
        return;

    processing = true;

    // Reset the prescaler counter
    prescalerCount = 0;

    handleDebugging();
    processing = false;
}

static inline uint32_t readChannel(Channel channel)
{
    uint32_t sample = 0;

    switch (channel.type)
    {
    case CAPTURE_PARAMETER:
    {
        uint8_t motor   = (channel.address >> 24) & 0xFF;
        uint8_t type    = (channel.address >> 0) & 0xFF;

        ((channel.eval_channel == 1) ? (&Evalboards.ch2) : (&Evalboards.ch1))->GAP(type, motor, (int32_t *)&sample);

        break;
    }
    case CAPTURE_REGISTER:
    {
        uint8_t motor = channel.address >> 24;

        ((channel.eval_channel == 1) ? (&Evalboards.ch2) : (&Evalboards.ch1))->readRegister(motor, channel.address, (int32_t *)&sample);

        break;
    }
    case CAPTURE_STACKED_REGISTER:
    {
        uint8_t motor                  = channel.address >> 24;
        uint8_t stackedRegisterValue   = channel.address >> 16;
        uint8_t stackedRegisterAddress = channel.address >> 8;
        uint8_t dataRegisterAddress    = channel.address >> 0;

        EvalboardFunctionsTypeDef *ch = (channel.eval_channel == 1) ? (&Evalboards.ch2) : (&Evalboards.ch1);

        // Backup the stacked address
        uint32_t oldAddress = 0;
        ch->readRegister(motor, stackedRegisterAddress, (int32_t *)&oldAddress);

        // Write the new stacked address
        ch->writeRegister(motor, stackedRegisterAddress, stackedRegisterValue);

        // Read the stacked data register
        ch->readRegister(motor, dataRegisterAddress, (int32_t *)&sample);

        // Restore the stacked address
        ch->writeRegister(motor, stackedRegisterAddress, oldAddress);
        break;
    }
    case CAPTURE_SYSTICK:
        sample = systick_getTick();//systick_getTimer10ms();
        break;
    case CAPTURE_ANALOG_INPUT:
        // Use same indices as in TMCL.c GetInput()
        switch(channel.address) {
        case 0:
            sample = *HAL.ADCs->AIN0;
            break;
        case 1:
            sample = *HAL.ADCs->AIN1;
            break;
        case 2:
            sample = *HAL.ADCs->AIN2;
            break;
        case 3:
            sample = *HAL.ADCs->DIO4;
            break;
        case 4:
            sample = *HAL.ADCs->DIO5;
            break;
        case 6:
            sample = *HAL.ADCs->VM;
            break;
        }
        break;
    default:
        sample = 0;
        break;
    }

    return sample;
}

// === Interfacing with the debugger ===========================================
void debug_init()
{
    int32_t i;

    // Disable data capture before changing the configuration
    captureEnabled = false;

    // Reset the RAMDebug state
    state = RAMDEBUG_IDLE;

    // Wipe the RAM debug buffer
    for (i = 0; i < RAMDEBUG_BUFFER_ELEMENTS; i++)
    {
        debug_buffer[i] = 0;
    }
    debug_write_index  = 0;

    // Set default values for the capture configuration
    prescaler   = 1;
    sampleCount = RAMDEBUG_BUFFER_ELEMENTS;
    sampleCountPre = 0;

    // Reset the channel configuration
    for (i = 0; i < RAMDEBUG_MAX_CHANNELS; i++)
    {
        channels[i].type = CAPTURE_DISABLED;
        channels[i].eval_channel = 0;
        channels[i].address = 0;
    }

    // Reset the trigger
    trigger.channel.type     = CAPTURE_DISABLED;
    trigger.channel.address  = 0;
    trigger.mask             = 0xFFFFFFFF;
    trigger.shift            = 0;

    global_enable = true;
}

bool debug_setChannel(uint8_t type, uint32_t channel_value)
{
    return (
        debug_setEvalChannel((channel_value >> 16) & 0x01) &&
        debug_setAddress(channel_value) &&
        debug_setType(type)
    );
}

bool debug_setTriggerChannel(uint8_t type, uint32_t channel_value)
{
    return (
        debug_setTriggerType(type) &&
        debug_setTriggerEvalChannel((channel_value >> 16) & 0x01) &&
        debug_setTriggerAddress(channel_value)
    );
}

bool debug_setType(uint8_t type)
{
    int32_t i;

    if (type >= CAPTURE_END)
        return false;

    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    for (i = 0; i < RAMDEBUG_MAX_CHANNELS; i++)
    {
        if (channels[i].type != CAPTURE_DISABLED)
            continue;

        // Add the configuration to the found channel
        channels[i].type     = type;

        return true;
    }

    return false;
}

bool debug_setEvalChannel(uint8_t eval_channel)
{
    int32_t i;

    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    for (i = 0; i < RAMDEBUG_MAX_CHANNELS; i++)
    {
        if (channels[i].type != CAPTURE_DISABLED)
            continue;

        // Add the configuration to the found channel
        channels[i].eval_channel     = eval_channel;

        return true;
    }

    return false;
}

bool debug_setAddress(uint32_t address)
{
    int32_t i;

    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    for (i = 0; i < RAMDEBUG_MAX_CHANNELS; i++)
    {
        if (channels[i].type != CAPTURE_DISABLED)
            continue;

        // Add the configuration to the found channel
        channels[i].address  = address;

        return true;
    }

    return false;
}

int32_t debug_getChannelType(uint8_t index, uint8_t *type)
{
    if (index == 0xFF)
    {
        *type = trigger.channel.type;
        return 1;
    }

    if (index >= RAMDEBUG_MAX_CHANNELS)
        return 0;

    *type = channels[index].type;

    return 1;
}

int32_t debug_getChannelAddress(uint8_t index, uint32_t *address)
{
    if (index == 0xFF)
    {
        *address = trigger.channel.address;
        return 1;
    }

    if (index >= RAMDEBUG_MAX_CHANNELS)
        return 0;

    *address = channels[index].address;

    return 1;
}

bool debug_setTriggerType(uint8_t type)
{
    if (type >= CAPTURE_END)
        return false;

    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    // Store the trigger configuration
    trigger.channel.type     = type;

    return true;
}

bool debug_setTriggerEvalChannel(uint8_t eval_channel)
{
    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    // Store the trigger configuration
    trigger.channel.eval_channel     = eval_channel;

    return true;
}

bool debug_setTriggerAddress(uint32_t address)
{
    if (state != RAMDEBUG_IDLE)
        return false;

    // ToDo: Type-specific address verification logic?

    // Store the trigger configuration
    trigger.channel.address     = address;

    return true;
}

void debug_setTriggerMaskShift(uint32_t mask, uint8_t shift)
{
    trigger.mask  = mask;
    trigger.shift = shift;
}

int32_t debug_enableTrigger(uint8_t type, uint32_t threshold)
{
    // Parameter validation
    if (type >= TRIGGER_END)
        return 0;

    if (state != RAMDEBUG_IDLE)
        return 0;

    // Do not allow the edge triggers with channel still missing
    if (type != TRIGGER_UNCONDITIONAL && trigger.channel.type == CAPTURE_DISABLED)
        return 0;

    // Store the trigger configuration
    trigger.type = type;
    trigger.threshold = threshold;

    // Initialize the trigger helper variable
    // Read out the trigger value and apply the mask/shift
    int32_t triggerValue = (readChannel(trigger.channel) & trigger.mask) >> trigger.shift;
    wasAboveSigned   = (int32_t)  triggerValue > (int32_t)  trigger.threshold;
    wasAboveUnsigned = (uint32_t) triggerValue > (uint32_t) trigger.threshold;

    // Enable the trigger
    state = RAMDEBUG_PRETRIGGER;

    // Enable the capturing IRQ
    captureEnabled = true;

    return 1;
}

void debug_setPrescaler(uint32_t divider)
{
    prescaler = divider;
}

void debug_setSampleCount(uint32_t count)
{
    if (count > RAMDEBUG_BUFFER_ELEMENTS)
        count = RAMDEBUG_BUFFER_ELEMENTS;

    sampleCount = count;
}

uint32_t debug_getSampleCount()
{
    return sampleCount;
}

void debug_setPretriggerSampleCount(uint32_t count)
{
    if (count > sampleCount)
        count = sampleCount;

    sampleCountPre = count;
    debug_write_index = count;
}

uint32_t debug_getPretriggerSampleCount()
{
    return sampleCountPre;
}

bool debug_getSample(uint32_t index, uint32_t *value)
{
    if (index >= sampleCount)
        return false;

    if (state != RAMDEBUG_COMPLETE)
    {
        if (state != RAMDEBUG_CAPTURE)
            return false;

        // If we are in CAPTURE state and the user requested data
        // thats already captured, allow the access
        if (index > ((debug_write_index - debug_start_index) % RAMDEBUG_BUFFER_ELEMENTS))
            return false;
    }

    *value = debug_buffer[(index + debug_start_index) % RAMDEBUG_BUFFER_ELEMENTS];

    return true;
}

void debug_updateFrequency(uint32_t freq)
{
    frequency = freq;
}

RAMDebugState debug_getState(void)
{
    return state;
}

bool debug_getInfo(uint32_t type, uint32_t *infoValue)
{
    switch(type)
    {
    case RAMDEBUG_INFO_MAX_CHANNELS:
        *infoValue = RAMDEBUG_MAX_CHANNELS;
        break;
    case RAMDEBUG_INFO_BUFFER_SIZE:
        *infoValue = RAMDEBUG_BUFFER_ELEMENTS;
        break;
    case RAMDEBUG_INFO_SAMPLING_FREQ:
        // PWM/Sampling Frequency
        *infoValue = frequency; // RAMDEBUG_FREQUENCY;
        break;
    case RAMDEBUG_INFO_SAMPLE_NUMBER:
        *infoValue = debug_write_index;
        break;
    default:
        return false;
    }

    return true;
}

void debug_useNextProcess(bool enable)
{
    use_next_process = enable;
}

void debug_nextProcess(void)
{
    next_process = true;
}

void debug_setGlobalEnable(bool enable)
{
    global_enable = enable;
}
