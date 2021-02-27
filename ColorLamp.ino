/*  IoT Color Lamp Arduino Program
    Version 1.1
    Author: Taylor Giles

    Using code from Example Arduino Program written by
      Sujay Phadke <electronicsguy123@gmail.com>
      Github: @electronicsguy
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include "HTTPSRedirect.h"
#include "DebugMacros.h"

#define MAX_ATTEMPTS 5
#define NUM_NOTES 3

const long RESET_TIME = 604800000; //1 week
const uint8_t TIMEOUT = 20000;

//DO NOT CHANGE (the lamps are soldered onto PCBs to match these pins)
const uint8_t RED_PIN = 14;
const uint8_t GREEN_PIN = 12;
const uint8_t BLUE_PIN = 13;
const uint8_t BUTTON_PIN = 4;
const uint8_t BUZZER_PIN = 5;

const boolean DEBUGGING = true;

//Server information
const char* host = "script.google.com";
const char *GScriptId = "AKfycbywhyKBsQ9cNWjLbCeJ1iff0v9gvV4DIwsAc4XpZQMsM_OK59U";
const char *lampId = "TG-084071";//"SP-083080";//"SB-083042";//"JM-074077";
const int httpsPort = 443;

//URLs
String writeUrl = String("/macros/s/") + GScriptId + "/exec?write&id=" + lampId;
String readUrl = String("/macros/s/") + GScriptId + "/exec?read&id=" + lampId;
String loginUrl = String("/macros/s/") + GScriptId + "/exec?login&id=" + lampId;

//Lamp vars
String myColor;
const String BLANK_COLOR = "000000000";
const String RED_COLOR = "255000000";
const String GREEN_COLOR = "000255000";
const String BLUE_COLOR = "000000255";
const String WHITE_COLOR = "255255255";
String currentVal = BLANK_COLOR;
String currentColor = BLANK_COLOR, prevColor = BLANK_COLOR;
boolean write = false;
long checkTime = 0;
String color = BLANK_COLOR;
int prevRed = 0, prevGreen = 0, prevBlue = 0;

//HTTPSRedirect class object
HTTPSRedirect* client = nullptr;

//Connection vars
int updateInterval;
static int error_count = 0;
static int connect_count = 0;
const unsigned int MAX_CONNECT = 20;
static bool created = false;
boolean isActive = false;

//Jingle vars
struct jingle {
  int freqs[NUM_NOTES];
  int durations[NUM_NOTES];
}; typedef struct jingle Jingle;

const Jingle JINGLES[] = {
  {.freqs = {000, 000, 000}, .durations = {000, 000, 000}}, //Silent
  {.freqs = {1760, 000, 000}, .durations = {200, 000, 000}}, //Beep
  {.freqs = {1760, 1760, 000}, .durations = {175, 175, 000}}, //Beep Twice
  {.freqs = {2349, 1760, 000}, .durations = {175, 175, 000}}, //Ding Dong
  {.freqs = {1760, 2349, 000}, .durations = {175, 175, 000}}, //Dong Ding
  {.freqs = {1174, 1760, 2349}, .durations = {120, 120, 120}}, //Ascending Tri-Tone
  {.freqs = {2349, 1760, 1174}, .durations = {120, 120, 120}}  //Descending Tri-Tone
};
const uint8_t SILENT = 0, BEEP = 1, TWICE = 2, DINGDONG = 3, DONGDING = 4, ASCEND = 5, DESCEND = 6;
Jingle myJingle = JINGLES[SILENT];


void setup() {
  Serial.begin(115200);
  Serial.println("\n");

  //Set pin modes
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  //Check for edit mode
  delay(500);
  if (digitalRead(BUTTON_PIN) == LOW) {
    editMode();
  }

  //Wait for network connection
  setDimColor(WHITE_COLOR);
  if (!connectToServer(MAX_ATTEMPTS)) {
    //Unable to connect
    setColor(RED_COLOR);
    debugPrintln("Error: Unable to connect to server.");
    delay(2000);

    //Pulse red light for 5 mins, then shut down
    long startMillis = millis();
    while(millis() - startMillis < 300000){
      for(int i = 255; i >= 0; i = i - 20){
        fadeRGB(i, 0, 0);
      }
      setColor(BLANK_COLOR);
      for(int i = 0; i <= 255; i = i + 20){
        fadeRGB(i, 0, 0);
      }
    }
    ESP.deepSleep(0);
  }
  debugPrintln("Connection to server successful.");

  //Login to get color, delay time, and sound
  client->setPrintResponseBody(false);
  if (client->GET(loginUrl, host, false)) {
    //Get login body results
    String loginBody = client->getResponseBody();
    int tempSize = strlen(loginBody.c_str());

    //Convert String to char*
    char buffer[tempSize];
    loginBody.toCharArray(buffer, tempSize);

    //Split into tokens and save values (color, interval, sound)
    char* loginResults = strtok(buffer, ",");
    myColor = loginResults;
    loginResults = strtok(NULL, ",");
    updateInterval = atoi(loginResults);
    loginResults = strtok(NULL, ",");
    myJingle = JINGLES[atoi(loginResults)];
  } else {
    //Pulse red light for 5 mins, then shut down
    long startMillis = millis();
    while(millis() - startMillis < 300000){
      for(int i = 255; i >= 0; i = i - 20){
        fadeRGB(i, 0, 0);
      }
      setColor(BLANK_COLOR);
      for(int i = 0; i <= 255; i = i + 20){
        fadeRGB(i, 0, 0);
      }
    }
    ESP.deepSleep(0);
  }

  debugPrint("\nColor assoc. with this ID: ");
  debugPrintln(myColor);
  debugPrint("Time between requests: ");
  debugPrintln(updateInterval);

  //Delete HTTPSRedirect object
  delete client;
  client = nullptr;

  //Attach interrupt on button press
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), needToWrite, FALLING);

  //Set check time to initialize in loop
  checkTime = updateInterval * -1;

  //Successful setup
  setColor(GREEN_COLOR);
  debugPrintln("\nSetup successful.\n");
  delay(2000);
  setColor(BLANK_COLOR);
}


void loop() {
  //Restart the board (thus logging in again) if it has been running for a specified interval
  if (millis() >= RESET_TIME) {
    debugPrintln("Routine restart due to extended activation");
    delay(100);
    ESP.restart();
  }

  //Write to spreadsheet if necessary
  if (write) {
    currentColor = myColor;
    setDimColor(myColor);
    setColor(connectToSpreadsheet(writeUrl));
    write = false;
  }

  //Update if ready
  if (millis() - checkTime >= updateInterval) {
    //Read in current value and set the color of the LED accordingly
    checkTime = millis();
    prevColor = currentColor;
    currentColor = connectToSpreadsheet(readUrl);
    setColor(currentColor);

    //Play jingle if necessary
    if(colorCompare(currentColor, BLANK_COLOR) != 0 && colorCompare(currentColor, prevColor) != 0){
      playJingle(myJingle);
    }
  }
}


/**
   Interrupt attached to button - indicates that a value needs to be written
*/
ICACHE_RAM_ATTR void needToWrite() {
  write = true;
}


