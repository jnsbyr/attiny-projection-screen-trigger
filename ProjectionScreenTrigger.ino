/*
 * Projection Screen Trigger
 *
 * file: ProjectionScreenTrigger.ino
 *
 * encoding: UTF-8
 *
 * Copyright (C) 2020 Jens B.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 * CHANGES:
 *
 * 1.0.0.0 - 05.12.2020
 *   initial release
 *
 * 1.0.1.0 - 06.12.2020
 *   power saving while monitoring analog inputs
 *
 * 1.0.2.0 - 06.12.2020
 *   option to use positive logic outputs instead of open collector outputs
 *
 * 1.0.3.0 - 29.12.2020
 *   support extended startup delay with power saving
 *
 * 1.0.4.0 - 07.01.2021
 *   repurpose pin 5 to report the trigger state
 * 
 */

#include <avr/power.h>
#include <avr/sleep.h>
#include <avr/wdt.h>
#include <limits.h>


#define VERSION "1.0.4.0"     // 07.01.2021


/** HARDWARE CONFIGURATION ATtiny 85 **/

// analog inputs
#define PIN_VREF      A1      // Pin 7, IN, A1 (PIN_PB2)
#define PIN_VSENSE    A3      // Pin 2, IN, A3 (PIN_PB3)

// digital outputs
#define PIN_UP        PIN_PB1 // Pin 6 OUT, OC1A
#define PIN_DOWN      PIN_PB4 // Pin 3 OUT, OC1B (A2)
#define PIN_OUT       PIN_PB0 // Pin 5 OUT, (A0)
//#define PIN_DEBUG_IN  PIN_PB5 // Pin 1 IN, uncomment for serial debugging via PIN_OUT

#define UP_DOWN_OC            // comment out for UP and DOWN outputs using positive logic, uncomment for open collector outputs
#define OUT_STATE             // comment out for STATE output reporting main loop frequency instead of trigger state

// circuit dependendend conversion factor: (ADC Vref * transformer ratio * AC voltage) / (burden resistance * ADC bits)
static const float WATTS_PER_ADC_BIT = (2.56f * 500 * 230)/(100UL * 1024);


/** DEBUGGING **/

#ifdef PIN_DEBUG_IN
  #include <SoftwareSerial.h>
  SoftwareSerial debugSerial(PIN_DEBUG_IN, PIN_OUT);
#else
  #ifndef OUT_STATE  
  // state of run output pin (it is faster than reading back the current pin state)
  bool alive = false;
  #endif
#endif


/**
 * FUNCTIONAL:
 *
 * The ATtiny monitors 2 analog inputs pins. One is the sense analog input of
 * the current sensor and the other is the reference analog input for the trigger
 * level. If the current is higher than the trigger level for more than the delay
 * duration the "UP" output pin is activated for the pulse duration. The "DOWN"
 * output pin is handled the same way if the current is lower than the trigger
 * level.
 *
 * Delays and pulses are created using timer interrupts (non-blocking wait).
 *
 * Input monitoring continues in delay phase. Pulse is cancelled if input is
 * unstable.
 *
 * As long as the analog inputs are monitored the CPU is powered down
 * most of the time. In the active phase when the pulse is generated the
 * idle mode is used to reduce power consumption.
 *
 * Possible modifications: If power consumption must be reduced even further
 * or longer timing periods are required one could use a WDT interrupt counter
 * instead of timer1.
 *
 *
 * BUILD ENVIRONMENT:
 *
 * IDE: Arduino 1.8.13
 *
 * SDK: ATtiny Core 1.4.1 LGPL 2.1 https://github.com/SpenceKonde/ATTinyCore
 *
 * Chip:            ATtiny 85
 * Clock:           1 MHz internal (hardware default)
 * B.O.D. Level:    disabled (hardware default, HFUSE 0-2 = 7)
 * Preserve EEPROM: disabled (hardware default, HFUSE 3 = 1)
 * Timer 1 Clock:   CPU (default)
 * LTO:             enabled (default)
 * millis/micros:   disabled
 *
 *
 * ELECTRICAL:
 *
 * Input: 230 V AC, Imax,eff = 16 A, Pmax,eff = 3680 W
 *
 * Current Transformer: Talema AZ-0500, 500:1
 * Rated Burden Resistance:    33 Ohms
 * Selected Burden Resistance: 100 Ohms
 * Imax = Imax,eff * 2^1/2 = 23 A
 * Imax,sec = Imax / 500 = 0.045 A
 * Vmax,sec = Imax,s * 100 Ohms = 4.5 V
 * Pmax,sec = 0.2 W
 *
 * ADC Vref = 2.56 V
 * ADC resolution: 1024
 * Imax,adc = 500 * 2.56 V / 100 Ohms = 12.8 A
 * Pmax,adc = Imax * 230 V = 2940 W
 * Pmin,adc = Pmax / 1024 = 2.9 W
 *
 * Using a current transformer as sensor allows mechanically non invasive
 * and electrically isolated monitoring of the projector current.
 *
 * The range for the trigger level is determined by the value of the burden
 * resistor. Choosing a higher resistance limits the trigger range but improves
 * the sensitivity and stability for smaller loads. A resistor of 100 Ohms
 * results in a resolution of 3 W and the minimum load that can be detected
 * reliably should be around 30 W. Resistor values below the rated burden of
 * 33 Ohms might damage the current transformer at high input currents.
 *
 *
 * !!! WARNING, PLEASE READ CAREFULLY !!!
 *
 * THIS CIRCUIT IS FOR EXPERIMENTAL PURPOSES ONLY.
 *
 * THIS CIRCUIT MAY CAUSE
 *
 * - ELECTRICAL SHOCK OR DEATH IF ASSEMBLED OR INSTALLED IMPROPERLY AND MAY
 *   THEREFORE ONLY BE ASSEMBLED AND INSTALLED BY A PROFESSIONAL ELECTRICAN
 *
 * - DAMAGE TO PROPERTY AND LIFE BY UNOBSERVERED OPERATION OF THE PROJECTOR
 *   SCREEN
 *
 */
