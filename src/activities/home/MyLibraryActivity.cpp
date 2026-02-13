#include "MyLibraryActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr unsigned long GO_HOME_MS = 1000;
constexpr int GRID_COLS = 3;
constexpr int GRID_THUMB_HEIGHT = 180;
constexpr int GRID_CELL_GAP = 10;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    // Directories first
    bool isDir1 = str1.back() == '/';
    bool isDir2 = str2.back() == '/';
    if (isDir1 != isDir2) return isDir1;

    // Start naive natural sort
    const char* s1 = str1.c_str();
    const char* s2 = str2.c_str();

    // Iterate while both strings have characters
    while (*s1 && *s2) {
      // Check if both are at the start of a number
      if (isdigit(*s1) && isdigit(*s2)) {
        // Skip leading zeros and track them
        const char* start1 = s1;
        const char* start2 = s2;
        while (*s1 == '0') s1++;
        while (*s2 == '0') s2++;

        // Count digits to compare lengths first
        int len1 = 0, len2 = 0;
        while (isdigit(s1[len1])) len1++;
        while (isdigit(s2[len2])) len2++;

        // Different length so return smaller integer value
        if (len1 != len2) return len1 < len2;

        // Same length so compare digit by digit
        for (int i = 0; i < len1; i++) {
          if (s1[i] != s2[i]) return s1[i] < s2[i];
        }

        // Numbers equal so advance pointers
        s1 += len1;
        s2 += len2;
      } else {
        // Regular case-insensitive character comparison
        char c1 = tolower(*s1);
        char c2 = tolower(*s2);
        if (c1 != c2) return c1 < c2;
        s1++;
        s2++;
      }
    }

    // One string is prefix of other
    return *s1 == '\0' && *s2 != '\0';
  });
}

void MyLibraryActivity::taskTrampoline(void* param) {
  auto* self = static_cast<MyLibraryActivity*>(param);
  self->displayTaskLoop();
}

bool MyLibraryActivity::isLeafDirectory() const {
  for (const auto& f : files) {
    if (f.back() == '/') return false;
  }
  return !files.empty();
}

int MyLibraryActivity::getItemsPerPage() const {
  auto metrics = UITheme::getInstance().getMetrics();
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight =
      renderer.getScreenHeight() - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int titleHeight = renderer.getLineHeight(UI_10_FONT_ID) + 5;
  const int cellHeight = GRID_THUMB_HEIGHT + titleHeight + GRID_CELL_GAP;
  const int rowsPerPage = contentHeight / cellHeight;
  return rowsPerPage * GRID_COLS;
}

int MyLibraryActivity::getPageForIndex(int index) const {
  const int ipp = getItemsPerPage();
  return ipp > 0 ? index / ipp : 0;
}

void MyLibraryActivity::startPageLoad(int page) {
  currentPage = page;
  pageCoversLoaded = false;
  const int ipp = getItemsPerPage();
  pageLoadIndex = page * ipp;
}

void MyLibraryActivity::loadNextPageCover() {
  const int ipp = getItemsPerPage();
  const int pageEnd = std::min((currentPage + 1) * ipp, static_cast<int>(gridEntries.size()));

  // Skip already-loaded items (from previous visits to this page)
  while (static_cast<int>(pageLoadIndex) < pageEnd && gridEntries[pageLoadIndex].loaded) {
    pageLoadIndex++;
  }

  if (static_cast<int>(pageLoadIndex) >= pageEnd) {
    pageCoversLoaded = true;
    return;
  }

  auto& entry = gridEntries[pageLoadIndex];
  const auto& filename = files[pageLoadIndex];
  std::string fullPath = basepath;
  if (fullPath.back() != '/') fullPath += "/";
  fullPath += filename;

  if (StringUtils::checkFileExtension(filename, ".epub")) {
    Epub epub(fullPath, "/.crosspoint");
    epub.load(false, true);
    entry.title = epub.getTitle();
    entry.coverBmpPath = epub.getThumbBmpPath();
    epub.generateThumbBmp(GRID_THUMB_HEIGHT);
  } else if (StringUtils::checkFileExtension(filename, ".xtch") || StringUtils::checkFileExtension(filename, ".xtc")) {
    Xtc xtc(fullPath, "/.crosspoint");
    if (xtc.load()) {
      entry.title = xtc.getTitle();
      entry.coverBmpPath = xtc.getThumbBmpPath();
      xtc.generateThumbBmp(GRID_THUMB_HEIGHT);
    }
  } else {
    entry.title = filename;
  }

  entry.loaded = true;
  pageLoadIndex++;
  updateRequired = true;
}

