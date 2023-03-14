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
#define CS_PIN D8
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
std::string &reduceDoubleSpaces(std::string &s);
std::string &removeNewLines(std::string &s);

time_t ntpEpoch;
// Setup End: For twitter webclient api

File root;
File dataRoot;
void printDirectory(File dir, int numTabs);
File getLatestFileByDate(File rootDir, std::string date, time_t epochTime);
void publishUnpublishedEvents(File rootDir);
std::vector<std::string> listDirSorted(File rootDir);
void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus);
// void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus);
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

#define MAINS_ANALOG_SENSE_PIN A0

File currentDayFile;
std::string currentDateString;
bool mainsPoweredOffAtleastOnce = false;
int lastSensorReading;
int lastMainPowerStatus = -1;

int mainPowerStatus();
void updatePowerStatusIfChanged();

IRAM_ATTR void senseFallingState();

boolean shouldInformNano = true;
unsigned long i;

void setup()
{
  Serial.begin(115200);
  if (!RTC.begin()) {
    Serial.println("ERR: Couldn't find RTC");
  } else {
    Serial.println("INFO: RTC Connected");
  }

  if (!RTC.isrunning()) {
    Serial.println("ERR: RTC is NOT running!");
  } else {
    Serial.println("INFO: RTC is running!");
  }

  Serial.print("DEBUG: twitter account consumer key: ");
  Serial.println(CONSUMER_KEY);
  Serial.print("DEBUG: twitter account access token: ");
  Serial.println(ACCESS_TOKEN);
  // LittleFS.begin();
  GUI.begin();
  configManager.begin();
  WiFiManager.begin("quality-of-power-supply-reporter");
  timeSync.begin();
  tcr.startNTP();
  
  // Get time for NTP/RTC
  ntpEpoch = getTimeFromMultipleSources();

  Serial.println("INFO: Initializing SD card...");

  bool initFailed = false;

  if (!SD.begin(CS_PIN, SD_SCK_MHZ(1)))
  {
    initFailed = true;
    Serial.println("ERR: Initialization SD card failed!");
    Serial.println();
  }

  delay(500);

  Serial.print("INFO: SD card type:         ");
  Serial.println(SD.type());
  Serial.print("INFO: SD card fatType:      ");
  Serial.println(SD.fatType());
  Serial.print("INFO: SD card size:         ");
  Serial.println(SD.size());

  Serial.println();
  if (initFailed)
  {
    delay(1000);
    while (1)
      ; // Soft reset
  }

  Serial.println("INFO: Initialization SD card complete");
  Serial.println("DEBUG: dataRoot folder is /qop");
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
  Serial.println("INFO: Setup complete!");
}

void loop()
{
  WiFiManager.loop();
  updater.loop();
  configManager.loop();
  // run();
  unsigned long currentMillis = millis();
  if (currentMillis >= i) {
    Serial.print("DEBUG: Run Loop started after ms: ");
    Serial.println(i);
    i = currentMillis + 5000;
    publishUnpublishedEvents(dataRoot);
  }
}

// mainPowerStatus: Returns 0 if power off for the first time, 
// post that returns 1 on power On and 0 on Powerr Off, otherwise returns -1
int mainPowerStatus() {
  int currentSensorValue = analogRead(MAINS_ANALOG_SENSE_PIN);
  if (lastSensorReading == NULL) {
    lastSensorReading = currentSensorValue;
  }
  int diff = currentSensorValue - lastSensorReading;
  Serial.print("DEBUG: lastSensorReading: ");
  Serial.println(lastSensorReading);
  Serial.print("DEBUG: currentSensorValue: ");
  Serial.println(currentSensorValue);
  Serial.print("DEBUG: sensor reading diff: ");
  Serial.println(diff);
  lastSensorReading = currentSensorValue;
  // diff +ve is raising edge and -ve is falling edge
  if (diff >= 100) {
    return 1;
  } else if (diff <= -100) {
    return 0;
  }
  return -1;
}