/**
   Connects to the spreadsheet using the given URL
   Using code from Sujay Phadke's Arduino Example
*/
String connectToSpreadsheet(String url) {
  if (!created) {
    client = new HTTPSRedirect(httpsPort);
    client->setInsecure();
    created = true;
    client->setPrintResponseBody(false);
    client->setContentTypeHeader("application/json");
  }

  //Connect to host if not already connected
  if (client != nullptr) {
    if (!client->connected()) {
      client->connect(host, httpsPort);
    }
  } else {
    debugPrintln("Error creating client object!");
    error_count = 5;
  }

  //Restart if the number of connection attempts exceeds the maximum number of allowed attempts
  if (connect_count > MAX_CONNECT) {
    connect_count = 0;
    delete client;
    created = false;
    return currentColor; //Avoid random blanks by returning the current color in the event of an error
  }

  //Get current value
  debugPrint("Connect to spreadsheet: ");
  if (client->GET(url, host, false)) {
    ++connect_count;
    error_count = 0;
    currentVal = client->getResponseBody();
    debugPrintln(currentVal);
  } else {
    ++error_count;
    debugPrint("Error-count while connecting: ");
    debugPrintln(error_count);
  }

  //Halt processor in case of continuous error
  if (error_count > 3) {
    debugPrintln("More than 3 failed attempts. Restarting in 10 minutes.");
    delete client;
    client = nullptr;
    Serial.flush();
    delay(600000000); //Stop for 10 minutes
    ESP.restart();
  }
  return currentVal;
}


