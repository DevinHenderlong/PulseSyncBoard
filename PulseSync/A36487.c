#include "A36487.h"


//Global Variables
CommandStringStruct command_string;
BUFFERBYTE64 uart2_input_buffer;
BUFFERBYTE64 uart2_output_buffer;
PULSE_PARAMETERS psb_params;
PSB_FAULTS psb_faults;
PSB_DATA psb_data;

//Limited scope variables
unsigned char change_pulse_width_counter;
//unsigned char state;
const unsigned int  dos_intensities[4] = {15, 95, 175, 255};  // fixed constants

void ReadTrigPulseWidth(void);
unsigned char FilterTrigger(unsigned char param);
void ReadAndSetEnergy(void);
void ProgramShiftRegisters(void);
unsigned int GetInterpolationValue(unsigned int low_point, unsigned int high_point, unsigned low_value, unsigned high_value, unsigned point);
void SetStatusAndLEDs(void);


void DoPulseSync(void) {
    psb_data.state_machine = STATE_INIT;
    MACRO_ClrWdt()

    while (1) {
        switch (psb_data.state_machine) {
            case STATE_INIT:
                MACRO_ClrWdt()
                Initialize();
                ETMCanInitialize();
                MACRO_ClrWdt()
                psb_data.personality = ReadDosePersonality();
                MACRO_ClrWdt()
                psb_data.local_state = STATE_WAIT_FOR_CONFIG;
                break;

            case STATE_WAIT_FOR_CONFIG:
                MACRO_ClrWdt()
                SetStatusAndLEDs();
                ETMCanDoCan();
                break;

            case STATE_RUN:
                MACRO_ClrWdt()
                SetStatusAndLEDs();
                ETMCanDoCan();
                break;

            case STATE_FAULT:
                MACRO_ClrWdt()
                psb_data.enable_pulses = 0;
                SetStatusAndLEDs();
                ETMCanDoCan();
                break;
        }
    }
}

void __attribute__((interrupt(__save__(CORCON,SR)), no_auto_psv)) _INT3Interrupt(void)
{
    if (psb_data.prf_ok_to_pulse) {
        TMR5 = 0;                   //Clear 2.4ms interrupt flag
        _T5IF = 0;
        psb_data.prf_ok_to_pulse = 0;
    }
    else {
        psb_faults.prf_fault = 1;
    }

    if (PIN_TRIG_INPUT != ILL_TRIG_ON)
    {
        psb_data.pulse_counter++;
        ReadTrigPulseWidth();
        ReadAndSetEnergy();

        if (psb_data.local_state & XRAY_ON) {
            psb_data.pulses_on++;
            psb_data.pulses_off = 0;
        }
        else {
            psb_data.pulses_on = 0;
            psb_data.pulses_off++;
        }
    }
    else  // if pulse trig stays on, set to minimum dose and flag fault
    {
        psb_faults.trigger_fault = 1;
        psb_data.trigger_filtered = 0;

        psb_data.pulses_on = 0;
        psb_data.pulses_off = 0;
    }

    ProgramShiftRegisters();

    ETMCanPulseSyncSendNextPulseLevel(psb_data.energy, psb_data.pulses_on + 1);
    if (etm_can_status_register.status_word_0 & ETM_CAN_STATUS_WORD_0_HIGH_SPEED_LOGGING_ENABLED)
        ETMCanLogCustomPacketC();

    _INT3IF = 0;		// Clear Interrupt flag
}

void ReadTrigPulseWidth(void)
{
      unsigned int data;
      unsigned char i;

      PIN_SPI_CLK_OUT  = 0;
      MACRO_NOP()
      PIN_PW_SHIFT_OUT = !OLL_PW_SHIFT; // load the reg
      MACRO_NOP()
      __delay32(1); // 100ns for 10M TCY
      PIN_PW_SHIFT_OUT = OLL_PW_SHIFT;  // enable shift
      MACRO_NOP()
      __delay32(1); // 100ns for 10M TCY

      data = PIN_SPI_DATA_IN;

      for (i = 0; i < 8; i++)
      {
      	PIN_SPI_CLK_OUT = 1;
        MACRO_NOP()
        data <<= 1;
        data |= PIN_SPI_DATA_IN;
      	PIN_SPI_CLK_OUT = 0;
        MACRO_NOP()
        __delay32(1); // 100ns for 10M TCY
      }

      PIN_PW_SHIFT_OUT = !OLL_PW_SHIFT; // make load active when idle
      MACRO_NOP()

      if (data & 0x0100)  // counter overflow
      {
          psb_data.trigger_input = 0xFF;
      }
      else
      {
          psb_data.trigger_input = data & 0xFF;
      }
      psb_data.trigger_filtered = FilterTrigger(psb_data.trigger_input);

      if (psb_data.trigger_filtered < 245)   //signify to pfn control board what mode to expect
          PIN_MODE_OUT = OLL_MODE_PORTAL;   //so it can use a different target
      else                                  //current setpoint for low energy
          PIN_MODE_OUT = OLL_MODE_GANTRY;
}

