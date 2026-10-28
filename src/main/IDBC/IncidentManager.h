/* // arduino version
#ifndef INCIDENT_MANAGER_H
#define INCIDENT_MANAGER_H
#if defined(__cplusplus)
extern "C" {
#endif
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <vector>
#include <FS.h>
#include <SD.h>

// 1. Define the struct where both files can see it
struct IncidentMetadata {
    unsigned long eventId;
    String folderPath;
};

extern SemaphoreHandle_t incidentMutex;

// 2. Declare the global vector extern so it's shared safely across files
extern std::vector<IncidentMetadata> pendingIncidents;

// 3. Declare helper prototypes so other files know they exist
void queueIncidentFolders(const char* baseFolderPath);
uint8_t removeDirRecursive(fs::FS &fs, const char *path);
void refreshList();
const char* find_target_path(uint32_t event_id);

#endif  // */

#ifndef INCIDENT_MANAGER_H
#define INCIDENT_MANAGER_H

#if defined(__cplusplus)
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_PENDING_INCIDENTS 16
#define MAX_PATH_LEN          64

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

// TODO strucutre for pending Incidents I called it IncidentNode, but it can be something else.


// Shared Globals
extern SemaphoreHandle_t incidentMutex;
extern IncidentNode* pendingIncidentsHead;

// Function Prototypes
void queue_incident_folders(const char* baseFolderPath);
uint8_t remove_dir_recursive(const char *path);
void refresh_list(void);
const char* find_target_path(uint32_t event_id);
uint8_t remove_incident_node(uint32_t event_id);
int get_pending_incident_count(void);

#if defined(__cplusplus)
}
#endif

#endif