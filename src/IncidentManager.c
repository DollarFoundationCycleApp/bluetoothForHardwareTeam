#include "IncidentManager.h"
#include "esp_log.h"

#define TAG "INCIDENT_MANAGER"
#define CSV_MATCH_TOLERANCE_MS 100   // rows are ~20-55 ms apart
extern void serial_print(const char* str);

// Global Linked List Head Pointer
IncidentNode* pendingIncidentsHead = NULL;

extern void serial_print(const char* str); // update this before prod


/**
 * Helper function to securely build child paths without buffer overflow or truncation
 */
static uint8_t build_child_path(char *dest, size_t dest_size, const char *parent, const char *child) {

  size_t parent_len = strlen(parent);
  size_t child_len = strlen(child);
  size_t prefix_len = parent_len;
  size_t separator_len = parent_len > 0 && parent[parent_len - 1] != '/' ? 1 : 0;

  // handle the different ways root is handled based on lang and library
  if (parent_len == 0) {
    dest[0] = '/';
    prefix_len = 1;
  } else if (parent_len == 1 && parent[0] == '/') {
    prefix_len = 1;
  }

  // gaurd against buffer overflow (needed for PlatformIO)
  if (prefix_len + separator_len + child_len + 1 > dest_size) {
    return FALSE;
  }

  // copy parts that matter into where we need them
  if (parent_len > 0) {
    memcpy(dest, parent, prefix_len);
  }
  if (separator_len > 0) {
    dest[prefix_len++] = '/';
  }
  memcpy(dest + prefix_len, child, child_len + 1);
  return TRUE;
}

/**
 * Internal helper to safely insert or queue an incident node with a cached image count
 */
static void add_incident_to_list(uint32_t eventId, const char* fullPath, uint32_t imageCount) {
  // try to grab the mutex, but 50ms timeout to prevent deadlock
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    uint8_t alreadyExists = FALSE;
    IncidentNode* current = pendingIncidentsHead;

    // check for duplicate before allocating a new node
    while (current != NULL) {
      if (current->eventId == eventId) {
        alreadyExists = TRUE;
        break;
      }
      current = current->next;
    }

    if (alreadyExists == FALSE) {
      IncidentNode* newNode = (IncidentNode*)malloc(sizeof(IncidentNode));
      if (newNode != NULL) {
        newNode->eventId = eventId;
        snprintf(newNode->folderPath, MAX_PATH_LEN, "%s", fullPath);
        newNode->imageCount = imageCount;
        newNode->active = TRUE;
        
        //prepend to the linked list for simplicity (FIFO order is not critical here)
        newNode->next = pendingIncidentsHead;
        pendingIncidentsHead = newNode;

        // log it for visibility
        ESP_LOGI(TAG, "Queued incident folder: %s [EventId: %lu, Images: %lu]", fullPath, eventId, (unsigned long)imageCount);
      } else {
        // log failure for development
        ESP_LOGE(TAG, "Malloc failed for new incident node!");
      }
    }
    // release mutex
    xSemaphoreGive(incidentMutex);
  }
}

/**
 * Scan and populate the linked list queue from SD card using POSIX VFS
 */
void queue_incident_folders(const char* baseFolderPath) {
  DIR *dir = opendir(baseFolderPath);
  if (dir == NULL) {
    ESP_LOGW(TAG, "Failed to open base folder for scanning: %s", baseFolderPath);
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    // skip current and parent dir pointers
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    // Match folders starting with "incident" safely using strncasecmp
    // todo rename to what we actually need. Might want it as a macro
    if (strncasecmp(entry->d_name, "incident", 8) == 0) {
      char fullPath[MAX_PATH_LEN];
      if (build_child_path(fullPath, sizeof(fullPath), baseFolderPath, entry->d_name)) {
        
        DIR *subDir = opendir(fullPath);
        if (subDir != NULL) {
          // Count images once during startup scan (avoiding runtime SD locks)
          uint32_t imgCount = 0;
          struct dirent *subEntry;
          while ((subEntry = readdir(subDir)) != NULL) {
            if (subEntry->d_type == DT_REG) {
              if (strstr(subEntry->d_name, ".jpg") != NULL || strstr(subEntry->d_name, ".png") != NULL) {
                imgCount++;
              }
            }
          }
          closedir(subDir);
          
          // Parse the trailing digits for event id #
          uint32_t eventId = (uint32_t)strtoul(entry->d_name + 8, NULL, 10);
          add_incident_to_list(eventId, fullPath, imgCount);
        }
      }
    }
  }
  // free resources
  closedir(dir);
  // log success
  ESP_LOGI(TAG, "Scan complete for %s. Total count: %d", baseFolderPath, get_pending_incident_count());
}

/**
 * Removes and frees a specific node from the linked list by Event ID (used during ACK cleanup)
 */
