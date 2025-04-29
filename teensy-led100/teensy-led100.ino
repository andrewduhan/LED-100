IntervalTimer lampRandTimer;
IntervalTimer outputTimer;

#include "Adafruit_TLC5947.h"

#define NUM_TLC5947   3
#define data         12
#define clock        13
#define latch        14
#define oe           -1  // set to -1 to not use the enable pin (its optional)

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

// strobe 0 = data latch, strobe 1 = transparent data input.
#define STROBE 4

#define adjustPot     9
#define selectSwitch 11

Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5947, clock, data, latch);

// keep track of the last strobed address.
byte selectedAddress = 0b00000000;
byte tempAddress = 0b00000000;
int tmpPwm;

int fadeTimeMs = 150; // in milliseconds - to be set later by adjustPot
int LEDUpdateInterval = 10000; // frequency that we push out LED data, in microseconds
int maxBrightness = 150; // 0 to 4095
int fadeStepCount = (fadeTimeMs * 1000) / LEDUpdateInterval;
int fadeStep = maxBrightness / fadeStepCount;

// Hold the state of all 60 lamp signals from the MPU, plus the 4 bonus lamps
bool lamps[64];

// we'll only change lamp states between address 0 and 15 - then stay in a holding pattern until address 0 comes around again.
bool itsInputTime = false;

// The order of lamps in the received data is different from the order of the TLC5947 ouputs.
// This map holds the keys; each the array position for each of the 72 TLC channels holds the corresponding position in the 
// 64 lamp bools.
const int tlcChannelMap[72] = { 19, 18, 25, 23, 22, 21, 17, 16, 20, 24, 26, 37,  4,  5,  6,  2,  0,  1,  3,  8,  9, 10, 11,  7, 
                                27, 12, 10, 28, 13, 29, 14, 11, 26, 42, 57, 40, 25, 58, 43, 55, 41, 56, 44, 59, 60, 61, 62, 63, 
                                45, 52, 50, 51, 49, 53, 48, 46, 55, 56, 47, 34, 33, 54, 32, 39, 38, 40, 35, 41, 31, 30, 36, 15 };

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
  pinMode(adjustPot, INPUT);

  lampsOff();
  lampRandTimer.begin(randAllLamps, 250000);
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

void randAllLamps(){
  for (int i = 0; i < 60; i++){
    lamps[i] = random(2) == 1;
  }
}

void captureAddress(){
  tempAddress = 0b00000000;
  bitWrite(tempAddress, 0, digitalRead(AD0));
  bitWrite(tempAddress, 1, digitalRead(AD1));
  bitWrite(tempAddress, 2, digitalRead(AD2));
  bitWrite(tempAddress, 3, digitalRead(AD3));
  if (selectedAddress != tempAddress){
    selectedAddress = tempAddress;
    if (selectedAddress < 15){
      // only see a non-15 address when we're in the input stage
      itsInputTime = true;
    }
    if (selectedAddress == 0){
      // The SCRs on an LDA-100 would hold a lamp lit until the next zero crossing interrupt, roughly at 120hz (twice per cycle), 
      // which would cause all SCRs to shut off until the next address cycle.
      // We'll treat address 0000 as the mark between two passes.
      lampsOff();
    }
    if (selectedAddress == 15) {
      // Address 15 gets set several times outside of the lamp codes.
      // No lamps are attached to any pin 15, so it's used as a "rest" 
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
    tmpPwm = tlc.getPWM(channel) + fadeStep;
  } else {
    tmpPwm = tlc.getPWM(channel) - fadeStep;
  }
  tlc.setPWM(channel, constrain(tmpPwm, 0, maxBrightness));
}