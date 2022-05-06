#include <Arduino.h>
// #include "LittleFS.h"

#include "WiFiManager.h"
#include "webServer.h"
#include "updater.h"
#include "fetch.h"
#include "configManager.h"
#include "timeSync.h"

#include "Config.h"

#include <SPI.h>
#include <SD.h>
#include <SdFat.h>

File root;
void printDirectory(File dir, int numTabs);
#define CS_PIN D8

// Setup start: For twitter webclient api
#include <NTPClient.h>
#include <TwitterWebAPI.h>
#include <ctime>

const char *ntp_server = "pool.ntp.org";
int timezone = 5; // IST to GMT difference

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntp_server, (timezone*3600) + 1800, 60000);  // NTP server pool, offset (in seconds), update interval (in milliseconds)
TwitterClient tcr(timeClient, CONSUMER_KEY, CONSUMER_SECRET, ACCESS_TOKEN, ACCESS_TOKEN_SECRET);

void publishTweet(time_t epoch);
int publishCounter = 1;
std::string& reduce_double_spaces(std::string& s);
std::string& remove_new_lines(std::string& s);

time_t powerOnTimeInEpoch;
// Setup End: For twitter webclient api

void setup() {
    Serial.print("consumer key: ");
    Serial.println(CONSUMER_KEY);
    Serial.print("access token: ");
    Serial.println(ACCESS_TOKEN);
    Serial.begin(115200);
    // LittleFS.begin();
    GUI.begin();
    configManager.begin();
    WiFiManager.begin("quality-of-power-supply-reporter");
    Serial.println("timeSync.begin()");
    timeSync.begin();
    Serial.println("tcr.startNTP()");

    tcr.startNTP();
    powerOnTimeInEpoch = tcr.getEpoch();

    Serial.print("Initializing SD card...");

    bool initFailed = false;

    if (!SD.begin(CS_PIN, SD_SCK_MHZ(1))) {
        initFailed = true;
        Serial.println("initialization failed!");
        Serial.println();
    }

    delay(500);

    Serial.print("Card type:         ");
    Serial.println(SD.type());
    Serial.print("fatType:           ");
    Serial.println(SD.fatType());
    Serial.print("size:              ");
    Serial.println(SD.size());

    Serial.println();
    if (initFailed) {
        delay(1000);
        while (1); // Soft reset
    }

    Serial.println("initialization done.");
    root = SD.open("/");
    printDirectory(root, 0);
    Serial.println("done!");
}

void loop() {
    WiFiManager.loop();
    updater.loop();
    configManager.loop();
    // time_t epoch = tcr.getEpoch();
    // Serial.println(epoch);
    if (powerOnTimeInEpoch < 1651363200) {
        return;
    }

    if (publishCounter++ <= 1) {
        Serial.println("Publishing Tweet");
        publishTweet(powerOnTimeInEpoch);
    }

    // delay(60000);
}

void publishTweet(time_t epoch) {
    // String timestamp = tcr.getTimeStamp();
    // std::string tweet = "[power-on][Time:" + std::string(timestamp.c_str()) + "]";
    // Serial.println(tweet.c_str());
    std::string epochCovertedTime = std::asctime(std::localtime(&epoch));
    std::string newTweet = "[power-on][Time:" + epochCovertedTime + "]";
    std::string cleanedTweet = remove_new_lines(reduce_double_spaces(newTweet));
    Serial.println(cleanedTweet.c_str());
    
    boolean val = tcr.tweet(cleanedTweet);
    Serial.print("Tweet published status: ");
    Serial.println(val);
}

// Copied from: https://stackoverflow.com/a/48029948
std::string& reduce_double_spaces(std::string& s)
{
    std::string::size_type pos = s.find("  ");
    while (pos != std::string::npos) {
        // replace BOTH spaces with one space
        s.replace(pos, 2, " ");
        // start searching again, where you left off
        // rather than going back to the beginning
        pos = s.find("  ", pos);
    }
    return s;
}
std::string& remove_new_lines(std::string& s)
{
    std::string::size_type pos = s.find("\n");
    while (pos != std::string::npos) {
        // replace BOTH spaces with one space
        s.replace(pos, 1, "");
        // start searching again, where you left off
        // rather than going back to the beginning
        pos = s.find("\n", pos);
    }
    return s;
}

void printDirectory(File dir, int numTabs) {
  while (true) {

    File entry =  dir.openNextFile();
    if (! entry) {
      // no more files
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      Serial.print('\t');
    }
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      printDirectory(entry, numTabs + 1);
    } else {
      // files have sizes, directories do not
      Serial.print("\t\t");
      Serial.println(entry.size(), DEC);
    }
    entry.close();
  }
}