class ProjectionScreenTrigger
{
public:
  enum TriggerState { STARTUP, MONITORING, CHANGED, PULSE };
  enum ScreenState  { UNKNOWN, UP, DOWN };

public:
  /**
   * configure the Projector Screen Trigger
   *
   * @param hysteresis [W] hysteresis between "on" an "off" power level, 1 .. 100 (.. 223 W)
   * @param startupDelay [ms] delay between ATtiny start and start of input evaluation, 64 .. 65472 ms
   * @param pulseDelay [ms] delay between detection and action, higher values improve stability, 17 .. 4177 ms
   * @param pulseDuration [ms] duration of output pulse, 17 .. 4177 ms
   */
  void setup(unsigned short hysteresis, unsigned short startupDelay, unsigned short pulseDelay, unsigned short pulseDuration)
  {
    this->hysteresis = ceil(((float)hysteresis)/(WATTS_PER_ADC_BIT*2));

    // configure digital pins
#ifndef UP_DOWN_OC
    pinMode(PIN_UP, OUTPUT);
    digitalWrite(PIN_UP, LOW);
    pinMode(PIN_DOWN, OUTPUT);
    digitalWrite(PIN_DOWN, LOW);
#else
    pinMode(PIN_UP, INPUT);
    pinMode(PIN_DOWN, INPUT);
#endif

#ifndef PIN_DEBUG_IN
    pinMode(PIN_OUT, OUTPUT);
    digitalWrite(PIN_OUT, LOW);
#endif

    // configure analog pins
    analogReference(INTERNAL2V56 - 1); // DEFAULT Vref = Vcc = 5 V (see wiring.h)

    // power down USI
    power_usi_disable();

    // configure timer
    setupTimer(startupDelay, pulseDelay, pulseDuration);
    interrupts();

    // preset analog inputs
    vRef = analogRead(PIN_VREF);
    vSense = analogRead(PIN_VSENSE);

    // start startup timer
    startTimer(this->startupDelay);
  }