bool MyLibraryActivity::storeCoverBuffer() {
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  freeCoverBuffer();
  const size_t bufferSize = GfxRenderer::getBufferSize();
  coverBuffer = static_cast<uint8_t*>(malloc(bufferSize));
  if (!coverBuffer) return false;
  memcpy(coverBuffer, frameBuffer, bufferSize);
  return true;
}

bool MyLibraryActivity::restoreCoverBuffer() {
  if (!coverBuffer) return false;
  uint8_t* frameBuffer = renderer.getFrameBuffer();
  if (!frameBuffer) return false;
  memcpy(frameBuffer, coverBuffer, GfxRenderer::getBufferSize());
  return true;
}

void MyLibraryActivity::freeCoverBuffer() {
  if (coverBuffer) {
    free(coverBuffer);
    coverBuffer = nullptr;
  }
  coverBufferStored = false;
  cachedPage = -1;
}

void MyLibraryActivity::loadFiles() {
  files.clear();

  auto root = Storage.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt") ||
          StringUtils::checkFileExtension(filename, ".md")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);

  // Reset grid state on every directory change
  freeCoverBuffer();
  gridEntries.clear();
  currentPage = -1;
  pageCoversLoaded = false;
  pageLoadIndex = 0;

  isGridMode = isLeafDirectory();
  if (isGridMode) {
    gridEntries.resize(files.size());
  }
}

void MyLibraryActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  loadFiles();

  selectorIndex = 0;
  updateRequired = true;

  xTaskCreate(&MyLibraryActivity::taskTrampoline, "MyLibraryActivityTask",
              8192,               // Stack size (increased for cover generation)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void MyLibraryActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  files.clear();
  gridEntries.clear();
  freeCoverBuffer();
}

void MyLibraryActivity::loop() {
  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS &&
      basepath != "/") {
    basepath = "/";
    loadFiles();
    selectorIndex = 0;
    updateRequired = true;
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    } else {
      onSelectBook(basepath + files[selectorIndex]);
      return;
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  }

  const int listSize = static_cast<int>(files.size());
  if (listSize == 0) return;

  if (isGridMode) {
    // Grid mode: 4-directional navigation using ButtonNavigator
    const int ipp = getItemsPerPage();

    // Left/Right: move by single item
    buttonNavigator.onRelease({MappedInputManager::Button::Left}, [this, listSize] {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
      updateRequired = true;
    });

    buttonNavigator.onRelease({MappedInputManager::Button::Right}, [this, listSize] {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
      updateRequired = true;
    });

    // Up/Down: move by row
    buttonNavigator.onRelease({MappedInputManager::Button::Up}, [this, listSize] {
      selectorIndex = (static_cast<int>(selectorIndex) + listSize - GRID_COLS) % listSize;
      updateRequired = true;
    });

    buttonNavigator.onRelease({MappedInputManager::Button::Down}, [this, listSize] {
      selectorIndex = (static_cast<int>(selectorIndex) + GRID_COLS) % listSize;
      updateRequired = true;
    });

    // Up/Down continuous: move by page
    buttonNavigator.onContinuous({MappedInputManager::Button::Up}, [this, listSize, ipp] {
      selectorIndex = (static_cast<int>(selectorIndex) + listSize - ipp) % listSize;
      updateRequired = true;
    });

    buttonNavigator.onContinuous({MappedInputManager::Button::Down}, [this, listSize, ipp] {
      selectorIndex = (static_cast<int>(selectorIndex) + ipp) % listSize;
      updateRequired = true;
    });
  } else {
    // List mode: standard ButtonNavigator navigation
    const int pageItems = UITheme::getInstance().getNumberOfItemsPerPage(renderer, true, false, true, false);

    buttonNavigator.onNextRelease([this, listSize] {
      selectorIndex = ButtonNavigator::nextIndex(static_cast<int>(selectorIndex), listSize);
      updateRequired = true;
    });

    buttonNavigator.onPreviousRelease([this, listSize] {
      selectorIndex = ButtonNavigator::previousIndex(static_cast<int>(selectorIndex), listSize);
      updateRequired = true;
    });

    buttonNavigator.onNextContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::nextPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      updateRequired = true;
    });

    buttonNavigator.onPreviousContinuous([this, listSize, pageItems] {
      selectorIndex = ButtonNavigator::previousPageIndex(static_cast<int>(selectorIndex), listSize, pageItems);
      updateRequired = true;
    });
  }
}

void MyLibraryActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    if (isGridMode && !pageCoversLoaded) {
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      loadNextPageCover();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void MyLibraryActivity::render() {
  if (isGridMode) {
    renderGrid();
  } else {
    renderList();
  }
}

void MyLibraryActivity::renderList() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No books found");
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, files.size(), selectorIndex,
        [this](int index) { return files[index]; }, nullptr, nullptr, nullptr);
  }

  // Help text
  const auto labels = mappedInput.mapLabels(basepath == "/" ? "« Home" : "« Back", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

void MyLibraryActivity::renderGrid() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();

  const int ipp = getItemsPerPage();
  const int viewPage = getPageForIndex(selectorIndex);

  // If page changed, start loading the new page's covers
  if (viewPage != currentPage) {
    startPageLoad(viewPage);
  }

  const int pageStart = viewPage * ipp;
  const int pageItemCount = std::min(ipp, static_cast<int>(gridEntries.size()) - pageStart);
  const int localSelected = static_cast<int>(selectorIndex) - pageStart;

  // Try to restore buffer if we have the right page cached
  bool bufferRestored = (coverBufferStored && cachedPage == viewPage && restoreCoverBuffer());

  if (!bufferRestored) {
    renderer.clearScreen();

    auto folderName = basepath == "/" ? "SD card" : basepath.substr(basepath.rfind('/') + 1).c_str();
    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, folderName);
  }

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;

  // Build page slice as vector<RecentBook> (same pattern as HomeActivity)
  std::vector<RecentBook> pageItems;
  pageItems.reserve(pageItemCount);
  for (int i = pageStart; i < pageStart + pageItemCount; i++) {
    const auto& entry = gridEntries[i];
    pageItems.push_back({"", entry.loaded ? entry.title : files[i], "", entry.loaded ? entry.coverBmpPath : ""});
  }

  GUI.drawBookCoverGrid(renderer, Rect{0, contentTop, pageWidth, contentHeight}, pageItems, localSelected,
                        GRID_THUMB_HEIGHT, GRID_COLS, bufferRestored);

  // Cache the buffer once all covers on this page are loaded
  if (!bufferRestored && pageCoversLoaded) {
    coverBufferStored = storeCoverBuffer();
    cachedPage = viewPage;
  }

  // Page indicator
  const int totalPages = (static_cast<int>(gridEntries.size()) + ipp - 1) / ipp;
  if (totalPages > 1) {
    std::string pageText = std::to_string(viewPage + 1) + " / " + std::to_string(totalPages);
    const int textWidth = renderer.getTextWidth(UI_10_FONT_ID, pageText.c_str());
    renderer.drawText(UI_10_FONT_ID, pageWidth - textWidth - metrics.contentSidePadding,
                      pageHeight - metrics.buttonHintsHeight - metrics.verticalSpacing - 15, pageText.c_str());
  }

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

size_t MyLibraryActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}