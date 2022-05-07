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
File dataRoot;

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

File getLatestFileByDate(File rootDir, std::string date);
void publishUnpublishedEvents(File rootDir);
std::vector<std::string> listDirSorted(File rootDir);
void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, boolean ntpStatus);
void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, boolean ntpStatus);
std::string getFilenameFromEpoch(time_t epochTime);
std::string getTimeOfEventFromEpoch(time_t epochTime);
boolean isEpochNTPSynced(time_t epoch);
std::vector<std::string> getPublishStatusContent();
boolean doesStatusExist(std::vector<std::string> statusVec);
std::string getDateFromStatus(std::vector<std::string> statusVec);
std::string getLineNumFromStatus(std::vector<std::string> statusVec);
void persistPubStatusToFile(std::string date, std::string lineNum);

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

    dataRoot = SD.open("/qop");
    if (!dataRoot) {
      SD.mkdir("qop");
      dataRoot = SD.open("/qop");
    }

    root = SD.open("/");
    printDirectory(root, 0);
    Serial.println("done!");
}

void loop() {
    WiFiManager.loop();
    updater.loop();
    configManager.loop();
    // if (powerOnTimeInEpoch < 1651363200) {
    //     return;
    // }

    // if (publishCounter++ <= 1) {
    //     Serial.println("Publishing Tweet");
    //     publishTweet(powerOnTimeInEpoch);
    // }

    std::string datedFilename = getFilenameFromEpoch(powerOnTimeInEpoch);

    Serial.println("getting latest file by date");
    File dateFile = getLatestFileByDate(dataRoot, datedFilename);
    if (!dateFile) {
      Serial.println("unable to open latest file for writing");
      return;
    }
    // wait for given time and write event to file
    std::string timeOfEvent = getTimeOfEventFromEpoch(powerOnTimeInEpoch);
    writePowerResumeEventToFile(dateFile, timeOfEvent, isEpochNTPSynced(powerOnTimeInEpoch));
    while (true) {
      delay(60000);
      time_t newEpochTime = tcr.getEpoch();
      std::string newTimeOfEvent = getTimeOfEventFromEpoch(newEpochTime);
      writePowerOnEventToFile(dateFile, newTimeOfEvent, isEpochNTPSynced(newEpochTime));
      // if date changes create a new file

      // read last unpublished events and publish them
      // update the file with publish status
      Serial.println("publishing unpublished events");
      publishUnpublishedEvents(dataRoot);
    }
    dateFile.close();
}

boolean isEpochNTPSynced(time_t epoch) {
    return (powerOnTimeInEpoch > 1651363200);
}

void writePowerResumeEventToFile(File dateFile, std::string timeOfEvent, boolean ntpStatus) {
    Serial.print("timeOfEvent: ");
    Serial.println(timeOfEvent.c_str());
    if (ntpStatus) {
      dateFile.print("1,");
    } else {
      dateFile.print("-,");
    }
    dateFile.print("PRES,");
    dateFile.println(timeOfEvent.c_str());
    dateFile.flush();
}

void writePowerOnEventToFile(File dateFile, std::string timeOfEvent, boolean ntpStatus) {
    Serial.print("timeOfEvent: ");
    Serial.println(timeOfEvent.c_str());
    if (ntpStatus) {
      dateFile.print("1,");
    } else {
      dateFile.print("-,");
    }
    dateFile.print("PON,");
    dateFile.println(timeOfEvent.c_str());
    dateFile.flush();
}

std::string getFilenameFromEpoch(time_t epochTime) {
    tm* localTime = std::localtime(&epochTime);
    char dateBuf[9];
    sprintf(dateBuf, "%d%02d%02d", 1900+(localTime->tm_year), (localTime->tm_mon)+1, localTime->tm_mday);
    return dateBuf;
}

std::string getTimeOfEventFromEpoch(time_t epochTime) {
    tm* localTime = std::localtime(&epochTime);
    char timeOfEvent[9];
    sprintf(timeOfEvent, "%02d:%02d:%02d", localTime->tm_hour, localTime->tm_min, localTime->tm_sec);
    return timeOfEvent;
}

File getLatestFileByDate(File rootDir, std::string date) {
  // std::vector<std::string> filenames;
  while (File nextFile = rootDir.openNextFile()) {
    if (!nextFile) {
          // no more files
          break;
    }
    if (date == nextFile.fullName()) {
      Serial.print("reading existing latest file by date: ");
      Serial.println(nextFile.fullName());
      return nextFile;
    }
    nextFile.close();
  }
  std::string fullname = rootDir.fullName();
  Serial.print("latest file by date:");
  Serial.println((fullname + "/" + date).c_str());
  return SD.open((fullname + "/" + date).c_str(), FILE_WRITE);
}

