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
#define RELAY_EN D3
#define MAINS_SENSE_PIN D4

File dataRoot;

// Setup start: For twitter webclient api
#include <NTPClient.h>
#include <TwitterWebAPI.h>
#include <ctime>


#include <Wire.h>             // Wire library (required for I2C devices)
#include "RTClib.h"

const char *ntp_server = "pool.ntp.org";
int timezone = 5; // IST to GMT difference

WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntp_server, (timezone * 3600) + 1800, 60000); // NTP server pool, offset (in seconds), update interval (in milliseconds)
TwitterClient tcr(timeClient, CONSUMER_KEY, CONSUMER_SECRET, ACCESS_TOKEN, ACCESS_TOKEN_SECRET);

int publishTweet(std::string event, time_t epoch);
int publishPowerOnEvent(time_t epoch);
int publishPowerOffEvent(time_t epoch);
int publishCounter = 1;
std::string &reduceDoubleSpaces(std::string &s);
std::string &removeNewLines(std::string &s);

time_t ntpEpoch;
// Setup End: For twitter webclient api

File getLatestFileByDate(File rootDir, std::string date, time_t epochTime);
void publishUnpublishedEvents(File rootDir);
std::vector<std::string> listDirSorted(File rootDir);
void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus);
void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus);
void writePowerOffEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus);
std::string getFilenameFromEpoch(time_t epochTime);
std::string getTimeOfEventFromEpoch(time_t epochTime);
boolean isEpochNTPSynced(time_t epoch);
std::vector<std::string> getPublishStatusContent();
boolean doesStatusExist(std::vector<std::string> statusVec);
std::string getDateFromStatus(std::vector<std::string> statusVec);
std::string getLineNumFromStatus(std::vector<std::string> statusVec);
void persistPubStatusToFile(std::string date, std::string lineNum);
void run();
time_t getTimeFromMultipleSources();
boolean requireRtcTimeAdjustment(tm *localTime, DateTime rtcNow);

// RTC setup
RTC_DS1307 RTC; // Setup an instance of DS1307 naming it RTC

int i;
#define MAINS_ANALOG_SENSE_PIN A0

File currentDayFile;
std::string currentDateString;
bool mainsPoweredOffAtleastOnce = false;
int lastSensorReading = 0;
int lastMainPowerStatus = -1;

int mainPowerStatus();
int digitalMainPowerStatus();
void updatePowerStatusIfChanged();
void shutdown();

IRAM_ATTR void senseFallingState();

boolean shouldInformNano = true;