unsigned char FilterTrigger(unsigned char param)
{
    int x;

    //Establish Dose Levels to reduce jitter and provide consistent dose vs trigger width
    //Every bit represents 20ns pulse width change on the electron gun
    //Every bit also represents a 200ns pulse width change from the customer
    if (param > (DOSE_LEVELS - 1))
    {
        for (x = 0; x <= (param % DOSE_LEVELS); x++)
            param--;
    }
    else
        param = 0;

    //Ensure that at least 15 of the same width pulses in a row only change the sampled width
    if (param != psb_data.last_trigger_filtered)
    {
        change_pulse_width_counter++;
        if (change_pulse_width_counter < 15)
            param = psb_data.last_trigger_filtered;
        else
            psb_data.last_trigger_filtered = param;
    }
    else
        change_pulse_width_counter = 0;

    return param;
}

void ReadAndSetEnergy()
{
    if ((PIN_LOW_MODE_IN == HI) && (PIN_HIGH_MODE_IN == HI))
    {
        if (PIN_ENERGY_CMD_IN1 == HI)
        {
            MACRO_NOP()
            PIN_AFC_TRIGGER_OK_OUT = OLL_AFC_TRIGGER_OK;    //Trigger the AFC in high energy only
            MACRO_NOP()
            PIN_GUN_POLARITY_OUT = !OLL_GUN_POLARITY;
            MACRO_NOP()
            PIN_ENERGY_CPU_OUT = !OLL_ENERGY_CPU;
            MACRO_NOP()
            psb_data.energy = HI;
        }
        else
        {
            MACRO_NOP()
            PIN_AFC_TRIGGER_OK_OUT = !OLL_AFC_TRIGGER_OK;   //Do not trigger the AFC in low energy
            MACRO_NOP()
            PIN_GUN_POLARITY_OUT = !OLL_GUN_POLARITY;
            MACRO_NOP()
            PIN_ENERGY_CPU_OUT = OLL_ENERGY_CPU;
            MACRO_NOP()
            psb_data.energy = LOW;
        }
    }
    else
    {
        if (PIN_HIGH_MODE_IN == HI)
        {
            MACRO_NOP()
            PIN_AFC_TRIGGER_OK_OUT = OLL_AFC_TRIGGER_OK;    //Trigger the AFC in single energy mode
            MACRO_NOP()
            PIN_GUN_POLARITY_OUT = OLL_GUN_POLARITY;
            MACRO_NOP()
            PIN_ENERGY_CPU_OUT = OLL_ENERGY_CPU;
            MACRO_NOP()
            psb_data.energy = LOW;
        }
        else
        {
            MACRO_NOP()
            PIN_AFC_TRIGGER_OK_OUT = OLL_AFC_TRIGGER_OK;    //Trigger the AFC in single energy mode
            MACRO_NOP()
            PIN_GUN_POLARITY_OUT = OLL_GUN_POLARITY;
            MACRO_NOP()
            PIN_ENERGY_CPU_OUT = !OLL_ENERGY_CPU;
            MACRO_NOP()
            psb_data.energy = HI;
        }
    }
}