uint8_t remove_incident_node(uint32_t event_id) {
  uint8_t removed = FALSE;
  // grab mutex, 100 ms timeout to prevent deadlocks
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    IncidentNode* prev = NULL;

    while (current != NULL) {
      if (current->eventId == event_id) {
        if (prev == NULL) {
          // Removing the head node
          pendingIncidentsHead = current->next;
        } else {
          // Removing a middle or tail node
          prev->next = current->next;
        }
        free(current); // Free heap memory back to system
        removed = TRUE;
        break;
      }
      // move the list along
      prev = current;
      current = current->next;
    }
    //release mutex
    xSemaphoreGive(incidentMutex);
  }
  return removed;
}

/**
* Helper to clear and free the entire linked list (used during refresh)
*/
void refresh_list(void) {
  // gather the mutex
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    // clear all of the incidents from the list
    while (current != NULL) {
      IncidentNode* nextNode = current->next;
      free(current);
      current = nextNode;
    }
    pendingIncidentsHead = NULL;
    // free the mutex
    xSemaphoreGive(incidentMutex);
  }
  
  //attempt to queue in all the forms we've found for the root of sd card.
  queue_incident_folders("/sd");
  vTaskDelay(pdMS_TO_TICKS(10));
  
  if (get_pending_incident_count() == 0) {
    queue_incident_folders("./");
  }
  if (get_pending_incident_count() == 0) {
    queue_incident_folders("");
  }
  if (get_pending_incident_count() == 0) {
    queue_incident_folders("/");
  }
}

/**
* Helper to find target folder path by event ID from the linked list
*/
const char* find_target_path(uint32_t event_id) {
  // prep the buffer
  static char targetBuffer[MAX_PATH_LEN];
  targetBuffer[0] = '\0';

  // snag the mutex so it doesn't get deleted in case
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    // search for matching id
    while (current != NULL) {
      if (current->eventId == event_id) {
        snprintf(targetBuffer, sizeof(targetBuffer), "%s", current->folderPath);
        break;
      }
      current = current->next;
    }
    // release the mutex
    xSemaphoreGive(incidentMutex);
  }
  // send back the path
  return targetBuffer;
}


/**
* Helper to get total number of items currently in the linked list
*/
int get_pending_incident_count(void) {
  int count = 0;
  // grab mutex so things don't randomly change
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    // counts
    while (current != NULL) {
      count++;
      current = current->next;
    }
    xSemaphoreGive(incidentMutex);
  }
  return count;
}

/**
* Recursive helper to clear files before removing directory
*/
uint8_t remove_dir_recursive(const char *path) {
  // attempt to open path
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return FALSE;
  }

  struct dirent *entry;
  char filePath[MAX_PATH_LEN];
  uint8_t success = TRUE;

  // check the path
  while ((entry = readdir(dir)) != NULL) {

    // safety check
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (!build_child_path(filePath, sizeof(filePath), path, entry->d_name)) {
      ESP_LOGE(TAG, "Path is too long under directory");
      success = FALSE;
      continue;
    }

    struct stat st;
    // ok now we get into removing
    if (stat(filePath, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {

        // try to recurse through and deletesub directories
        if (remove_dir_recursive(filePath) == FALSE) {
          success = FALSE;
        }
      } else {
        // remove individual files
        if (unlink(filePath) != 0) {
          ESP_LOGE(TAG, "Failed to unlink file: %s", filePath);
          success = FALSE;
        }
      }
    }
  }
  // safely clear out
  closedir(dir);

  // report errors if needed but otherwise delete emptied directory
  if (rmdir(path) != 0) {
    ESP_LOGE(TAG, "Failed to remove directory: %s", path);
    success = FALSE;
  }

  return success;
}

/**
 * Extracts the millisecond timestamp from an image title string.
 * Supports formats like: image-4141ms.jpg, image-start-4141ms.jpg, image-approach-4141ms.jpg
 */

uint8_t get_ms_from_image_title(const char *filename, uint32_t *out_ms) {
    if (filename == NULL || out_ms == NULL) {
        return FALSE;
    }

    // Locate the timestamp suffix.
    const char *msPtr = strstr(filename, "ms.jpg");
    if (msPtr == NULL || strcmp(msPtr, "ms.jpg") != 0) {
        return FALSE;
    }

    // Find the beginning of the digits without moving
    // outside the filename buffer.
    const char *digitsStart = msPtr;

    while (digitsStart > filename &&
           digitsStart[-1] >= '0' &&
           digitsStart[-1] <= '9') {
        digitsStart--;
    }

    // Make sure at least one digit was found.
    if (digitsStart == msPtr) {
        return FALSE;
    }

    // Convert the digits into a uint32_t.
    uint32_t timestamp = 0;

    for (const char *p = digitsStart; p < msPtr; p++) {
        uint32_t digit = (uint32_t)(*p - '0');

        // Prevent integer overflow.
        if (timestamp > (UINT32_MAX - digit) / 10U) {
            return FALSE;
        }

        timestamp = timestamp * 10U + digit;
    }

    *out_ms = timestamp;
    return TRUE;
}


/**
 * Reads an incident's data.csv and returns the distance from the row whose
 * timestamp is closest to target_ms (within CSV_MATCH_TOLERANCE_MS).
 */
