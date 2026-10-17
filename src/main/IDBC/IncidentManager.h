#ifndef INCIDENT_MANAGER_H
#define INCIDENT_MANAGER_H

#include <Arduino.h>
#include <vector>
#include <FS.h>
#include <SD.h>

// 1. Define the struct where both files can see it
struct IncidentMetadata {
    unsigned long eventId;
    String folderPath;
};

// 2. Declare the global vector extern so it's shared safely across files
extern std::vector<IncidentMetadata> pendingIncidents;

// 3. Declare helper prototypes so other files know they exist
void queueIncidentFolders(const char* baseFolderPath);
bool removeDirRecursive(fs::FS &fs, const char *path);

#endif