void ProgramShiftRegisters(void)
{
    unsigned int p;
    unsigned int q;
    unsigned long temp;
    unsigned long bittemp;

    PIN_PW_CLR_CNT_OUT = OLL_PW_CLR_CNT;			 // clear width count
    MACRO_NOP()
    PIN_PW_HOLD_LOWRESET_OUT = !OLL_PW_HOLD_LOWRESET;	 // reset start to disable pulse
    MACRO_NOP()

    // do inteplation for grid delay and grid width
    for (p = 0; p < 4; p++)
    {
    	if (psb_data.trigger_filtered <= dos_intensities[p]) break;
    }
    
    if (p == 0)
    {
        if (psb_data.energy == HI) {
            psb_data.grid_delay = psb_params.grid_delay_high0;
            psb_data.grid_width = psb_params.grid_width_high0;
        }
        else {
            psb_data.grid_delay = psb_params.grid_delay_low0;
            psb_data.grid_width = psb_params.grid_width_low0;
        }
    }
    else if (p >= 4)
    {
        if (psb_data.energy == HI) {
            psb_data.grid_delay = psb_params.grid_delay_high3;
            psb_data.grid_width = psb_params.grid_width_high3;
        }
        else {
            psb_data.grid_delay = psb_params.grid_delay_low3;
            psb_data.grid_width = psb_params.grid_width_low3;
        }
    }
    else // interpolation
    {
        if (p == 1) {
            if (psb_data.energy == HI) {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_high0, psb_params.grid_delay_high1, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_high0, psb_params.grid_width_high1, psb_data.trigger_filtered);
            }
            else {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_low0, psb_params.grid_delay_low1, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_low0, psb_params.grid_width_low1, psb_data.trigger_filtered);
            }
        }
        else if (p == 2) {
            if (psb_data.energy == HI) {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_high1, psb_params.grid_delay_high2, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_high1, psb_params.grid_width_high2, psb_data.trigger_filtered);
            }
            else {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_low1, psb_params.grid_delay_low2, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_low1, psb_params.grid_width_low2, psb_data.trigger_filtered);
            }
        }
        else if (p == 3) {
            if (psb_data.energy == HI) {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_high2, psb_params.grid_delay_high3, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_high2, psb_params.grid_width_high3, psb_data.trigger_filtered);
            }
            else {
                psb_data.grid_delay = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_delay_low2, psb_params.grid_delay_low3, psb_data.trigger_filtered);
                psb_data.grid_width = GetInterpolationValue(dos_intensities[p - 1], dos_intensities[p], psb_params.grid_width_low2, psb_params.grid_width_low3, psb_data.trigger_filtered);
            }
        }
    }

    for (p = 0; p < 6; p++)
    {
    	if (p == 0)
            temp = psb_data.grid_width;     //Grid Width
        else if (p == 1)
            temp = psb_data.grid_delay;     //Grid Delay
        else if (p == 2) {
            psb_data.rf_delay = psb_data.grid_delay - 9;
            temp = psb_data.rf_delay;       //RF PCB Delay
        }
        else if (p == 3)
            temp = psb_params.pfn_delay_high;   //PFN Delay
        else if (p == 4)
            temp = 0;                           //Dosimeter delay (not used)
        else if (p == 5)
            temp = psb_params.afc_delay_high;   //AFC Delay
        else
            temp = 0;

        for (q = 0; q < 8; q++)
        {
            PIN_SPI_CLK_OUT = 0;
            MACRO_NOP()

            bittemp = temp & 0x80;
            temp = temp << 1;

            if (bittemp == 0x80)
            {
                PIN_SPI_DATA_OUT = 1;
                MACRO_NOP()
            }
            else
            {
                PIN_SPI_DATA_OUT = 0;
                MACRO_NOP()
            }

            PIN_SPI_CLK_OUT = 1;
            MACRO_NOP()
        }

        if (p == 1)						//Latch Gun delay and width data into shift registers
        {
            PIN_LD_DELAY_GUN_OUT = 0;
            MACRO_NOP()
            PIN_LD_DELAY_GUN_OUT = 1;
            MACRO_NOP()
        }
        else if (p == 3)				//Latch PFN/RF delay data into shift registers
        {
            PIN_LD_DELAY_PFN_OUT = 0;
            MACRO_NOP()
            PIN_LD_DELAY_PFN_OUT = 1;
            MACRO_NOP()
        }
        else if (p == 5)				//Latch AFC/Dose delay data into shift registers
        {
            PIN_LD_DELAY_AFC_OUT = 0;
            MACRO_NOP()
            PIN_LD_DELAY_AFC_OUT = 1;
            MACRO_NOP()
        }
    }

    PIN_PW_CLR_CNT_OUT = !OLL_PW_CLR_CNT;			 // enable width count
    MACRO_NOP()
    if (PIN_TRIG_INPUT != ILL_TRIG_ON)
    {
    	PIN_PW_HOLD_LOWRESET_OUT = OLL_PW_HOLD_LOWRESET;   // clear reset only when trig pulse is low
        MACRO_NOP()
    	psb_faults.trigger_fault = 0;
    }
    else
    	psb_faults.trigger_fault = 1;
}

// calculate the interpolation value
unsigned int GetInterpolationValue(unsigned int low_point, unsigned int high_point, unsigned low_value, unsigned high_value, unsigned point)
{
   double dtemp, dslope;
   unsigned int ret = low_value;

   if (high_point > low_point)  // high point has to be bigger
   {
   	dslope = ((double)high_value - (double)low_value) / ((double)high_point - (double)low_point);
        dtemp = (double)point - (double)low_point;
        dtemp *= dslope;
        dtemp += low_value;
        ret = (unsigned)dtemp;
   }
   return (ret);
}