/*
   Attempts to connect to the network. Returns true if successful, false otherwise
*/
boolean connectToServer(int numAttempts) {
  debugPrint("Connecting to network: ");
  debugPrintln(WiFi.SSID());
  Serial.flush();

  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    return false;
  }

  debugPrintln("");
  debugPrintln("WiFi connected.");
  debugPrint("IP address: ");
  debugPrintln(WiFi.localIP().toString());

  // Use HTTPSRedirect class to create a new TLS connection
  client = new HTTPSRedirect(httpsPort);
  client->setInsecure();
  client->setPrintResponseBody(true);
  client->setContentTypeHeader("application/json");

  debugPrint("Connecting to ");
  debugPrintln(host);

  // Try to connect a maximum of <numAttempts> times
  bool connected = false;
  for (int i = 0; i < numAttempts; i++) {
    int connectionStatus = client->connect(host, httpsPort);
    if (connectionStatus == 1) {
      connected = true;
      break;
    } else {
      debugPrintln("Connection failed. Retrying...");
    }
  }

  if (!connected) {
    debugPrint("Could not connect to server: ");
    debugPrintln(host);
    debugPrintln("Exiting...");
  }

  return connected;
}


/**
 * Fades into the specified RGB color values
 */
void fadeRGB(int redVal, int greenVal, int blueVal){
  //Calculate intervals for fading
  double redInterval = ((double)redVal - (double)prevRed) / 255.0;
  double greenInterval = ((double)greenVal - (double)prevGreen) / 255.0;
  double blueInterval = ((double)blueVal - (double)prevBlue) / 255.0;
  double red = prevRed, green = prevGreen, blue = prevBlue;

  //Fade into new color
  for(int i = 0; i < 255; i++){
    red += redInterval;
    green += greenInterval;
    blue += blueInterval;

    //Set values to LED
    analogWrite(RED_PIN, map(red, 0, 255, 0, 1023));
    analogWrite(GREEN_PIN, map(green, 0, 255, 0, 1023));
    analogWrite(BLUE_PIN, map(blue, 0, 255, 0, 1023));

    delay(1);
  }

  prevRed = redVal;
  prevGreen = greenVal;
  prevBlue = blueVal;
  
  //Set values to LED
  analogWrite(RED_PIN, map(redVal, 0, 255, 0, 1023));
  analogWrite(GREEN_PIN, map(greenVal, 0, 255, 0, 1023));
  analogWrite(BLUE_PIN, map(blueVal, 0, 255, 0, 1023));
}


/**
   Sets the color of the LED to match the given String.
   String should be of the format #########,
   where the first three values denote red intensity, then green, then blue.
*/
void setColor(String colorString) {
  int redVal, greenVal, blueVal;

  //Get values from string
  if (colorString.length() < 9) {
    redVal = 0;
    greenVal = 0;
    blueVal = 0;
  } else {
    redVal = colorString.substring(0, 3).toInt();
    greenVal = colorString.substring(3, 6).toInt();
    blueVal = colorString.substring(6, 9).toInt();
  }

  //Set color
  fadeRGB(redVal, greenVal, blueVal);
}


