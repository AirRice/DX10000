#include <SPI.h>
#include <SD.h>
#include <Adafruit_VS1053.h>
#include <string.h>
#include <math.h>
#include "pinouts.h"

#define FILENAME "/test1.mid"
#define BUZZER_TRACK_COUNT 8
#define TOTAL_TRACK_COUNT 24
#define DRUM_CHANNEL 9 

#define CARDCS         14     // Card chip select pin

#define DEBUG_PRINT 1
#define USE_BUZZERS 1

#if defined(ESP8266) || defined(__AVR_ATmega328__) || defined(__AVR_ATmega328P__)
  #define VS1053_MIDI Serial
#else
  // anything else? use the hardware serial1 port
  #define VS1053_MIDI Serial1
#endif

// THE UTILITY FUNCTIONS BELOW COURTESY OF LEN SHUSTEK
/*----------------------------------------------------------------------------------------
* The MIT License (MIT)
* Copyright (c) 2011,2013,2015,2016,2019,2021 Len Shustek
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR
* IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*********************************************************************************************/

// MIDI file header formats

struct midi_header {
   int8_t MThd[4];
   uint32_t header_size;
   uint16_t format_type;
   uint16_t number_of_tracks;
   uint16_t time_division; };

struct track_header {
   int8_t MTrk[4];
   uint32_t track_size; };


struct midi_track {
  byte *trkptr;
  byte *trkend;
  byte last_event;
  unsigned long last_event_time;
  unsigned long track_time;
  unsigned long remaining_delay;
  byte eventType; //event type;
  byte note; // Midi note
  byte cur_playing_note;
  byte channel; // midi track this plays on
  byte volume;
  byte instrument; // MIDI instrument
  bool trk_ended;
} midi_all_tracks[TOTAL_TRACK_COUNT] = { 0 };

/* portable string length */
int strlength (const char *str) {
   int i;
   for (i = 0; str[i] != '\0'; ++i);
   return i; }
   
/* match a constant character sequence */
int charcmp (const char *buf, const char *match) {
   int len, i;
   len = strlength (match);
   for (i = 0; i < len; ++i)
      if (buf[i] != match[i]) return 0;
   return 1; }
   
// Big endian (midi format) -> Little endian (esp32) conversion
uint16_t rev_short (uint16_t val) {
   return ((val & 0xff) << 8) | ((val >> 8) & 0xff); }

uint32_t rev_long (uint32_t val) {
   return (((rev_short ((uint16_t) val) & 0xffff) << 16) |
           (rev_short ((uint16_t) (val >> 16)) & 0xffff)); }

// Gets the variable length value. Ranges from 1-4 bytes in length in total.
unsigned long get_varlen(byte **ptr)
{
  unsigned long output = 0;
  int i;
  for (i = 0; i<4; i++)
  {
    byte next_byte = *((*ptr)++);
    output = (output <<7) | (next_byte&0x7f);
    if (!(next_byte & 0x80))
    {
         return output; 
    }
  }
  return output; 
}

void midiSetChannelVolume(uint8_t chan, uint8_t vol) {
  if (chan > 15) return;
  if (vol > 127) return;
  
  VS1053_MIDI.write(0xB0 | chan);
  VS1053_MIDI.write(0x07);
  VS1053_MIDI.write(vol);
}

void setup() {
  Serial.begin(115200);
  // Wait for serial port to be opened, remove this line for 'standalone' operation
  while (!Serial) { delay(1); }
  delay(500);
  Serial.println("VS1053 MIDI test");
  Serial.println(F("VS1053 found"));
  if (!SD.begin(CARDCS)) {
    Serial.println(F("SD failed, or not present"));
    while (1);  // don't do anything more
  }
  Serial.println("SD OK!");
  pinMode(1, OUTPUT);
  digitalWrite(1, HIGH);
  VS1053_MIDI.begin(31250); // MIDI uses a 'strange baud rate'
  while (!VS1053_MIDI) { delay(1); }
  for (int i = 0; i < BUZZER_TRACK_COUNT; i++)
  {
    pinMode(output_pins[i], OUTPUT);
    ledcAttachPin(output_pins[i], i*2);
    ledcSetup(i*2, 10000, 8); // Set up each buzzer for output
    Serial.print("SETUP PINS: GPIO ");
    Serial.print(output_pins[i]);
    Serial.print(" TO CHANNEL ");
    Serial.println(i*2);
    delay(100);
  }
  delay(1000);
}

