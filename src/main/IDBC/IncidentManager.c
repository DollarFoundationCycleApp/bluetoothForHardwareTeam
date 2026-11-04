#include "IncidentManager.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"

#define TAG "INCIDENT_MANAGER"

// Global Linked List Head Pointer
IncidentNode* pendingIncidentsHead = NULL;

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
  int countAdded = 0;

  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type == DT_DIR && strncmp(entry->d_name, "incident", 8) == 0) {
      char fullPath[MAX_PATH_LEN];
      if (strcmp(baseFolderPath, "/") == 0 || strlen(baseFolderPath) == 0) {
        snprintf(fullPath, sizeof(fullPath), "/%s", entry->d_name);
      } else {
        if (baseFolderPath[strlen(baseFolderPath) - 1] == '/') {
          snprintf(fullPath, sizeof(fullPath), "%s%s", baseFolderPath, entry->d_name);
        } else {
          snprintf(fullPath, sizeof(fullPath), "%s/%s", baseFolderPath, entry->d_name);
        }
      }
      
      uint32_t eventId = (uint32_t)strtoul(entry->d_name + 8, NULL, 10);

      if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // Check if already in the linked list
        uint8_t alreadyExists = FALSE;
        IncidentNode* current = pendingIncidentsHead;
        while (current != NULL) {
          if (current->eventId == eventId) {
            alreadyExists = TRUE;
            break;
          }
          current = current->next;
        }

        if (alreadyExists == FALSE) {
          // Allocate a new node on the heap (low memory, exact fit)
          IncidentNode* newNode = (IncidentNode*)malloc(sizeof(IncidentNode));
          if (newNode != NULL) {
            newNode->eventId = eventId;
            snprintf(newNode->folderPath, MAX_PATH_LEN, "%s", fullPath);
            
            // Prepend for quick O(1) insertion
            newNode->next = pendingIncidentsHead;
            pendingIncidentsHead = newNode;

            countAdded++;
            ESP_LOGI(TAG, "Queued incident folder: %s [EventId: %lu]", fullPath, eventId);
          } else {
            ESP_LOGE(TAG, "[ERROR] Malloc failed for new incident node!");
          }
        }
        xSemaphoreGive(incidentMutex);
      }
    }
  }
  closedir(dir);
  ESP_LOGI(TAG, "Scan complete. Added this pass. Total count: %d", get_pending_incident_count());
}

/**
* Removes and frees a specific node from the linked list by Event ID (used during ACK cleanup)
*/
uint8_t remove_incident_node(uint32_t event_id) {
  uint8_t removed = FALSE;
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
      prev = current;
      current = current->next;
    }
    xSemaphoreGive(incidentMutex);
  }
  return removed;
}

/**
* Helper to clear and free the entire linked list (used during refresh)
*/
void refresh_list(void) {
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    while (current != NULL) {
      IncidentNode* nextNode = current->next;
      free(current); // Release each node's memory
      current = nextNode;
    }
    pendingIncidentsHead = NULL;
    xSemaphoreGive(incidentMutex);
  }
  
  queue_incident_folders("/");
  vTaskDelay(pdMS_TO_TICKS(10));
  
  if (get_pending_incident_count() == 0) {
    queue_incident_folders("./");
  }
  if (get_pending_incident_count() == 0) {
    queue_incident_folders("");
  }
}

/**
* Helper to find target folder path by event ID from the linked list
*/
const char* find_target_path(uint32_t event_id) {
  static char targetBuffer[MAX_PATH_LEN];
  targetBuffer[0] = '\0';

  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    while (current != NULL) {
      if (current->eventId == event_id) {
        snprintf(targetBuffer, sizeof(targetBuffer), "%s", current->folderPath);
        break;
      }
      current = current->next;
    }
    xSemaphoreGive(incidentMutex);
  }
  return targetBuffer;
}

/**
* Helper to get total number of items currently in the linked list
*/
int get_pending_incident_count(void) {
  int count = 0;
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    IncidentNode* current = pendingIncidentsHead;
    while (current != NULL) {
      count++;
      current = current->next;
    }
    xSemaphoreGive(incidentMutex);
  }
  return count;
}

/**
* POSIX Recursive helper to clear files before removing directory
*/
uint8_t remove_dir_recursive(const char *path) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return FALSE;
  }

  struct dirent *entry;
  char filePath[MAX_PATH_LEN];
  uint8_t success = TRUE;

  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    snprintf(filePath, sizeof(filePath), "%s/%s", path, entry->d_name);

    struct stat st;
    if (stat(filePath, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        if (remove_dir_recursive(filePath) == FALSE) {
          success = FALSE;
        }
      } else {
        if (unlink(filePath) != 0) {
          ESP_LOGE(TAG, "Failed to unlink file: %s", filePath);
          success = FALSE;
        }
      }
    }
  }
  closedir(dir);

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
    if (!filename || !out_ms) return FALSE;

    // Look for the "ms.jpg" suffix
    const char *msPtr = strstr(filename, "ms.jpg");
    if (!msPtr) {
        return FALSE;
    }

    // Walk backwards from "ms" to isolate the numeric characters
    const char *p = msPtr - 1;
    while (p >= filename && *p >= '0' && *p <= '9') {
        p--;
    }
    
    // If no digits were found right before "ms"
    if (p == msPtr - 1) {
        return FALSE;
    }

    *out_ms = (uint32_t)strtoul(p + 1, NULL, 10);
    return TRUE;
}

/**
 * Reads an incident's data.csv file line-by-line to match the timestamp and pull sensor data.
 */
 uint8_t match_timestamp_in_csv(const char *incident_folder_path, uint32_t target_ms, float *out_distance) {
  char csvPath[MAX_PATH_LEN];
  snprintf(csvPath, sizeof(csvPath), "%s/data.csv", incident_folder_path);

  FILE *file = fopen(csvPath, "r");
  if (file == NULL) {
    ESP_LOGW(TAG, "Failed to open CSV file: %s", csvPath);
    return FALSE;
  }

  char lineBuffer[64];
  uint8_t found = false;

  while (fgets(lineBuffer, sizeof(lineBuffer), file) != NULL) {
    uint32_t rowTimestamp = 0;
    float rowDistance = 0.0f;

    if (sscanf(lineBuffer, "%lu,%f", &rowTimestamp, &rowDistance) == 2) {
      if (rowTimestamp == target_ms) {
        *out_distance = rowDistance;
        found = TRUE;
        break;
      }
    }
  }
  fclose(file);
  return found;
 }

/**
 * Scans an incident folder using POSIX VFS to parse image timestamps 
 * and match them against the incident's meta information.
 */
uint8_t parse_incident_metadata(const char *incidentFolderPath) {
    char csvPath[MAX_PATH_LEN];
    snprintf(csvPath, sizeof(csvPath), "%s/data.csv", incidentFolderPath);

    DIR *dir = opendir(incidentFolderPath);
    if (dir == NULL) {
        ESP_LOGW(TAG, "Failed to open incident folder: %s", incidentFolderPath);
        return FALSE;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Check if the file is a JPEG image
        if (entry->d_type == DT_REG && strstr(entry->d_name, ".jpg") != NULL) {
            uint32_t imgMs = 0;
            if (get_ms_from_image_title(entry->d_name, &imgMs)) {
                ESP_LOGI(TAG, "Found image: %s -> Extracted Timestamp: %lu ms", entry->d_name, imgMs);
                
                // TODO: From here open data.csv and match rowTimestamp == imgMs
            }
        }
    }
    closedir(dir);
    return TRUE;
}