void updatePowerStatusIfChanged() {
  time_t currentEpochTime = getTimeFromMultipleSources();

  if (isEpochNTPSynced(currentEpochTime)) {
    std::string newDateString = getFilenameFromEpoch(currentEpochTime);
    // Create new file in case, date changes
    if (currentDateString.compare(newDateString) != 0)
    {
      if (currentDayFile != NULL) {
        currentDayFile.close();
      }
      
      currentDayFile = getLatestFileByDate(dataRoot, newDateString, currentEpochTime);
      currentDateString = newDateString;
      Serial.print("INFO: Picked new date file for upcoming events: ");
      Serial.println(currentDayFile.fullName());
    }
  } else {
    Serial.print("DEBUG: epoch time is less than minimum epoch configured, current epoch time: ");
    Serial.println(currentEpochTime);
  }

  int currentMainPowerStatus = mainPowerStatus();
  Serial.print("DEBUG: Main Power status(1 -> PRES, 0 -> POFF and -1 for no change) evaluated to: ");
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
  Serial.print("DEBUG: NTP date before adjustment -> ");
  Serial.print("NTP year: ");
  Serial.print(localTime->tm_year);
  Serial.print(", NTP month: ");
  Serial.print(localTime->tm_mon);
  Serial.print(", NTP date: ");
  Serial.println(localTime->tm_mday);


  DateTime rtcNow = RTC.now();
  String nowTimestamp = rtcNow.timestamp();
  Serial.print("DEBUG: RTC timestamp before adjustment:");
  Serial.println(nowTimestamp);

  // Making sure epoch timestamp year is greater than current year (2022) itself (dirty check)
  if (localTime->tm_year + 1900 >= 2022 && requireRtcTimeAdjustment(localTime, rtcNow))  {
    Serial.println("DEBUG: RTC clock needed adjustment");
    RTC.adjust(DateTime(uint16_t(localTime->tm_year+1900), uint8_t(localTime->tm_mon + 1), uint8_t(localTime->tm_mday), uint8_t(localTime->tm_hour), uint8_t(localTime->tm_min), uint8_t(localTime->tm_sec)));  // Time and date is expanded to date and time on your computer at compiletime
    rtcNow = RTC.now();
    String adjustedRtcNowTimestamp = rtcNow.timestamp();
    Serial.print("DEBUG: RTC clock timestamp adjusted to:");
    Serial.println(adjustedRtcNowTimestamp);
    return ntpEpoch;
  }

  uint32_t rtcEpoch = rtcNow.unixtime();
  Serial.print("DEBUG: epoch from NTP:");
  Serial.println(ntpEpoch);
  Serial.print("DEBUG: epoch from RTC:");
  Serial.println(rtcEpoch);
  return time_t(rtcEpoch);
}

boolean requireRtcTimeAdjustment(tm *localTime, DateTime rtcNow) {
  // According to this https://en.wikipedia.org/wiki/Time_Protocol
  // 07-02-2036 date can be ignored if it is coming from NTP source
  if (localTime->tm_year+1900 == 2036 && localTime->tm_mon + 1 == 2 && localTime->tm_mday == 7) {
    return false;
  }
  if (localTime->tm_year+1900 != rtcNow.year()) {
    Serial.print("DEBUG: NTP year not equal to RTC year: ");
    Serial.print("NTP year: ");
    Serial.print(localTime->tm_year+1900);
    Serial.print("RTC year: ");
    Serial.print(rtcNow.year());
    return true;
  }
  if (localTime->tm_mon + 1 != rtcNow.month()) {
    Serial.print("DEBUG: NTP month not equal to RTC month: ");
    Serial.print("NTP month: ");
    Serial.print(localTime->tm_mon+1);
    Serial.print("RTC month: ");
    Serial.print(rtcNow.month());
    return true;
  }
  if (localTime->tm_mday != rtcNow.day()) {
    Serial.print("DEBUG: NTP day not equal to RTC day: ");
    Serial.print("NTP day: ");
    Serial.print(localTime->tm_mday);
    Serial.print("RTC day: ");
    Serial.print(rtcNow.day());
    return true;
  }
  if (localTime->tm_mday != rtcNow.day()) {
    Serial.print("DEBUG: NTP hour not equal to RTC Hour: ");
    Serial.print("NTP Hour: ");
    Serial.print(localTime->tm_hour);
    Serial.print("RTC Hour: ");
    Serial.print(rtcNow.hour());
    return true;
  }
  if ((localTime->tm_min - rtcNow.minute()) > 2) {
    Serial.print("DEBUG: NTP and RTC differ by more than 2mins: ");
    Serial.print("NTP Min: ");
    Serial.print(localTime->tm_min);
    Serial.print("RTC Min: ");
    Serial.print(rtcNow.minute());
    return true;
  }
  return false;
}

boolean isEpochNTPSynced(time_t epoch)
{
  // 1651363200 -> Sunday, 1 May 2022 05:30:00 GMT+05:30
  return (ntpEpoch > 1651363200);
}