void setup()
{
  Serial.begin(115200);
  if (!RTC.begin()) {
    Serial.println("Couldn't find RTC");
  } else {
    Serial.println("Connected to RTC");
  }

  if (!RTC.isrunning()) {
    Serial.println("RTC is NOT running!");
  } else {
    Serial.println("RTC is running!");
  }

  Serial.print("consumer key: ");
  Serial.println(CONSUMER_KEY);
  Serial.print("access token: ");
  Serial.println(ACCESS_TOKEN);
  // LittleFS.begin();
  GUI.begin();
  configManager.begin();
  WiFiManager.begin("quality-of-power-supply-reporter");
  timeSync.begin();
  tcr.startNTP();
  
  // Get time for NTP/RTC
  ntpEpoch = getTimeFromMultipleSources();

  Serial.print("Initializing SD card...");

  bool initFailed = false;

  if (!SD.begin(CS_PIN, SD_SCK_MHZ(1)))
  {
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
  if (initFailed)
  {
    delay(1000);
    while (1)
      ; // Soft reset
  }

  Serial.println("initialization done.");
  dataRoot = SD.open("/qop");
  if (!dataRoot)
  {
    SD.mkdir("qop");
    dataRoot = SD.open("/qop");
  }
  File pubDataRoot = SD.open("/qop-published");
  if (!pubDataRoot)
  {
    SD.mkdir("qop-published");
  }
  pubDataRoot.close();

  root = SD.open("/");
  printDirectory(root, 0);
  Serial.println("done!");
}

void loop()
{
  WiFiManager.loop();
  updater.loop();
  configManager.loop();
  // run();
  if (millis() >= i) {
    Serial.print("Loop start: ");
    Serial.println(i);
    i = i + 1000;
    publishUnpublishedEvents(dataRoot);
  }
}

int mainPowerStatus() {
  // return digitalMainPowerStatus();
  int currentSensorValue = analogRead(MAINS_ANALOG_SENSE_PIN);
  int diff = currentSensorValue - lastSensorReading;
  Serial.print("lastSensorReading: ");
  Serial.println(lastSensorReading);
  Serial.print("currentSensorValue: ");
  Serial.println(currentSensorValue);
  Serial.print("diff: ");
  Serial.println(diff);
  lastSensorReading = currentSensorValue;
  if (mainsPoweredOffAtleastOnce) {
    if (diff >= 100) {
      return 1;
    } else if (diff < -100) {
      return 0;
    }
  } else {
    if (diff <= -100) {
      mainsPoweredOffAtleastOnce = true;
      return 0;
    }
  }
  return -1;
}

void updatePowerStatusIfChanged() {
  Serial.println("Updating power status if changed");
  time_t currentEpochTime = getTimeFromMultipleSources();
  Serial.print("Current epoch time: ");
  Serial.println(currentEpochTime);
  if (currentDayFile == NULL) {
    currentDateString = getFilenameFromEpoch(currentEpochTime);
    currentDayFile = getLatestFileByDate(dataRoot, currentDateString, currentEpochTime);
    std::string timeOfEventString = getTimeOfEventFromEpoch(currentEpochTime);
    writePowerResumeEventToFile(currentDayFile, timeOfEventString, currentEpochTime, isEpochNTPSynced(currentEpochTime));
  } else {
    if (isEpochNTPSynced(currentEpochTime))
    {
      std::string newDateString = getFilenameFromEpoch(currentEpochTime);
      // Create new file in case, date changes
      if (currentDateString.compare(newDateString) != 0)
      {
        currentDayFile.close();
        currentDayFile = getLatestFileByDate(dataRoot, newDateString, currentEpochTime);
        currentDateString = newDateString;
        Serial.print("picked file for writing new events: ");
        Serial.println(currentDayFile.fullName());
      }
    }
  }

  int currentMainPowerStatus = mainPowerStatus();
  Serial.print("mainPowerStatus: ");
  Serial.println(currentMainPowerStatus);
  // Writes event only if main power status changed and it is different from last one
  if (currentMainPowerStatus != -1 && lastMainPowerStatus != currentMainPowerStatus) {
    std::string timeOfEventString = getTimeOfEventFromEpoch(currentEpochTime);
    if (currentMainPowerStatus == 1) {
      writePowerResumeEventToFile(currentDayFile, timeOfEventString, currentEpochTime, isEpochNTPSynced(currentEpochTime));
    } else if (currentMainPowerStatus == 0) {
      writePowerOffEventToFile(currentDayFile, timeOfEventString, currentEpochTime, isEpochNTPSynced(currentEpochTime));
    }
    lastMainPowerStatus = currentMainPowerStatus;
  }
}

time_t getTimeFromMultipleSources() {
  ntpEpoch = tcr.getEpoch();
  tm *localTime = std::localtime(&ntpEpoch);

  DateTime rtcNow = RTC.now();
  String nowTimestamp = rtcNow.timestamp();
  Serial.print("RTC timestamp before adjustment:");
  Serial.println(nowTimestamp);

  // Making sure epoch timestamp year is greater than current year itself (dirty check)
  if (localTime->tm_year + 1900 >= 2022 && requireRtcTimeAdjustment(localTime, rtcNow))  {
    Serial.println("adjusting RTC clock");
    RTC.adjust(DateTime(uint16_t(localTime->tm_year+1900), uint8_t(localTime->tm_mon + 1), uint8_t(localTime->tm_mday), uint8_t(localTime->tm_hour), uint8_t(localTime->tm_min), uint8_t(localTime->tm_sec)));  // Time and date is expanded to date and time on your computer at compiletime
    rtcNow = RTC.now();
    String nowTimestamp = rtcNow.timestamp();
    Serial.print("RTC timestamp after adjustment:");
    Serial.println(nowTimestamp);
    return ntpEpoch;
  }

  uint32_t rtcEpoch = rtcNow.unixtime();
  Serial.print("NTP epoch:");
  Serial.println(ntpEpoch);
  Serial.print("RTC epoch:");
  Serial.println(rtcEpoch);
  return time_t(rtcEpoch);
}

boolean requireRtcTimeAdjustment(tm *localTime, DateTime rtcNow) {
  return localTime->tm_year+1900 != rtcNow.year() || localTime->tm_mon + 1 != rtcNow.month() || localTime->tm_mday != rtcNow.day() ||
  localTime->tm_hour != rtcNow.hour() || (localTime->tm_min - rtcNow.minute()) > 10;
}


boolean isEpochNTPSynced(time_t epoch)
{
  return (ntpEpoch > 1651363200);
}

void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus)
{
  Serial.print("PRES timeOfEvent: ");
  Serial.println(timeOfEvent.c_str());
  if (ntpStatus)
  {
    dateFile.print("1,");
  }
  else
  {
    dateFile.print("-,");
  }
  dateFile.print("PRES,");
  dateFile.print(timeOfEvent.c_str());
  dateFile.print(",");
  dateFile.println(epoch);
  dateFile.flush();
}

void writePowerOffEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus)
{
  Serial.print("POFF timeOfEvent: ");
  Serial.println(timeOfEvent.c_str());
  if (ntpStatus)
  {
    dateFile.print("1,");
  }
  else
  {
    dateFile.print("-,");
  }
  dateFile.print("POFF,");
  dateFile.print(timeOfEvent.c_str());
  dateFile.print(",");
  dateFile.println(epoch);
  dateFile.flush();
}

void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus)
{
  Serial.print("PON timeOfEvent: ");
  Serial.println(timeOfEvent.c_str());
  if (ntpStatus)
  {
    dateFile.print("1,");
  }
  else
  {
    dateFile.print("-,");
  }
  dateFile.print("PON,");
  dateFile.print(timeOfEvent.c_str());
  dateFile.print(",");
  dateFile.println(epoch);
  dateFile.flush();
}

std::string getFilenameFromEpoch(time_t epochTime)
{
  tm *localTime = std::localtime(&epochTime);
  char dateBuf[9];
  sprintf(dateBuf, "%d%02d%02d", 1900 + (localTime->tm_year), (localTime->tm_mon) + 1, localTime->tm_mday);
  return dateBuf;
}

std::string getTimeOfEventFromEpoch(time_t epochTime)
{
  tm *localTime = std::localtime(&epochTime);
  char timeOfEvent[9];
  sprintf(timeOfEvent, "%02d:%02d:%02d", localTime->tm_hour, localTime->tm_min, localTime->tm_sec);
  return timeOfEvent;
}

// Get Latest file matching current date if time stamp is valid, otherwise returns latest file
File getLatestFileByDate(File rootDir, std::string date, time_t epochTime)
{
  std::vector<std::string> filenamesSorted = listDirSorted(rootDir);
  if (!filenamesSorted.empty()) {
  std::string lastFile = filenamesSorted.at(filenamesSorted.size() - 1);
    if (!isEpochNTPSynced(epochTime))
    {
      return SD.open(lastFile.c_str(), FILE_WRITE);
    }
  }
  std::string dateFilePath = rootDir.fullName();
  dateFilePath.append("/" + date);
  return SD.open(dateFilePath.c_str(), FILE_WRITE);
}