/**
   Sets the color of the LED to match a dimmed version of the
   specified String RGB color representation.
*/
void setDimColor(String colorString) {
  int redVal, greenVal, blueVal;

  //Get values from string
  if (colorString.length() < 9) {
    redVal = 0;
    greenVal = 0;
    blueVal = 0;
  } else {
    redVal = colorString.substring(0, 3).toInt();
    greenVal = colorString.substring(3, 6).toInt();
    blueVal = colorString.substring(6, 9).toInt();
  }

  //Set color
  fadeRGB(redVal * 0.5, greenVal * 0.5, blueVal * 0.5);
}


/**
   Uses the WiFiManager library to open an access point
   and receive network credentials.
*/
void editMode() {
  setColor(BLUE_COLOR);
  String oldSSID, oldPass;
  oldSSID = WiFi.SSID();
  oldPass = WiFi.psk();
  WiFi.disconnect();
  WiFiManager wifiManager;
  if(!wifiManager.startConfigPortal("Color-Lamp", "TheCoreSquad")){
    //Pulse red light for 5 mins, then shut down
    long startMillis = millis();
    while(millis() - startMillis < 300000){
      for(int i = 255; i >= 0; i = i - 20){
        fadeRGB(i, 0, 0);
      }
      setColor(BLANK_COLOR);
      for(int i = 0; i <= 255; i = i + 20){
        fadeRGB(i, 0, 0);
      }
    }
    ESP.deepSleep(0);
  } else {
    //Connection success behavior
    setColor(GREEN_COLOR);
    delay(3000);
    setColor(BLANK_COLOR);
    ESP.restart();
  }
}


/**
 * Plays the given jingle on the buzzer
 */
void playJingle(Jingle jingle){
  for(int i = 0; i < NUM_NOTES; i++){
    tone(BUZZER_PIN, jingle.freqs[i], jingle.durations[i]);
    Serial.print((int)jingle.freqs[i]);
    Serial.print(" ");
    Serial.println((int)jingle.durations[i]);
    delay(jingle.durations[i]);
  }
  noTone(BUZZER_PIN);
}


/**
 * Compares two color String representations
 */
int colorCompare(String color1, String color2){
  int red1, green1, blue1;
  int red2, green2, blue2;

  //Get values from string
  if (color1.length() < 9) {
    red1 = 0;
    green1 = 0;
    blue1 = 0;
  } else {
    red1 = color1.substring(0, 3).toInt();
    green1 = color1.substring(3, 6).toInt();
    blue1 = color1.substring(6, 9).toInt();
  }

  if (color2.length() < 9) {
    red2 = 0;
    green2 = 0;
    blue2 = 0;
  } else {
    red2 = color2.substring(0, 3).toInt();
    green2 = color2.substring(3, 6).toInt();
    blue2 = color2.substring(6, 9).toInt();
  }

  int rDiff = red1 - red2;
  int gDiff = green1 - green2;
  int bDiff = blue1 - blue2;
  if(rDiff == 0){
    if(gDiff == 0){
      if(bDiff == 0){
        return 0;
      } else {
        return bDiff;
      }
    } else {
      return gDiff;
    }
  } else {
    return rDiff;
  }

  
}


/**
   Debugger print macros
*/
void debugPrint(String str) {
  if (DEBUGGING) {
    Serial.print(str);
  }
}

void debugPrint(int num) {
  if (DEBUGGING) {
    Serial.print((String)num);
  }
}

void debugPrintln(String str) {
  if (DEBUGGING) {
    Serial.println(str);
  }
}

void debugPrintln(int num) {
  if (DEBUGGING) {
    Serial.println((String)num);
  }
}
