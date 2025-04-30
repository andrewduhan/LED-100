#include "Adafruit_TLC5947.h"
IntervalTimer lampRandTimer;
IntervalTimer readPotTimer;
IntervalTimer outputTimer;

#define NUM_TLC5947   3
#define data         12
#define clock        13
#define latch        14
#define oe           15  // Not used atm; seems to fuck everything up.
// 4-bit parallel address lines, shared to each of the 4 MC14514Bs
#define AD0 3
#define AD1 2
#define AD2 1
#define AD3 0
// A MC14514B's outputs are LOW when the PDx line (inhibit) is HIGH.   
#define PD0 9
#define PD1 10
#define PD2 11
#define PD3 12
#define STROBE 4 // strobe 0 = data latch, strobe 1 = transparent data input.
#define adjustPot    A9 // pin 25 aka digital 23
#define selectSwitch 11 // pin 13

Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5947, clock, data, latch);

bool lamps[64]; // Hold the state of all 60 lamp signals from the MPU, plus 4 bonus lamps
byte selectedAddress = 0b00000000; // keep track of the last strobed address.

// interval to push out LED data, in microseconds. 
// Too high and you may see flicker or "aliasing", too low and there may not be enough time between pushes
int LEDUpdateInterval = 5000; 

int maxBrightness = 4095;     // How bright should the LEDs be?  0 = 0%, 4095 = 100%

// Initialize these three vars - they will be updated at runtime if the pot is adjusted. 
int fadeTimeMs = 0;           // in milliseconds - adjusted by the pot.
int fadeStepCount = 1;        // steps between off and fully on - changes as fateTimeMs changes.
int fadeStepSize = 4095;      // step size (both brighter and darker) for each 

// we'll only change lamp states between address 0 and 15,
// then stay in a holding pattern until address 0 comes around again.
bool itsInputTime = false;

// The order of lamps in the received data is different from the order of the TLC5947 ouputs.
// This map is the key; each array position for the 72 TLC channels holds the corresponding position in the 60 (+4) lamp bools.
// Some lamps are duplicated here because some SCRs on the original board would drive two different outputs.
// (usually for things like SHOOT AGAIN that light on both playfield + backbox)
const int tlcChannelMap[72] = { 19, 18, 25, 23, 22, 21, 17, 16, 20, 24, 26, 37,  4,  5,  6,  2,  0,  1,  3,  8,  9, 10, 11,  7, 
                                27, 12, 10, 28, 13, 29, 14, 11, 26, 42, 57, 40, 25, 58, 43, 55, 41, 56, 44, 59, 60, 61, 62, 63, 
                                45, 52, 50, 51, 49, 53, 48, 46, 55, 56, 47, 34, 33, 54, 32, 39, 38, 40, 35, 41, 31, 30, 36, 15 };

// only init these temp vars once;
byte tempAddress = 0b00000000;
int tempPwm = 0;
int tempPotVal = 0;                                

void setup() {
  // on the LDA-100, all lines are pulled up to 5V
  pinMode(STROBE, INPUT_PULLUP);
  pinMode(AD0, INPUT_PULLUP);
  pinMode(AD1, INPUT_PULLUP);
  pinMode(AD2, INPUT_PULLUP);
  pinMode(AD3, INPUT_PULLUP);
  pinMode(PD0, INPUT_PULLUP);
  pinMode(PD1, INPUT_PULLUP);
  pinMode(PD2, INPUT_PULLUP);
  pinMode(PD3, INPUT_PULLUP);

  // The MPU walks through all 16 addresses (0 to 15) and pulses the strobe line LOW to latch the address into the chips.
  // So we need to keep track of the most recently strobed address for use if/when the PD lines are triggered
  attachInterrupt(STROBE, captureAddress, FALLING);

  // Then the LDx lines get pulled low to select which chips should turn ON for that address.
  attachInterrupt(PD0, setLamp0, FALLING);
  attachInterrupt(PD1, setLamp1, FALLING);
  attachInterrupt(PD2, setLamp2, FALLING);
  attachInterrupt(PD3, setLamp3, FALLING);

  tlc.begin();
  lampsOff();

  // lampRandTimer.begin(randAllLamps, 125000); // pretty lights for testing 

  readPotTimer.begin(readPot, 250000);
  outputTimer.begin(updateAllLEDs, LEDUpdateInterval);
}