void publishUnpublishedEvents(File rootDir)
{
  // check and update power status change - start
  updatePowerStatusIfChanged();
  // check and update power status change - end

  std::vector<std::string> sortedFilenames;
  Serial.println("started sorting directories");
  sortedFilenames = listDirSorted(rootDir);
  if (sortedFilenames.size() < 1)
  {
    return;
  }

  std::string unPublishedStartDate;
  std::string unPublishedLineNum;
  std::vector<std::string> pubStatus = getPublishStatusContent();
  // check if there is existing file status
  if (doesStatusExist(pubStatus))
  {
    unPublishedStartDate = getDateFromStatus(pubStatus);
    unPublishedLineNum = getLineNumFromStatus(pubStatus);
  }
  Serial.print("unPublishedStartDate: ");
  Serial.print(unPublishedStartDate.c_str());
  Serial.print(" , ");
  Serial.print("unPublishedLineNum: ");
  Serial.println(unPublishedLineNum.c_str());

  // check and update power status change - start
  updatePowerStatusIfChanged();
  // check and update power status change - end

  Serial.println("processing sorted file names one by one");
  std::string prevEventName;
  std::string prevTimeOfEvent;
  std::string prevNtpSynced;
  std::string prevEpoch;
  for (int i = 0; i < sortedFilenames.size(); i++)
  {
    // check and update power status change - start
    updatePowerStatusIfChanged();
    // check and update power status change - end

    std::string pickedFile = sortedFilenames.at(i);
    // if publish status exist, skip files older than that
    if (!unPublishedStartDate.empty() && !unPublishedLineNum.empty())
    {
      if (pickedFile.compare(unPublishedStartDate) < 0)
      {
        // Move this file to published dir /qop-published/
        Serial.print("skipping file for publishing: ");
        Serial.println(pickedFile.c_str());

        std::string actualFileName = pickedFile.substr(pickedFile.rfind("/") + 1);
        std::string newPubFilePath = "/qop-published/" + actualFileName;
        Serial.print("moving file to qop-published directory: ");
        Serial.println(newPubFilePath.c_str());
        SD.rename(pickedFile.c_str(), newPubFilePath.c_str());
        continue;
      }
    }
    Serial.print("opening file for read: ");
    Serial.println(pickedFile.c_str());
    File openedFile = SD.open(pickedFile.c_str(), FILE_READ);
    String line;
    int currLineNumber = 0;
    while (true)
    {
      // check and update power status change - start
      updatePowerStatusIfChanged();
      // check and update power status change - end

      line = openedFile.readStringUntil('\n');
      // Serial.print("line read: ");
      // Serial.println(line);
      if (line.isEmpty())
      {
        break;
      }
      currLineNumber++;
      if (!unPublishedLineNum.empty())
      {
        if (pickedFile.compare(unPublishedStartDate) == 0 && currLineNumber < std::stoi(unPublishedLineNum))
        {
          continue;
        }
      }

      boolean ntpSynced = false;
      std::string timeOfEvent;
      std::string eventName;
      std::string epoch;
      char *token = std::strtok((char *)line.c_str(), ",");
      int idx = 0;
      while (token != NULL && idx <= 3)
      {
        switch (idx++)
        {
        case 0:
          if (token == "-")
          { // NTP synced time
            ntpSynced = true;
          }
          break;
        case 1:
          eventName = token;
          break;
        case 2:
          if (token != "")
          { // Event time stamp
            timeOfEvent = token;
          }
          break;
        case 3:
          epoch = token;
          break;
        default:
          break;
        }
        token = std::strtok(NULL, ",");
      }
      Serial.println("====== event info start ======");
      Serial.print("timeOfEvent: ");
      Serial.println(timeOfEvent.c_str());
      Serial.print("eventName: ");
      Serial.println(eventName.c_str());
      Serial.print("ntpSynced: ");
      Serial.println(ntpSynced);
      Serial.print("epoch: ");
      Serial.println(epoch.c_str());
      Serial.println("====== event info end ======");
      if (unPublishedLineNum.empty() || pickedFile.compare(unPublishedStartDate) > 0 || (pickedFile.compare(unPublishedStartDate) == 0 && currLineNumber > std::stoi(unPublishedLineNum)))
      {
        //    check for unpublished events
        //    publish event
        if (prevEventName == "POFF")
          {
            // publish power off event first
            int epochInt = std::stoi(prevEpoch);
            int pubStatus = publishPowerOffEvent(epochInt);
            if (pubStatus == 0)
            {
              openedFile.close();
              return;
            }
            Serial.println("published power off event successfully");
            persistPubStatusToFile(pickedFile, std::to_string(currLineNumber - 1));
        }
        if (eventName == "PRES")
        {
          // publish power resumed event
          int epochInt = std::stoi(epoch);
          int pubStatus = publishPowerOnEvent(epochInt);
          if (pubStatus == 0)
          {
            openedFile.close();
            return;
          }
          Serial.println("published power resumed event successfully");
          persistPubStatusToFile(pickedFile, std::to_string(currLineNumber));
        }
      }

      prevEventName = eventName;
      prevTimeOfEvent = timeOfEvent;
      prevNtpSynced = ntpSynced;
      prevEpoch = epoch;
    }
    openedFile.close();
  }
}

