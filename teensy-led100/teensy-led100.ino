#include "Adafruit_TLC5947.h"
IntervalTimer lampRandTimer;
// IntervalTimer readPotTimer;
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
int PDs[4] = {9, 10, 11, 12};
// #define PD0 9
// #define PD1 10
// #define PD2 11
// #define PD3 12
#define STROBE 4 // strobe 0 = data latch, strobe 1 = transparent data input.
#define adjustPot    A9 // pin 25 aka digital 23
#define selectSwitch 11 // pin 13

Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5947, clock, data, latch);

bool lamps[64]; // Hold the state of all 60 lamp signals from the MPU, plus 4 bonus lamps
byte selectedAddress = 0b00000000; // keep track of the last strobed address.

// interval to push out LED data, in microseconds. 
// Too high and you may see individual brightness steps, too low and there may not be enough time between pushes to do the needful.
// 10000us = 100hz
int LEDUpdateInterval = 10000; 

int maxBrightness = 4095;     // How bright can the LEDs get?  0 = 0%, 4095 = 100%

// Initialize these three vars - they will be updated at runtime if the pot is adjusted. 
int fadeTimeMs = 0;           // in milliseconds - adjusted by the pot.
int fadeStepCount = 1;        // steps between off and fully on - updated when fateTimeMs changes.
int fadeStepSize = 4095;      // step size (both brighter and darker) for LED output smoothing - updated when fadeStepCount changes. 

// int fadeStepSize = 4095;

int lampPos = 0;

// Lamp data can come in at any time while STROBE is low, and should be ignored when STROBE is high.
bool itsCaptureTime = false;

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
// bool switchPressed = false;                        

void setup() {
  // on the LDA-100, all lines are pulled up to 5V
  pinMode(STROBE, INPUT_PULLUP);
  pinMode(AD0, INPUT_PULLUP);
  pinMode(AD1, INPUT_PULLUP);
  pinMode(AD2, INPUT_PULLUP);
  pinMode(AD3, INPUT_PULLUP);
  pinMode(PDs[0], INPUT_PULLUP);
  pinMode(PDs[1], INPUT_PULLUP);
  pinMode(PDs[2], INPUT_PULLUP);
  pinMode(PDs[3], INPUT_PULLUP);
  pinMode(selectSwitch, INPUT_PULLUP);

  setAllLamps(false);
  tlc.begin();

  // lampRandTimer.begin(randAllLamps, 125000); // pretty lights for testing 
  // lampRandTimer.begin(cycleLamps, 125000); // pretty lights for testing 
  // lampRandTimer.begin(flashAllLamps, 1000000); // pretty lights for testing 

  // The MPU walks through all 16 addresses (0 to 15) and pulses the strobe line LOW to latch the address into the chips.
  // So we need to keep track of the most recently strobed address for use if/when the PD lines are triggered
  attachInterrupt(STROBE, setCapturetimeTrue, FALLING);
  attachInterrupt(STROBE, setCaptureTimeFalse, RISING);

  // readPotTimer.begin(readPot, 250000);
  outputTimer.begin(updateAllLEDs, LEDUpdateInterval);
}

void loop() {
  captureAddress();
  setLampValues();
}

void setCaptureTimeFalse(){
  itsCaptureTime = false;
}

void setCapturetimeTrue(){
  itsCaptureTime = true;
}

void setLampValues() {
  if(itsCaptureTime){
    // For each of the lamps associated with selectedAddress, turn them on if their PD line is LOW.
    // There is no "off" signal; all lamps are re-blanked once per ~120hz cycle.
    for (int i = 0; i < 4; i++){
      if ( digitalRead(PDs[i]) == LOW ){
        lamps[i + (i + 16)] = true;
      }
    }
  }
}

void setAllLamps(bool v){
  for (int i = 0; i < 64; i++){
    lamps[i] = v;
  }
}