void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus)
{
  Serial.print("INFO: Writing PRES event to file occured at: ");
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
  Serial.print("INFO: Writing POFF event to file occured at: ");
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

// void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, time_t epoch, boolean ntpStatus)
// {
//   Serial.print("INFO: Writing PON event to file occured at: ");
//   Serial.println(timeOfEvent.c_str());
//   if (ntpStatus)
//   {
//     dateFile.print("1,");
//   }
//   else
//   {
//     dateFile.print("-,");
//   }
//   dateFile.print("PON,");
//   dateFile.print(timeOfEvent.c_str());
//   dateFile.print(",");
//   dateFile.println(epoch);
//   dateFile.flush();
// }

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
  Serial.println("INFO: Checking for unpublished events to be published");
  // check and update power status change - start
  updatePowerStatusIfChanged();
  // check and update power status change - end

  std::vector<std::string> sortedFilenames;
  Serial.println("DEBUG: Sorting directories in root");
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
  Serial.print("DEBUG: Evaluated start date: ");
  Serial.print(unPublishedStartDate.c_str());
  Serial.print(" , ");
  Serial.print(" line num: ");
  Serial.print(unPublishedLineNum.c_str());
  Serial.println(" for unpublished events");

  // check and update power status change - start
  updatePowerStatusIfChanged();
  // check and update power status change - end

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
        Serial.print("DEBUG: skipped already published file: ");
        Serial.println(pickedFile.c_str());

        std::string actualFileName = pickedFile.substr(pickedFile.rfind("/") + 1);
        std::string newPubFilePath = "/qop-published/" + actualFileName;
        boolean isRemoved = SD.remove(newPubFilePath.c_str());
        Serial.print("DEBUG: Deleting file on /qop-published directory before moving: ");
        Serial.print(newPubFilePath.c_str());
        Serial.print(", isRemoved: ");
        Serial.println(isRemoved);
        boolean isSuccess = SD.rename(pickedFile.c_str(), newPubFilePath.c_str());
        Serial.print("DEBUG: Moved published file to /qop-published directory: ");
        Serial.print(newPubFilePath.c_str());
        Serial.print(", isSuccessful: ");
        Serial.println(isSuccess);
        continue;
      }
    }
    Serial.print("DEBUG: Opening file for events to be published: ");
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
      if (unPublishedLineNum.empty() || pickedFile.compare(unPublishedStartDate) > 0 || (pickedFile.compare(unPublishedStartDate) == 0 && currLineNumber > std::stoi(unPublishedLineNum)))
      {
          // publish events
          Serial.println("DEBUG: ====== Publishable Event INFO ======");
          Serial.print("DEBUG: TimeOfEvent: ");
          Serial.println(timeOfEvent.c_str());
          Serial.print("DEBUG: EventName: ");
          Serial.println(eventName.c_str());
          Serial.print("DEBUG: isNTPSynced: ");
          Serial.println(ntpSynced);
          Serial.print("DEBUG: Epoch: ");
          Serial.println(epoch.c_str());
          Serial.println("DEBUG: ====== Publishable Event INFO ======");
          int epochInt = std::stoi(epoch);
          int pubStatus = 0;
          if (eventName == "POFF") {
            pubStatus = publishPowerOffEvent(epochInt);
          } else if (eventName == "PRES") {
            pubStatus = publishPowerOnEvent(epochInt);
          }
          
          if (pubStatus == 0)
          {
            std::string log = "";
            log.append("DEBUG: Failed to publish ");
            log.append(eventName);
            log.append(" event");
            Serial.println(log.c_str());
            openedFile.close();
            return;
          }

          std::string log = "";
          log.append("DEBUG: Successfully published ");
          log.append(eventName);
          log.append(" event");
          Serial.println(log.c_str());
          persistPubStatusToFile(pickedFile, std::to_string(currLineNumber));
      }
    }
    openedFile.close();
  }
}

void persistPubStatusToFile(std::string date, std::string lineNum)
{
  // TODO: Recoverable file write impl
  Serial.println("DEBUG: Persisting publish status to /qop.status file");
  SD.remove("qop.status.old");
  SD.rename("qop.status", "qop.status.old");
  File pubStatusFile = SD.open("qop.status", FILE_WRITE);
  pubStatusFile.print(date.c_str());
  pubStatusFile.print(",");
  pubStatusFile.println(lineNum.c_str());
  pubStatusFile.flush();
  pubStatusFile.close();
  Serial.println("DEBUG: Publish status persisted successfully");
}

std::vector<std::string> getPublishStatusContent()
{
  // TODO: Recoverable file read impl
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
  std::string newTweet = event + " [Time:" + epochCovertedTime + "]" + " [monotonic: " + std::to_string(millis()) + "]";
  std::string cleanedTweet = removeNewLines(reduceDoubleSpaces(newTweet));
  Serial.println(cleanedTweet.c_str());

  boolean val = tcr.tweet(cleanedTweet);
  Serial.print("INFO: Tweet publish status: ");
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