void persistPubStatusToFile(std::string date, std::string lineNum)
{
  // Do recoverable file write
  Serial.println("persisting publish status");
  SD.remove("qop.status");
  File pubStatusFile = SD.open("qop.status", FILE_WRITE);
  pubStatusFile.print(date.c_str());
  pubStatusFile.print(",");
  pubStatusFile.println(lineNum.c_str());
  pubStatusFile.flush();
  pubStatusFile.close();
  Serial.println("publish status persisted successfully");
}

std::vector<std::string> getPublishStatusContent()
{
  std::vector<std::string> pubStatus;
  File pubStatusFile = SD.open("qop.status", FILE_READ);
  String status = pubStatusFile.readStringUntil('\n');
  pubStatusFile.close();
  if (status == "")
  {
    return pubStatus;
  }
  char *token = std::strtok((char *)status.c_str(), ",");
  pubStatus.push_back(token);
  token = std::strtok(NULL, ",");
  pubStatus.push_back(token);
  return pubStatus;
}

boolean doesStatusExist(std::vector<std::string> statusVec)
{
  return statusVec.size() != 0;
}

std::string getDateFromStatus(std::vector<std::string> statusVec)
{
  return statusVec.at(0);
}

std::string getLineNumFromStatus(std::vector<std::string> statusVec)
{
  return statusVec.at(1);
}

std::vector<std::string> listDirSorted(File rootDir)
{
  std::vector<std::string> filenames;
  while (true)
  {
    File pickedFile = rootDir.openNextFile();
    if (!pickedFile)
    {
      break;
    }
    filenames.push_back(pickedFile.fullName());
    pickedFile.close();
  }
  std::sort(filenames.begin(), filenames.end());
  return filenames;
}

int publishPowerOnEvent(time_t epoch)
{
  return publishTweet("[power-on]", epoch);
}
int publishPowerOffEvent(time_t epoch)
{
  return publishTweet("[power-off]", epoch);
}
int publishTweet(std::string event, time_t epoch)
{
  std::string epochCovertedTime = std::asctime(std::localtime(&epoch));
  std::string newTweet = event + "[Time:" + epochCovertedTime + "]";
  std::string cleanedTweet = removeNewLines(reduceDoubleSpaces(newTweet));
  Serial.println(cleanedTweet.c_str());

  boolean val = tcr.tweet(cleanedTweet);
  Serial.print("Tweet published status: ");
  Serial.println(val);
  return val;
}

// Copied from: https://stackoverflow.com/a/48029948
std::string &reduceDoubleSpaces(std::string &s)
{
  std::string::size_type pos = s.find("  ");
  while (pos != std::string::npos)
  {
    // replace BOTH spaces with one space
    s.replace(pos, 2, " ");
    // start searching again, where you left off
    // rather than going back to the beginning
    pos = s.find("  ", pos);
  }
  return s;
}
std::string &removeNewLines(std::string &s)
{
  std::string::size_type pos = s.find("\n");
  while (pos != std::string::npos)
  {
    // replace BOTH spaces with one space
    s.replace(pos, 1, "");
    // start searching again, where you left off
    // rather than going back to the beginning
    pos = s.find("\n", pos);
  }
  return s;
}
void printDirectory(File dir, int numTabs)
{
  while (true)
  {

    File entry = dir.openNextFile();
    if (!entry)
    {
      // no more files
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++)
    {
      Serial.print('\t');
    }
    Serial.print(entry.name());
    if (entry.isDirectory())
    {
      Serial.println("/");
      printDirectory(entry, numTabs + 1);
    }
    else
    {
      // files have sizes, directories do not
      Serial.print("\t\t");
      Serial.println(entry.size(), DEC);
    }
    entry.close();
  }
}