void loop() {  
  byte *midi_buffer, *parseptr;
  File midiFile = SD.open(FILENAME,FILE_READ);
  if(!midiFile){
    Serial.println("Failed to open file for reading");
    return;
  }
  unsigned int filelength = (unsigned int)midiFile.size();
  midi_buffer = new byte [filelength + 1];
  if(!midi_buffer){
    Serial.println("Failed to allocate sufficient memory for .mid file");
    return;
  }
  delay(1000);
  int i = 0;
  while (midiFile.available()) {
    midi_buffer[i] = midiFile.read();
    i++;
  }
  midiFile.close();
  for (int i = 0; i<16; i++)
  {
    midiSetChannelVolume(i,127);
  }
  parseptr = midi_buffer;
  
  /////////// MIDI FILE HEADER //////////
  struct midi_header *hdr;
  unsigned int time_division;
  uint16_t num_tracks;
  uint16_t time_div;
  uint16_t ticks_per_beat;
  hdr = (struct midi_header *) parseptr; 

  if (!charcmp ((char *) hdr->MThd, "MThd")) {
    Serial.println("No header found");
    return;    
  }
  num_tracks = rev_short (hdr->number_of_tracks);
  time_division = rev_short (hdr->time_division);
  if (time_division < 0x8000)
  {
    ticks_per_beat = time_division;
  }
  else
  {
    // Edge case with SMPTE time format, very headache inducing, thanks to Len Shustek
    ticks_per_beat = ((time_division >> 8) & 0x7f) *(time_division & 0xff);  
  }
  //Move pointer to track header
  parseptr += rev_long (hdr->header_size) + 8; 
  if (num_tracks > TOTAL_TRACK_COUNT){
    Serial.println("File has too many tracks");
    return;    
  }

  delay(1000);
  struct track_header *trkhdr;
  unsigned long tracklen;

  unsigned long delta_ticks;
  unsigned long currenttempo = 500000;
  unsigned long next_note = micros();
  //struct midi_track *midi_all_tracks = new midi_track [num_tracks];
  
  /////////// SETUP MIDI TRACK HEADERS ///////////
  for (int trk = 0; trk < num_tracks; trk++)
  {  
  	trkhdr = (struct track_header *) parseptr;
  	if (!charcmp ((char *) trkhdr->MTrk, "MTrk")) {
  	  Serial.println("No track header found");
  	  return;    
  	}
  	tracklen = rev_long (trkhdr->track_size);
  	parseptr += sizeof (struct track_header);    
    // Initialise the track parsing struct //
    midi_all_tracks[trk].eventType = 0x0;
    midi_all_tracks[trk].trk_ended = false;
    midi_all_tracks[trk].last_event = 0x0;
    midi_all_tracks[trk].last_event_time = 0;
    midi_all_tracks[trk].track_time = 0;
    midi_all_tracks[trk].remaining_delay = 0;
    
  	/////////// MIDI TRACK PARSING //////////
  	midi_all_tracks[trk].trkptr = parseptr;
  	parseptr += tracklen;
  	midi_all_tracks[trk].trkend = parseptr;
    Serial.print("MIDI TRACK ");
    Serial.print(trk);
    Serial.println(" LOADED");
  }
  bool songend = false;
  while (!songend){
	unsigned long least_delay = ULONG_MAX;
  for (int trk = 1; trk < num_tracks; trk++)
  {
    if (midi_all_tracks[trk].trk_ended && midi_all_tracks[trk-1].trk_ended == midi_all_tracks[trk].trk_ended)
    {
      songend = true;
    }
    else {
      songend = false;
      break;
    }
  }
	for (int trk = 0; trk < num_tracks; trk++)
	{
		if (midi_all_tracks[trk].trkptr < midi_all_tracks[trk].trkend && !midi_all_tracks[trk].trk_ended){
      byte *last_ptr_loc = midi_all_tracks[trk].trkptr;
      delta_ticks = get_varlen (&(&midi_all_tracks[trk])->trkptr);
      if (midi_all_tracks[trk].remaining_delay > 0 ){     
        if (midi_all_tracks[trk].remaining_delay < least_delay){
          least_delay = midi_all_tracks[trk].remaining_delay;
        }
        midi_all_tracks[trk].trkptr = last_ptr_loc;
        continue;
      }
      midi_all_tracks[trk].track_time += delta_ticks;
      if (midi_all_tracks[trk].remaining_delay == 0 && delta_ticks != 0){
        midi_all_tracks[trk].remaining_delay = delta_ticks;  
      }
      if (midi_all_tracks[trk].remaining_delay < least_delay){
        least_delay = midi_all_tracks[trk].remaining_delay;
      }
		  if (*midi_all_tracks[trk].trkptr < 0x80) {
			  midi_all_tracks[trk].eventType = midi_all_tracks[trk].last_event;  // using "running status": same event as before
		  }
		  else 
		  {
			  midi_all_tracks[trk].eventType = *midi_all_tracks[trk].trkptr++; // otherwise get new "status" (event type) */
		  }
	  
		  if (midi_all_tracks[trk].eventType == 0xff) { // meta-event
			byte cmd = *(midi_all_tracks[trk].trkptr++);
			switch (cmd){
			  case 0x51:    // tempo: 3 byte big-endian integer, not a varlen integer!
  				currenttempo = rev_long (*(uint32_t *) (midi_all_tracks[trk].trkptr)) & 0xffffffL;
  				break;
        case 0x2f:
          midi_all_tracks[trk].trk_ended = true;
          midi_all_tracks[trk].trkptr = last_ptr_loc;
          continue;
			  default:
				  break;
			 }
			 midi_all_tracks[trk].trkptr += get_varlen (&(&midi_all_tracks[trk])->trkptr); 
		  }

		  else if (midi_all_tracks[trk].eventType < 0x80){
  			Serial.println(midi_all_tracks[trk].eventType, HEX);
  			Serial.println(".mid file malformed");
			  return;   
		  }

		  else {
			 if (midi_all_tracks[trk].eventType < 0xf0){
				midi_all_tracks[trk].last_event = midi_all_tracks[trk].eventType;  // remember "running status" if not meta or sysex event
			 }
			 midi_all_tracks[trk].channel = (int)(midi_all_tracks[trk].eventType & 0x0f);
			 switch (midi_all_tracks[trk].eventType >> 4) {
			 case 0x8: // note off
				midi_all_tracks[trk].note = *(midi_all_tracks[trk].trkptr++);
				midi_all_tracks[trk].volume = *(midi_all_tracks[trk].trkptr++);
				break;
			 case 0x9: // note on
				midi_all_tracks[trk].note = *(midi_all_tracks[trk].trkptr++);
				midi_all_tracks[trk].volume = *(midi_all_tracks[trk].trkptr++);
				if (midi_all_tracks[trk].volume == 0)
				{
				  midi_all_tracks[trk].eventType &= 0xef;
				}
				break;
			 case 0xc: // program patch, ie which instrument
				midi_all_tracks[trk].instrument = *(midi_all_tracks[trk].trkptr++);
				break;
			 case 0xd: // channel pressure
				midi_all_tracks[trk].trkptr++;
				break;
			 case 0xa: // aftertouch pressure
			 case 0xb: // controller value
			 case 0xe: // pitch wheel change
				midi_all_tracks[trk].trkptr +=2;
				break;
			 case 0xf: // sysex event
				midi_all_tracks[trk].trkptr += get_varlen (&midi_all_tracks[trk].trkptr);
				break;
			 default:
				Serial.println("Unknown MIDI event");
				return;   
			 } 
		  }
		}
	}
	if (least_delay != ULONG_MAX){
		delayMicroseconds((int)round((least_delay/((1/(float)currenttempo)*ticks_per_beat))));
    Serial.print("Time since last: ");
    Serial.println(least_delay);
	}
	for (int trk = 0; trk < num_tracks; trk++)
	{
    if (least_delay != ULONG_MAX ){
      midi_all_tracks[trk].remaining_delay -= least_delay;
    } 
		if (midi_all_tracks[trk].remaining_delay <= 0 && !midi_all_tracks[trk].trk_ended){
			if (midi_all_tracks[trk].eventType >> 4 == 0x8 || midi_all_tracks[trk].eventType >> 4 == 0x9)
			{ 
        if (DEBUG_PRINT){
          Serial.print("TRACK ");
          Serial.print(midi_all_tracks[trk].channel);
  				Serial.print(", Note ");
  				Serial.print((uint8_t)midi_all_tracks[trk].note,HEX);
  				Serial.print(", Volume ");
  				Serial.print((uint8_t)midi_all_tracks[trk].volume);
  				Serial.println(midi_all_tracks[trk].eventType >> 4 == 0x8 ? " Off" : " On");
        }
        if (midi_all_tracks[trk].channel != DRUM_CHANNEL && midi_all_tracks[trk].channel < BUZZER_TRACK_COUNT && USE_BUZZERS){
          int outputchn = midi_all_tracks[trk].channel*2;
          if (midi_all_tracks[trk].eventType >> 4 == 0x9){
            ledcWrite(outputchn, midi_all_tracks[trk].volume*2);
            ledcWriteTone(outputchn, tones[midi_all_tracks[trk].note]);
            if (midi_all_tracks[trk].cur_playing_note != midi_all_tracks[trk].note) {
              midi_all_tracks[trk].cur_playing_note = midi_all_tracks[trk].note;
            }
          }
          else if (midi_all_tracks[trk].cur_playing_note == midi_all_tracks[trk].note)
          {
            ledcWrite(outputchn, 0);
          }
        }
        else{
          VS1053_MIDI.write(midi_all_tracks[trk].eventType);
          VS1053_MIDI.write((uint8_t)midi_all_tracks[trk].note);
          VS1053_MIDI.write(midi_all_tracks[trk].volume);
        }
			}
			else if (midi_all_tracks[trk].eventType >> 4 == 0xc){
				VS1053_MIDI.write(midi_all_tracks[trk].eventType);
				VS1053_MIDI.write(midi_all_tracks[trk].instrument);
				Serial.print("Instrument ");
				Serial.println(midi_all_tracks[trk].instrument);   
			}
		}
	}
  }
  delete[] midi_buffer;
  Serial.println("Track playback complete."); 
  delay(1000);
}