  /**
   * sample analog inputs and check trigger level
   *
   * Notes:
   * - executes at 8 Hz consuming 0.08 mA @ 4.8V while monitoring analog inputs
   *   and 0.6 mA @ 4.8V when pulsing reducing average power consumption
   *   below 40 µW
   * - would be executes at 660 Hz without explicit delay constantly consuming
   *   1.6 mA @ 4.8V or 8 mW without using sleep modes
   *
   */
  void check()
  {
    // power management
    powerSave((triggerState == STARTUP || triggerState == MONITORING || triggerState == CHANGED)? WDTO_60MS : 0, !(triggerState == STARTUP || triggerState == MONITORING));

    // trigger state management
    switch (triggerState)
    {
      case MONITORING:
      case CHANGED:
      {
        // read analog inputs (discard 1st sample to increase accuracy after changing ADC mux)
        analogRead(PIN_VREF);
        vRef = (3*vRef + (unsigned short)analogRead(PIN_VREF))/4;
        analogRead(PIN_VSENSE);
        vSense = (vSense + (unsigned short)analogRead(PIN_VSENSE))/2;

        // prevent unsigned underflow
        if (vRef < hysteresis)
        {
          vRef = hysteresis;
        }

#ifdef PIN_DEBUG_IN
        debugSerial.print("R=");
        debugSerial.print(vRef);
        debugSerial.print(" V=");
        debugSerial.print(vSense);
#endif

        // compare analog inputs
        bool higher = vSense > (vRef + hysteresis);
        bool lower = vSense < (vRef - hysteresis);

        // check state
        noInterrupts();
        switch (triggerState)
        {
          case MONITORING:
#ifdef PIN_DEBUG_IN
            debugSerial.print(" M");
#endif
            if ((screenState != DOWN && higher) || (screenState != UP && lower))
            {
              // changed, define start delay timer
              if (screenState == UNKNOWN)
              {
                // preset initial screen state at power up
                screenState = higher? UP : DOWN;
              }
#ifdef PIN_DEBUG_IN
              debugSerial.print(screenState == UP? " U" : " D");
              if (higher) debugSerial.print("H");
              else if (lower) debugSerial.print("L");
              else  debugSerial.print("-");
#endif
              triggerState = CHANGED;
              startTimer(pulseDelay);
            }
            break;

          case CHANGED:
            if ((screenState == UP && !higher) || (screenState == DOWN && !lower))
            {
#ifdef PIN_DEBUG_IN
              debugSerial.print(" C");
#endif
              // unstable, cancel delay timer
              triggerState = MONITORING;
              stopTimer();
            }
            break;
        }

        interrupts();

#ifdef PIN_DEBUG_IN
        debugSerial.println();
#endif
        break;
      }
    }
  }

  /**
   * timer event handler for:
   * <ul>
   *   <li>pulse delay expired</li>
   *   <li>pulse duration expired</li>
   * </ul>
   */
  void timerEvent()
  {
    stopTimer();

    switch (triggerState)
    {
      case CHANGED:
        // start of pulse, start pulse duration timer
        screenState = screenState == UP? DOWN : UP;
#ifndef UP_DOWN_OC
        if (screenState == UP)
        {
          digitalWrite(PIN_DOWN, LOW);
          digitalWrite(PIN_UP, HIGH);
        }
        else
        {
          digitalWrite(PIN_UP, LOW);
          digitalWrite(PIN_DOWN, HIGH);
        }
#else
        if (screenState == UP)
        {
          pinMode(PIN_DOWN, INPUT);
          digitalWrite(PIN_UP, LOW);
          pinMode(PIN_UP, OUTPUT);
        }
        else
        {
          pinMode(PIN_UP, INPUT);
          digitalWrite(PIN_DOWN, LOW);
          pinMode(PIN_DOWN, OUTPUT);
        }
#endif
        triggerState = PULSE;
#ifdef PIN_DEBUG_IN
        debugSerial.println(" P");
#else
  #if defined(OUT_STATE)
        digitalWrite(PIN_OUT, screenState == UP? LOW : HIGH);
  #endif        
#endif
        startTimer(pulseDuration);
        break;

      case PULSE:
        // end of pulse
#ifndef UP_DOWN_OC
        digitalWrite(PIN_UP, LOW);
        digitalWrite(PIN_DOWN, LOW);
#else
        pinMode(PIN_UP, INPUT);
        pinMode(PIN_DOWN, INPUT);
#endif
        triggerState = MONITORING;
        break;
    }
  }

  /**
   * watchdog event handler for:
   * <ul>
   *   <li>startup delay</li>
   * </ul>
   */
  void watchdogEvent()
  {
    switch (triggerState)
    {
      case STARTUP:
        watchdogTicks++;
        if (watchdogTicks >= startupDelay)
        {
          // end of startup delay
          triggerState = MONITORING;
        }
        break;
    }
  }

  TriggerState getTriggerState()
  {
    return triggerState;
  }

private:
  /**
   * clip input value to the range 1 .. 255
   * for timer compare
   */
  static byte clipByte(unsigned long i)
  {
    if (i <= 2U)
    {
      return 1U;
    }
    else if (i >= 256U)
    {
      return 255U;
    }
    else
    {
      return i--;
    }
  }

  /**
   * clip input value to the range 1 .. USHORT_MAX
   * for watchdog tick compare
   */
  static unsigned short clipUShort(float f)
  {
    unsigned short u;
    if (f < 1)
    {
      u = 1U;
    }
    else if (f-0.5f > USHRT_MAX)
    {
      u = USHRT_MAX;
    }
    else
    {
      u = round(f);
    }
    return u;
  }

