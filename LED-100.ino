#include <TimerOne.h>
#include "Adafruit_TLC5947.h"

// How many TLC5947s do you have chained?
#define NUM_TLC5947 3
#define data  18
#define clock 19
#define latch 20
#define oe    -1
Adafruit_TLC5947 tlc = Adafruit_TLC5947(NUM_TLC5947, clock, data, latch);

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

// FADEIN/OUT step size will be set by reading two trim pots in a later rev
#define FADEIN_STEP_SIZE = 100;
#define FADEOUT_STEP_SIZE = 100;

// Hold the state of all 60 lamp signals from the MPU
uint64_t lamps = 0;

// The order of lamps in the received data is different from the order that LED brightness data should be sent to the TLC5947s.
// This map holds the keys; each the array position for each TLC channel holds the corresponding position in the 64 lamps bits.
const int tlcChannelMap[72] = { 19, 18, 25, 23, 22, 21, 17, 16, 20, 24, 26, 37,  4,  5,  6,  2,  0,  1,  3,  8,  9, 10, 11,  7, 
                                27, 12, 10, 28, 13, 29, 14, 11, 26, 42, 57, 40, 25, 58, 43, 55, 41, 56, 44, 59, 60, 61, 62, 63, 
                                45, 52, 50, 51, 49, 53, 48, 46, 55, 56, 47, 34, 33, 54, 32, 39, 38, 40, 35, 41, 31, 30, 36, 15 };

// keep track of the last strobed address.
byte selectedAddress = 0b00000000;
byte tempAddress = 0b00000000;

// we'll only change lamp states between address 0 and 15 - then stay in a holding pattern until address 0 comes around again.
bool itsInputTime = false;
bool itsOutputTime = false;

// Each LED gets a 12-bit brightness value;
// At some regular interval, lamps that are set ON by the inputs have their brightness value increased to a max of 4098
// and lamps that are OFF decrease to a min of zero.

void setup() {
  // pinMode(LED_BUILTIN, OUTPUT);
  // digitalWrite(LED_BUILTIN, HIGH);
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

  if (oe >= 0) {
    pinMode(oe, OUTPUT);
    digitalWrite(oe, LOW);
  }
  tlc.begin();
  
  Timer1.initialize(8000);
  Timer1.attachInterrupt(setOutputTime);
}

void loop() {
  if (!itsInputTime){
    updateAllLEDs();
    itsOutputTime = false;
    delay(100);
  }
}

void setLamp0(){
  if (itsInputTime){
    bitSet(lamps, selectedAddress);
  }
}
void setLamp1(){
  if (itsInputTime){
    bitSet(lamps, selectedAddress + 16);
  }
}
void setLamp2(){
  if (itsInputTime){
    bitSet(lamps, selectedAddress + 32);
  }
}
void setLamp3(){
  if (itsInputTime){
    bitSet(lamps, selectedAddress + 48);
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
      lamps = 0;
    }
    if (selectedAddress == 15) {
      // Address 15 gets set several times outside of the lamp codes.
      // No lamps are attached to any pin 15, so it's used as a "rest" 
      itsInputTime = false;
    }
    
  }
}

void setOutputTime(){
  itsOutputTime = true;
}

void updateAllLEDs(){
  for (int i = 0; i < 72; i++){
    updateLED(i);
  }
  tlc.write();
}

void updateLED(int channel){
  bool lampIsOn = bitRead(lamps, tlcChannelMap[channel]);
  if (lampIsOn){
    brightenLED(channel);
  } else {
    dimmenLED(channel);
  }
}

void brightenLED(int channel){
  // int newPwm = tlc.getPWM(channel) + FADEIN_STEP_SIZE;
  // if (newPwm > 4096) {
  //   newPwm = 4096;
  // } 
  // tlc.setPWM(channel, newPwm);
  tlc.setPWM(channel, 4096);
}

void dimmenLED(int channel){
  // int newPwm = tlc.getPWM(channel) - FADEOUT_STEP_SIZE;
  // if (newPwm < 0) {
  //   newPwm = 0;
  // } 
  // tlc.setPWM(channel, newPwm);
  tlc.setPWM(channel, 0);
}

// void updateLEDs(){
//   printInt(lamps3, 16);
//   printInt(lamps2, 16);
//   printInt(lamps1, 16);
//   printInt(lamps0, 16);
//   Serial.println();
// }

// void printInt(int thingy, int n){
// // prints an n-bit int in binary style, with zero-padding.
//   for(int i = n - 1; i >= 0; i--){
//     Serial.print(bitRead(thingy,i));  
//   }
// }