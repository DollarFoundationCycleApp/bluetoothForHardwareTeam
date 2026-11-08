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

extern void serial_print(const char* str); // update this before prod

#define ESP_LOGI(tag, format, ...) { char buf[360]; snprintf(buf, sizeof(buf), "[INFO] " format, ##__VA_ARGS__); serial_print(buf); }
#define ESP_LOGW(tag, format, ...) { char buf[360]; snprintf(buf, sizeof(buf), "[WARN] " format, ##__VA_ARGS__); serial_print(buf); }
#define ESP_LOGE(tag, format, ...) { char buf[360]; snprintf(buf, sizeof(buf), "[ERROR] " format, ##__VA_ARGS__); serial_print(buf); }

/**
 * Helper function to securely build child paths without buffer overflow or truncation
 */
static uint8_t build_child_path(char *dest, size_t dest_size, const char *parent, const char *child) {
  size_t parent_len = strlen(parent);
  size_t child_len = strlen(child);
  size_t prefix_len = parent_len;
  size_t separator_len = parent_len > 0 && parent[parent_len - 1] != '/' ? 1 : 0;

  if (parent_len == 0) {
    dest[0] = '/';
    prefix_len = 1;
  } else if (parent_len == 1 && parent[0] == '/') {
    prefix_len = 1;
  }

  if (prefix_len + separator_len + child_len + 1 > dest_size) {
    return FALSE;
  }

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
 * Internal helper to safely insert or queue an incident node
 */
static void add_incident_to_list(uint32_t eventId, const char* fullPath) {
  if (incidentMutex && xSemaphoreTake(incidentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
      IncidentNode* newNode = (IncidentNode*)malloc(sizeof(IncidentNode));
      if (newNode != NULL) {
        newNode->eventId = eventId;
        snprintf(newNode->folderPath, MAX_PATH_LEN, "%s", fullPath);
        newNode->active = TRUE;
        
        newNode->next = pendingIncidentsHead;
        pendingIncidentsHead = newNode;

        ESP_LOGI(TAG, "Queued incident folder: %s [EventId: %lu]", fullPath, eventId);
      } else {
        ESP_LOGE(TAG, "Malloc failed for new incident node!");
      }
    }
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
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    // Match folders starting with "incident" safely using strncasecmp
    if (strncasecmp(entry->d_name, "incident", 8) == 0) {
      char fullPath[MAX_PATH_LEN];
      if (build_child_path(fullPath, sizeof(fullPath), baseFolderPath, entry->d_name)) {
        
        DIR *subDir = opendir(fullPath);
        if (subDir != NULL) {
          closedir(subDir);
          
          // Parse the trailing digits
          uint32_t eventId = (uint32_t)strtoul(entry->d_name + 8, NULL, 10);
          add_incident_to_list(eventId, fullPath);
        }
      }
    }
  }
  closedir(dir);
  ESP_LOGI(TAG, "Scan complete for %s. Total count: %d", baseFolderPath, get_pending_incident_count());
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
      free(current);
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
    if (!build_child_path(filePath, sizeof(filePath), path, entry->d_name)) {
      ESP_LOGE(TAG, "Path is too long under directory");
      success = FALSE;
      continue;
    }

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