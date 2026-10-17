#include <FS.h>
#include <SD.h>
#include <String.h>
#include <Arduino.h>
#include <vector>
#include "IncidentManager.h"


std::vector<IncidentMetadata> pendingIncidents;

/**
* Scan and populate the queue
*/
void queueIncidentFolders(const char* baseFolderPath) {
    File root = SD.open(baseFolderPath);
    if (!root || !root.isDirectory()) {
        if (Serial){
          Serial.println("Failed to open base folder for scanning.");
        }
        return;
    }

    File entry = root.openNextFile();
    while (entry) {
        if (entry.isDirectory()) {
            String dirName = String(entry.name());
            if (dirName.startsWith("incident")) {
                String fullPath = String(baseFolderPath) + "/" + dirName;
                String incidentNumStr = dirName.substring(8);
                uint32_t eventId = strtoul(incidentNumStr.c_str(), NULL, 10);

                IncidentMetadata meta = {
                    .eventId = eventId,
                    .folderPath = fullPath
                };
                pendingIncidents.push_back(meta);
                if (Serial){
                  Serial.printf("Queued incident folder: %s\n", fullPath.c_str());
                  Serial.printf("[BLE Debug] EventId: %lu", eventId);
                }
            }
        }
        entry.close();
        entry = root.openNextFile();
    }
    root.close();
    if (Serial){
      Serial.printf("Total incident folders queued: %u\n", pendingIncidents.size());
    }
}

/**
* Recursive helper to clear files before removing directory
*/
bool removeDirRecursive(fs::FS &fs, const char *path) {
    File root = fs.open(path);
    if (!root || !root.isDirectory()) {
        return false;
    }

    File file = root.openNextFile();
    while (file) {
        String filePath = String(path) + "/" + file.name();
        if (file.isDirectory()) {
            file.close();
            removeDirRecursive(fs, filePath.c_str());
        } else {
            file.close();
            fs.remove(filePath.c_str());
        }
        file = root.openNextFile();
    }
    root.close();
    
    return fs.rmdir(path);
}

/**
* Helper to build queue again to encompass new incidents
*/
void refreshList() {
  if(!pendingIncidents.empty()){
    pendingIncidents.clear();
  }
  queueIncidentFolders("/");
  if(pendingIncidents.empty()){ // attempting a new path to get it. Depends on SD for various ways that can work
    queueIncidentFolders("");
  }
  if(pendingIncidents.empty()) {
    queueIncidentFolders("./");
  }
}