void loop() {

}

void lampsOff(){
  for (int i = 0; i < 64; i++){
    lamps[i] = false;
  }
}

void setLamp0(){
  if (itsInputTime){
    lamps[selectedAddress] = true;
  }
}
void setLamp1(){
  if (itsInputTime){
    lamps[selectedAddress + 16] = true;
  }
}
void setLamp2(){
  if (itsInputTime){
    lamps[selectedAddress + 32] = true;
  }
}
void setLamp3(){
  if (itsInputTime){
    lamps[selectedAddress + 48] = true;
  }
}

void captureAddress(){
  // A Bit Of Theory about the signal from the MPU: 
  // About 120 times per second, there is a 'dip' in 5V power rails that feeds the lamps. This causes all SCRs on the LDA-100 
  // to shut off. Immediately after this, the MPU signals the LDA-100 to turn on any lamps that should be on. 
  // Thus, the lamp signals are only "turn these on".
  //
  // Data for the 60 lamps arrives in 15 batches of 4. First, the 4-bit address 0000 is strobed into the decoders. Then, 0-4 of 
  // the PDx lines are toggled low, triggering the SCR for any of lamps 0, 16, 32, or 48 that should be on.
  // Next, the 4-bit address 0001 is stobed in, and the process repeats through 1110.
  // 1111 (or 15) is special - no lamps are attached to 1111, so this is a no-op.
  //
  // Once all 16 addresses (0-15) have been sent to the LDA-100, the Strobe may go low a few times before the next full address cycle,
  // but the address remains 1111, so these can be ignored.

  // When STROBE falls, we should read a new address and store it as selectedAddress for use in subsequent PDx changes.
  tempAddress = 0b00000000;
  bitWrite(tempAddress, 0, digitalRead(AD0));
  bitWrite(tempAddress, 1, digitalRead(AD1));
  bitWrite(tempAddress, 2, digitalRead(AD2));
  bitWrite(tempAddress, 3, digitalRead(AD3));

  if (selectedAddress != tempAddress){
    selectedAddress = tempAddress;
    if (selectedAddress == 0){
      // We are at the start of new lamp signal data, so we momentarily blank our lamps.
      // Note to self: It might be nice to hold the new data in a shadow array and swap or update the primary array(?)
      lampsOff();
      itsInputTime = true;
    }
    if (selectedAddress == 15) {
      // Address 15 gets set at the end of the main sequence, and also several times outside of it.
      // No lamps are attached to any pin 15, so it's used as a "rest" signal here - we dont' want to update lamp values until we get 0000.
      itsInputTime = false;
    }
  }
}

void updateAllLEDs(){
  for (int i = 0; i < NUM_TLC5947 * 24; i++){
    updateLED(i);
  }
  tlc.write();
}

void updateLED(int channel){
  // check the lamps array - should this light be on?
  bool lampIsOn = lamps[tlcChannelMap[channel]];
  // fade the lamp toward on-ness or off-ness
  if (lampIsOn){
    tempPwm = tlc.getPWM(channel) + fadeStepSize;
  } else {
    tempPwm = tlc.getPWM(channel) - fadeStepSize;
  }
  tlc.setPWM(channel, constrain(tempPwm, 0, maxBrightness));
}

void readPot(){
  // This routine reads the pot setting and calculates new stepsize. 
  // Should be called fairly infrequently (4hz maybe?)
  tempPotVal = constrain(analogRead(adjustPot), 0, 1000);
  if (tempPotVal == fadeTimeMs){
    return;
  }
  fadeTimeMs = tempPotVal; 
  fadeStepCount = (fadeTimeMs * 10000) / LEDUpdateInterval; // number of brightness steps between off and on
  if (fadeStepCount <= 0){
    fadeStepCount = 1;
  } 
  fadeStepSize = (maxBrightness * 10) / fadeStepCount; // size of each brightness step.
}

void randAllLamps(){
  for (int i = 0; i < 60; i++){
    lamps[i] = random(4) == 1;
  }
}