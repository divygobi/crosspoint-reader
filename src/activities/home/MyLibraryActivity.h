#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "RecentBooksStore.h"
#include "util/ButtonNavigator.h"

struct BookGridEntry {
  std::string title;
  std::string coverBmpPath;
  bool loaded = false;
};

class MyLibraryActivity final : public Activity {
 private:
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  ButtonNavigator buttonNavigator;

  size_t selectorIndex = 0;
  bool updateRequired = false;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;

  // Grid mode state
  bool isGridMode = false;
  std::vector<BookGridEntry> gridEntries;

  // Per-page lazy loading
  int currentPage = -1;
  bool pageCoversLoaded = false;
  size_t pageLoadIndex = 0;
  int cachedItemsPerPage = 0;

  // Frame buffer cache (same pattern as HomeActivity)
  uint8_t* coverBuffer = nullptr;
  bool coverBufferStored = false;
  int cachedPage = -1;

  // Callbacks
  const std::function<void(const std::string& path)> onSelectBook;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render();
  void renderList() const;
  void renderGrid();

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

  // Grid helpers
  bool isLeafDirectory() const;
  void loadNextPageCover();
  void startPageLoad(int page);
  int getItemsPerPage() const;
  int getPageForIndex(int index) const;

  // Buffer cache management
  bool storeCoverBuffer();
  bool restoreCoverBuffer();
  void freeCoverBuffer();

 public:
  explicit MyLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                             const std::function<void()>& onGoHome,
                             const std::function<void(const std::string& path)>& onSelectBook,
                             std::string initialPath = "/")
      : Activity("MyLibrary", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelectBook(onSelectBook),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