uint8_t match_timestamp_in_csv(const char *incident_folder_path, uint32_t target_ms, float *out_distance) {
  if (incident_folder_path == NULL || out_distance == NULL) {
    return FALSE;
  }

  // find the csv
  char csvPath[MAX_PATH_LEN];
  snprintf(csvPath, sizeof(csvPath), "%s/data.csv", incident_folder_path);

  //open csv
  FILE *file = fopen(csvPath, "r");
  if (file == NULL) {
    ESP_LOGW(TAG, "Failed to open CSV file: %s", csvPath);
    return FALSE;
  }

  // find the line
  char lineBuffer[64];
  uint8_t found = FALSE;
  uint32_t bestDiff = UINT32_MAX;

  // get the line or run out of lines
  while (fgets(lineBuffer, sizeof(lineBuffer), file) != NULL) {
    uint32_t rowTs = 0;
    float rowDist = 0.0f;

    // scan for what we're looking for
    if (sscanf(lineBuffer, "%" SCNu32 ",%f", &rowTs, &rowDist) != 2) continue;

    // calculate difference
    uint32_t diff = (rowTs > target_ms) ? (rowTs - target_ms) : (target_ms - rowTs);
    if (diff < bestDiff) {
      bestDiff = diff;
      *out_distance = rowDist;
      found = TRUE;
    } else if (rowTs > target_ms) {
      break;  // rows are in time order, so it only gets worse from here
    }
  }
  fclose(file);
  // return true if we found a match within tolerance, false otherwise
  return (found && bestDiff <= CSV_MATCH_TOLERANCE_MS) ? TRUE : FALSE;
}

/*
 * Helper for scanning directory.
 */
void scan_incident_directory(const char *incidentFolderPath) {
  // open directory
  DIR *dir = opendir(incidentFolderPath);
  if (dir == NULL) {
    serial_print("scan_incident_directory: failed to open folder");
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type != DT_REG || strstr(entry->d_name, ".jpg") == NULL) continue;

    //attempt to get image ms 
    uint32_t imgMs = 0;
    if (!get_ms_from_image_title(entry->d_name, &imgMs)) continue;

    float distInch = 0.0f;
    char msg[512]; // Increased buffer size to prevent format truncation warnings
    // match it and report back to logs
    if (match_timestamp_in_csv(incidentFolderPath, imgMs, &distInch)) { // handles saving
      snprintf(msg, sizeof(msg), "%s -> %lu ms, %.2f in", entry->d_name, (unsigned long)imgMs, distInch);
    } else {
      snprintf(msg, sizeof(msg), "%s -> %lu ms, no CSV match", entry->d_name, (unsigned long)imgMs);
    }
    serial_print(msg);
  }
  // bail from directory
  closedir(dir);
}

/**
 * Scans an incident folder using POSIX VFS to parse image timestamps 
 * and match them against the incident's meta information.
 */
uint8_t parse_incident_metadata(const char *incidentFolderPath) {
    if (incidentFolderPath == NULL) {
        serial_print("Folder path is NULL");
        return FALSE;
    }

    scan_incident_directory(incidentFolderPath);
    return TRUE;
}

/**
 * Picks the incident's approach image (or the start image if there isn't one),
 * reads its ms from the filename, and looks up the distance at that ms
 * in data.csv. Returns the distance in cm and the image's ms.
 */
uint8_t get_incident_event_metadata(const char *incident_folder_path, uint32_t *out_distance_cm, uint32_t *out_ms) {
  // good safety check
  if (incident_folder_path == NULL || out_distance_cm == NULL || out_ms == NULL) {
    return FALSE;
  }

  // get to where we need to be
  DIR *dir = opendir(incident_folder_path);
  if (dir == NULL) {
    ESP_LOGW(TAG, "Failed to open incident folder: %s", incident_folder_path);
    return FALSE;
  }

  // setup up metadata support
  uint8_t haveApproach = FALSE, haveStart = FALSE;
  uint32_t approachMs = 0, startMs = 0;
  struct dirent *entry;

  // parse through the data
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type != DT_REG) continue;

    //timestamp
    uint32_t ms = 0;
    if (!get_ms_from_image_title(entry->d_name, &ms)) continue;

    // different images that we know about currently
    if (strncmp(entry->d_name, "image-approach-", 15) == 0) {
      approachMs = ms;
      haveApproach = TRUE;
    } else if (strncmp(entry->d_name, "image-start-", 12) == 0) {
      startMs = ms;
      haveStart = TRUE;
    }
  }
  closedir(dir); // close the directory

  // parse ms elements?
  uint32_t eventMs;
  if (haveApproach) {
    eventMs = approachMs;
  } else if (haveStart) {
    eventMs = startMs;
  } else {
    return FALSE;
  }

  // math yimestamp and get distance
  float distInch = 0.0f;
  if (!match_timestamp_in_csv(incident_folder_path, eventMs, &distInch)) {
    return FALSE;
  }

  // update meta data
  *out_distance_cm = (uint32_t)(distInch * 2.54f + 0.5f);
  *out_ms = eventMs;
  return TRUE;
}