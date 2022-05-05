#include <Arduino.h>
#include "LittleFS.h"

#include "WiFiManager.h"
#include "webServer.h"
#include "updater.h"
#include "fetch.h"
#include "configManager.h"
#include "timeSync.h"

#include "Config.h"

#define ledPin 5
#define onboardLedPin 2

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
// Setup End: For twitter webclient api

void setup() {
    Serial.print("consumer key: ");
    Serial.println(CONSUMER_KEY);
    Serial.print("access token: ");
    Serial.println(ACCESS_TOKEN);
    pinMode(ledPin, OUTPUT);
    pinMode(onboardLedPin, OUTPUT);
    digitalWrite(ledPin, HIGH);
    digitalWrite(onboardLedPin, HIGH);
    Serial.begin(115200);
    LittleFS.begin();
    GUI.begin();
    configManager.begin();
    WiFiManager.begin("quality-of-power-supply-reporter");
    Serial.println("timeSync.begin()");
    timeSync.begin();
    Serial.println("tcr.startNTP()");

    tcr.startNTP();
}

void loop() {
    WiFiManager.loop();
    updater.loop();
    configManager.loop();
    time_t epoch = tcr.getEpoch();
    // Serial.println(epoch);
    if (epoch < 1651363200) {
        return;
    }

    if (publishCounter++ <= 1) {
        Serial.println("Publishing Tweet");
        publishTweet(epoch);
        delay(30000);
        digitalWrite(ledPin, LOW);
        digitalWrite(onboardLedPin, LOW);
        Serial.print("TurningOff");
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