void publishUnpublishedEvents(File rootDir) {
  // read files, sort by date
  std::vector<std::string> sortedFilenames;
  Serial.println("started sorting directories");
  sortedFilenames = listDirSorted(rootDir);
  if (sortedFilenames.size() < 1) {
    return;
  }

  std::string unPublishedStartDate;
  std::string unPublishedLineNum;
  std::vector<std::string> pubStatus = getPublishStatusContent();
  if (doesStatusExist(pubStatus)) {
    unPublishedStartDate = getDateFromStatus(pubStatus);
    unPublishedLineNum = getLineNumFromStatus(pubStatus);
  }

  Serial.println("processing sorted file names");
  // pick the lowest date file
  for (int i =0; i<sortedFilenames.size();i++) {
    std::string pickedFile = sortedFilenames.at(i);
    if (!unPublishedStartDate.empty() && !unPublishedLineNum.empty()) {
      if (pickedFile.c_str() != unPublishedStartDate.c_str()) {
        // TODO: Move this file to published dir /qop-published/
        continue;
      }
    }
    Serial.print("opening file for read: ");
    Serial.println(pickedFile.c_str());
    File openedFile = SD.open(pickedFile.c_str(), FILE_READ);
    String line;
    std::string prevEventName;
    std::string prevTimeOfEvent;
    std::string prevNtpSynced;
    boolean publishError;
    int currLineNumber = 0;
    while(true) {
      line = openedFile.readStringUntil('\n');
      Serial.print("line read: ");
      Serial.println(line);
      if (line.isEmpty()) {
        break;
      }
      currLineNumber++;
      if (!unPublishedLineNum.empty()) {
        if (currLineNumber < std::stoi(unPublishedLineNum)) {
          continue;
        }
      }

      boolean ntpSynced = false;
      std::string timeOfEvent;
      std::string eventName;
      Serial.println("string tokenisation");
      char *token = std::strtok((char*)line.c_str(), ",");
      Serial.print("token");
      Serial.println(token);

      int idx = 0;
      while (token != NULL && idx <= 2) {
        Serial.print("idx: ");
        Serial.println(idx);
        switch (idx++)
        {
        case 0:
          if (token == "-") { // NTP synced time
            ntpSynced = true;
          }
          break;
        case 1:
          eventName = token;
          break;
        case 2:
          if (token != "") { // Event time stamp
            timeOfEvent = token;
          }
          break;
        default:
          break;
        }
        token = std::strtok(NULL, ",");
      }
      Serial.print("timeOfEvent: ");
      Serial.println(timeOfEvent.c_str());
      Serial.print("eventName: ");
      Serial.println(eventName.c_str());
      Serial.print("ntpSynced: ");
      Serial.println(ntpSynced);
      if (unPublishedLineNum.empty() || currLineNumber > std::stoi(unPublishedLineNum)) {
        if (eventName == "PRES") {
          if (prevEventName == "PON" || prevEventName == "PRES") {
            // publish power off event first
            Serial.println("publishing power off event");
            
            persistPubStatusToFile(pickedFile, std::to_string(currLineNumber-1));
          }
          Serial.println("publishing power resumed event");
          persistPubStatusToFile(pickedFile, std::to_string(currLineNumber));
          // publish power resumed event
        }
      }

      prevEventName = eventName;
      prevTimeOfEvent = timeOfEvent;
      prevNtpSynced = ntpSynced;
        //    check for unpublished events
        //    publish event
    };
    openedFile.close();
    if (!publishError) {
      //    mark the file as complete  
    }
    // stop when all files are exhausted
  }
}

void persistPubStatusToFile(std::string date, std::string lineNum) {
  // Do recoverable file write
  Serial.println("persisting pub status");
  SD.remove("qop.status");
  File pubStatusFile = SD.open("qop.status", FILE_WRITE);
  pubStatusFile.print(date.c_str());
  pubStatusFile.print(",");
  pubStatusFile.println(lineNum.c_str());
  pubStatusFile.flush();
  pubStatusFile.close();
  Serial.println("pub status persisted");
}

std::vector<std::string> getPublishStatusContent() {
  std::vector<std::string> pubStatus;
  File pubStatusFile = SD.open("qop.status", FILE_READ); 
  String status = pubStatusFile.readStringUntil('\n');
  pubStatusFile.close();
  if (status == "") {
    return pubStatus;
  }
  char *token = std::strtok((char *)status.c_str(), ",");
  pubStatus.push_back(token);
  token = std::strtok(NULL, ",");
  pubStatus.push_back(token);
  return pubStatus;
}

boolean doesStatusExist(std::vector<std::string> statusVec) {
  return statusVec.size() != 0;
}

std::string getDateFromStatus(std::vector<std::string> statusVec) {
  return statusVec.at(0);
}

std::string getLineNumFromStatus(std::vector<std::string> statusVec) {
  return statusVec.at(1);
}

std::vector<std::string> listDirSorted(File rootDir) {
  std::vector<std::string> filenames;
  while (true) {
    File pickedFile = rootDir.openNextFile();
    if (!pickedFile) {
      break;
    }
    Serial.print("file name for sort: ");
    Serial.println(pickedFile.fullName());
    filenames.push_back(pickedFile.fullName());
  }
  std::sort(filenames.begin(), filenames.end());
  Serial.print("finished file sort of size: ");
  Serial.println(filenames.size());
  return filenames;
}

void publishTweet(time_t epoch) {
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