  /**
   * @param startupDelay [ms] delay between ATtiny start and start of input evaluation
   * @param pulseDelay [ms] delay between detection and action, higher values improve stability
   * @param pulseDuration [ms] duration of output pulse
   */
  void setupTimer(unsigned short startupDelay, unsigned short pulseDelay, unsigned short pulseDuration)
  {
    // power down timer0
    power_timer0_disable();

    // configure timer1 to 61 Hz with output compare match A interrupt -> 0 .. 4.2 s (16.4 ms)
    TCCR1 = 0;                                              // clear timer1
    TCCR1 |= (1<<CS13) | (1<<CS12) | (1<<CS11) | (1<<CS10); // set 1 MHz clock prescaler to 16384
    TIMSK &= ~((1<<OCIE1A) | (1<<OCIE1B) | (1<<TOIE1));     // disable timer1 interrupts

    this->pulseDelay = clipByte(1000UL*pulseDelay/16384U);
    this->pulseDuration = clipByte(1000UL*pulseDuration/16384U);

    this->startupDelay = clipUShort(startupDelay/64.0f);    // 64 ms watchodg period
  }

  static void startTimer(byte ticks)
  {
    TCNT1 = 0;
    OCR1A = ticks;
    TIFR |= (1<<OCF1A);   // clear pending output compare match A interrupt
    TIMSK |= (1<<OCIE1A); // enable output compare match A interrupt
  }

  static void stopTimer()
  {
    TIMSK &= ~(1<<OCIE1A); // disable output compare match A interrupt
  }

  /**
   * power down ADC and timer1 units and put CPU to sleep until
   * wakeup by WDT or timer1 occurs
   *
   * @param watchdogTimeout if not zero use WDT wakeup, use constants from wdt.h excluding WDTO_15MS, e.g. WDTO_60MS
   * @param timer1Wakeup keep timer1 running
   *
   * Note: if called with the parameter combination (0, false) or (0, true) and timer 1 disabled a reset is required for wakeup
   *
   */
  static void powerSave(byte watchdogTimeout, bool timer1Wakeup)
  {
    // shutdown
    byte adcsra = ADCSRA;                    // save ADC state
    ADCSRA = 0;                              // disable ADC
    if (timer1Wakeup)
    {
      set_sleep_mode(SLEEP_MODE_IDLE);
      power_adc_disable();
    }
    else
    {
      set_sleep_mode(SLEEP_MODE_PWR_DOWN);
      power_all_disable();
    }
    noInterrupts();
    if (watchdogTimeout > WDTO_15MS)
    {
      MCUSR &= ~(1 << WDRF);                           // clear watchdog reset flag
      WDTCR = (1<<WDIE) | (1<<WDCE) | watchdogTimeout; // enable watchdog interrupt after 64 ms
    }
    sleep_enable();
    interrupts();

    // put CPU to sleep wait for WDT wakeup
    sleep_cpu();

    // startup
    sleep_disable();
    if (watchdogTimeout > WDTO_15MS)
    {
      wdt_disable();
    }
    power_adc_enable();
    power_timer1_enable();
    ADCSRA = adcsra;                         // restore ADC state
  }

private:
  volatile TriggerState triggerState = STARTUP;
  volatile ScreenState screenState = UNKNOWN;

  byte hysteresis = 0;    // [bits]
  byte pulseDelay = 0;    // [timer ticks]
  byte pulseDuration = 0; // [timer ticks]
  unsigned short startupDelay = 0; // [watchdog ticks]

  unsigned short watchdogTicks = 0;

  unsigned short vRef = 0;   // [bits]
  unsigned short vSense = 0; // [bits]

} projectionScreenTrigger;


/**
 *  Arduino setup function
 */
void setup()
{
#ifdef PIN_DEBUG_IN
  debugSerial.begin(9600);
  debugSerial.println("Projection Screen Trigger ...");
#endif

  // configure projector screen trigger: hysteresis [W], startup delay [ms], input filter [ms], pulse duration [ms]
  projectionScreenTrigger.setup(34, 10000, 2000, 500);
}

/**
 * Arduino loop function
 */
void loop()
{
  projectionScreenTrigger.check();

#if !defined(PIN_DEBUG_IN) && !defined(OUT_STATE)
  // toggle output pin to report loop frequency
  if (projectionScreenTrigger.getTriggerState() != ProjectionScreenTrigger::STARTUP)
  {
    alive = !alive;
    digitalWrite(PIN_OUT, alive? HIGH : LOW);
  }
#endif
}

/**
 * ATtiny X5 timer1 interrupt service routine for compare match of register A
 *
 * Note: All variables that are written during execution of ISR must be of type "volatile"!
 */
ISR(TIMER1_COMPA_vect)
{
  projectionScreenTrigger.timerEvent();
}

/**
 * ATtiny interrupt service routine for watchdog
 */
ISR(WDT_vect)
{
  projectionScreenTrigger.watchdogEvent();
}