void SetStatusAndLEDs(void) {
    
    if (psb_faults.reset_faults) {
        psb_faults.can_comm_fault = 0;
        psb_faults.prf_fault = 0;
        psb_faults.reset_faults = 0;
    }

    if (psb_data.prf >= MAX_FREQUENCY) {
        psb_faults.prf_fault = 1;
        psb_data.enable_pulses = 0;
        psb_faults.inhibit_pulsing = 1;
    }

    if (PIN_XRAY_CMD_MISMATCH_IN == ILL_XRAY_CMD_MISMATCH)
        psb_faults.mismatch_fault = 1;
    else
        psb_faults.mismatch_fault = 0;

    if ((psb_data.system_state != psb_data.local_state) && (psb_data.system_state & XRAY_ON)) {
        psb_data.enable_pulses = 0;
        psb_faults.inhibit_pulsing = 1;
    }
    else
        psb_data.local_state = psb_data.system_state;

    if (psb_data.counter_config_received > 3)
        psb_data.state_machine = STATE_RUN;

    if ((psb_faults.inhibit_pulsing) || (psb_faults.prf_fault) || (psb_faults.trigger_fault) || (psb_faults.mismatch_fault)) {
        psb_data.local_state &= !XRAY_ON;
        psb_data.state_machine = STATE_FAULT;
    }

    if ((psb_faults.panel_fault) || (psb_faults.keylock_fault) || (psb_faults.can_comm_fault)) {
        psb_data.local_state = STANDBY;
        psb_data.state_machine = STATE_FAULT;
    }

    //This LED will be used for CAN status
    /*if (psb_data.local_state & WARMING_UP) {               //Warming up
        PIN_LED_WARMUP = OLL_LED_ON;
        PIN_CPU_WARMUP_OUT = OLL_CPU_WARMUP;
    }
    else {
        PIN_LED_WARMUP = !OLL_LED_ON;
        PIN_CPU_WARMUP_OUT = !OLL_CPU_WARMUP;
    }*/

    /*if (psb_data.local_state & STANDBY) {               //Warm (standby)
        PIN_LED_STANDBY = OLL_LED_ON;
        PIN_CPU_STANDBY_OUT = OLL_CPU_STANDBY;
    }
    else {
        PIN_LED_STANDBY = !OLL_LED_ON;
        PIN_CPU_STANDBY_OUT = !OLL_CPU_STANDBY;
    }*/

    if (psb_data.local_state & READY) {               //Ready
        PIN_LED_READY = OLL_LED_ON;
        PIN_CPU_READY_OUT = OLL_CPU_READY;
    }
    else {
        PIN_LED_READY = !OLL_LED_ON;
        PIN_CPU_READY_OUT = !OLL_CPU_READY;
    }

    if (psb_data.local_state & XRAY_ON) {               //Xrays
        PIN_LED_XRAY_ON = OLL_LED_ON;
    }
    else {
        PIN_LED_XRAY_ON = !OLL_LED_ON;
    }

    if (psb_data.local_state & SUM_FAULT) {               //Fault
        PIN_LED_SUMFLT = OLL_LED_ON;
        PIN_CPU_SUMFLT_OUT = OLL_CPU_SUMFLT;
        psb_data.state_machine = STATE_FAULT;
    }
    else {
        PIN_LED_SUMFLT = !OLL_LED_ON;
        PIN_CPU_SUMFLT_OUT = !OLL_CPU_SUMFLT;
    }
}

void __attribute__((interrupt, no_auto_psv)) _T4Interrupt(void)
{
    // This is a 100ms timer.  Used for CAN communication, PRF Calculation, and the heartbeat

    //Calculate PRF every second
    psb_data.prf_counter_100ms++;
    if (psb_data.prf_counter_100ms >= 10) {
        psb_data.prf = psb_data.pulse_counter;
        psb_data.pulse_counter = 0;
        psb_data.prf_counter_100ms = 0;
        psb_faults.prf_fault = 0;
    }

    //CAN Communication Timeout Fault
    psb_data.can_counter_100ms++;
    if ((psb_data.can_counter_100ms >= 2) && (psb_data.can_comm_ok)) {
        psb_data.can_counter_100ms = 0;
        psb_data.can_comm_ok = 0;
        psb_faults.can_comm_fault = 0;
    }
    else if ((psb_data.can_counter_100ms >= 2) && (!psb_data.can_comm_ok)) {
        psb_data.can_counter_100ms = 0;
        psb_faults.can_comm_fault = 1;
    }

    //Heartbeat the standby LED
    if (psb_data.heartbeat >= 5) {
        psb_data.heartbeat = 0;
        if (PIN_LED_STANDBY)
            PIN_LED_STANDBY = !OLL_LED_ON;
        else
            PIN_LED_STANDBY = OLL_LED_ON;
    }

   TMR4 = 0;
   _T4IF = 0;
}

void __attribute__((interrupt, no_auto_psv)) _T5Interrupt(void)
{
    // This is a 2.4ms timer.  Used to ensure PRF is not exceeded.

    psb_data.prf_ok_to_pulse = 1;

    //The interrupt flag is cleared when a trigger arrives
}