// void setLamp0(){
//   if (itsInputTime){
//     lamps[selectedAddress] = true;
//   }
// }
// void setLamp1(){
//   if (itsInputTime){
//     lamps[selectedAddress + 16] = true;
//   }
// }
// void setLamp2(){
//   if (itsInputTime){
//     lamps[selectedAddress + 32] = true;
//   }
// }
// void setLamp3(){
//   if (itsInputTime){
//     lamps[selectedAddress + 48] = true;
//   }
// }

void captureAddress(){
  // A Bit Of Theory about the signal from the MPU: 
  // About 120 times per second, there is a 'dip' in 5V power rails that feeds the lamps. This causes all SCRs on the LDA-100 
  // to shut off. Immediately after this, the MPU begins signalling the LDA-100 to turn on any lamps that should be on, in batches. 
  // Thus, the lamp signals are only "turn these on".
  //
  // Data for the 60 lamps arrives in 15 batches of 4. First, the 4-bit address 0000 (0) is strobed into the decoders. Then, 0-4 of 
  // the PDx lines are toggled low, triggering the SCR for any of lamps 0, 16, 32, or 48 that should be on.
  // Next, the 4-bit address 0001 (1) is stobed in, and the process repeats through 1111 (15).
  // 1111 is special - no SCRs/lamps are attached to 1111, so this is a no-op.
  //
  // Once all 16 addresses (0-15) have been sent to the LDA-100, the Strobe may go low a few times before the next full address cycle,
  // but the address remains 1111, so these can be ignored.

  // When STROBE falls, we should read a the address from the ADx pins and store it as selectedAddress for use in subsequent PDx changes.
  
  // 
  tempAddress = 0b00000000;
  bitWrite(tempAddress, 0, digitalRead(AD0));
  bitWrite(tempAddress, 1, digitalRead(AD1));
  bitWrite(tempAddress, 2, digitalRead(AD2));
  bitWrite(tempAddress, 3, digitalRead(AD3));

  if (selectedAddress != tempAddress){
    selectedAddress = tempAddress;
    Serial.println(selectedAddress);
    if (selectedAddress == 0){
      // We are at the start of new lamp signal data, so we momentarily blank our lamps, because the MPU never signals a lamp to turn off, 
      // only which should be on.
      // Note to self: It might be nice to hold the new data in a shadow array and swap or update the primary array(?)
      setAllLamps(false);
      itsInputTime = true;
    }
    if (selectedAddress == 15){
      // Address 15 gets set at the end of the main sequence, and also several times outside of it.
      // No lamps are attached to any pin 15, so it's used as a "rest" signal here - we don't want to update lamp values until we get a 0000 again.
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
  // Check the lamps array - should this light be on?
  // Then fade the lamp toward on-ness or off-ness
  if (lamps[tlcChannelMap[channel]]){
    tempPwm = tlc.getPWM(channel) + fadeStepSize;
  } else {
    tempPwm = tlc.getPWM(channel) - fadeStepSize;
  }
  tlc.setPWM(channel, constrain(tempPwm, 0, maxBrightness));
}

void readPot(){
  // This routine reads the pot setting and calculates new stepsize. 
  // Should be called fairly infrequently (4hz maybe?)
  // linearFadeStepSize = analogRead(adjustPot) * 4;

  tempPotVal = analogRead(adjustPot);
  tempPotVal = ((0.1 * tempPotVal) * (0.1 * tempPotVal)) / 10 ; // lower sensitvity for lower values - parabolic
  fadeTimeMs = tempPotVal; 
  fadeStepCount = constrain((fadeTimeMs * 10000) / LEDUpdateInterval, 1, 10000); // number of brightness steps between off and on
  fadeStepSize = (maxBrightness * 10) / fadeStepCount; // size of each brightness step.
}

// void switchPress(){
//   switchPressed = true;
// }

void randAllLamps(){
  for (int i = 0; i < 60; i++){
    lamps[i] = random(6) == 1;
  }
}

void cycleLamps(){
  setAllLamps(false);
  lamps[tlcChannelMap[lampPos + 1]] = true;
  lampPos += 1;
  if (lampPos == 72){
    lampPos = 0; 
  }
}

void flashAllLamps(){
  if (lamps[0]){
    setAllLamps(false);
  } else {
    setAllLamps(true);
  }
}