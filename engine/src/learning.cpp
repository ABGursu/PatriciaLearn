#include "learning.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <vector>

namespace Learning {

// Experience Book options (default values from HypnoS)
bool experienceBookEnabled = false;
int experienceBookWidth = 1;
int experienceBookEvalImportance = 5;
int experienceBookMinDepth = 27;
int experienceBookMaxMoves = 16;

// Write buffer size
#ifndef NDEBUG
constexpr size_t WriteBufferSize = 1024;
#else
constexpr size_t WriteBufferSize = 1024 * 1024 * 16; // 16MB
#endif

// Experience data storage class (moved outside anonymous namespace for access)
class ExperienceData {
private:
  std::string _filename;
  std::vector<ExpEntryEx*> _expData;
  std::vector<ExpEntryEx*> _newPvExp;
  std::vector<ExpEntryEx*> _newMultiPvExp;
  std::vector<ExpEntryEx*> _oldExpData;

  // Hash map: key -> ExpEntryEx* (linked list head)
  std::unordered_map<uint64_t, ExpEntryEx*> _mainExp;

  bool _loading;
  std::atomic<bool> _abortLoading;
  std::atomic<bool> _loadingResult;
  std::thread* _loaderThread;
  std::condition_variable _loadingCond;
  std::mutex _loaderMutex;

  void clear() {
    _abortLoading.store(true, std::memory_order_relaxed);
    wait_for_load_finished();
    assert(_loaderThread == nullptr);

    clear_new_exp();

    // Free main exp data
    for (ExpEntryEx*& p : _expData)
      free(p);

    // Delete previous game experience data
    for (ExpEntryEx*& p : _oldExpData)
      delete p;

    _mainExp.clear();
    _oldExpData.clear();
    _expData.clear();
  }

  void clear_new_exp() {
    for (auto& newExp : {_newPvExp, _newMultiPvExp})
      std::copy(newExp.begin(), newExp.end(), std::back_inserter(_oldExpData));

    _newPvExp.clear();
    _newMultiPvExp.clear();
  }

  bool link_entry(ExpEntryEx* exp) {
    auto itr = _mainExp.find(exp->key);

    // If new entry: insert into map
    if (itr == _mainExp.end()) {
      _mainExp[exp->key] = exp;
      return true;
    }

    // If existing entry and same move exists then merge
    ExpEntryEx* exp2 = itr->second->find(static_cast<uint16_t>(exp->move));
    if (exp2) {
      exp2->merge(exp);
      return false;
    }

    // If existing entry and different move then insert sorted
    exp2 = itr->second;
    do {
      if (exp->compare(exp2) > 0) {
        if (exp2 == itr->second) {
          itr->second = exp;
          exp->next = exp2;
        } else {
          exp->next = exp2->next;
          exp2->next = exp;
        }
        return true;
      }

      if (!exp2->next) {
        exp2->next = exp;
        return true;
      }

      exp2 = exp2->next;
    } while (true);
  }

  bool _load(const std::string& fn) {
    std::ifstream in(fn, std::ios::in | std::ios::binary | std::ios::ate);

    if (!in.is_open()) {
      std::cout << "info string Could not open experience file: " << fn
                << std::endl;
      return false;
    }

    const size_t inSize = in.tellg();

    if (inSize == 0) {
      std::cout << "info string The experience file [" << fn << "] is empty"
                << std::endl;
      return false;
    }

    // Check signature (accept both PatriciaLearn and HypnoS signatures for compatibility)
    in.seekg(0, std::ios::beg);
    char sigBuffer[256];
    const size_t sigLen = strlen(ExperienceSignature);
    constexpr const char* HypnoSSignature = "SugaR Experience version 2";
    const size_t hypnoSSigLen = strlen(HypnoSSignature);
    const size_t maxSigLen = std::max(sigLen, hypnoSSigLen);
    
    if (inSize < maxSigLen) {
      std::cout << "info string The file [" << fn
                << "] is not a valid experience file" << std::endl;
      return false;
    }

    if (!in.read(sigBuffer, maxSigLen)) {
      std::cout << "info string Failed to read signature from [" << fn << "]"
                << std::endl;
      return false;
    }

    // Check if it matches PatriciaLearn signature
    bool isPatriciaSig = (memcmp(sigBuffer, ExperienceSignature, sigLen) == 0);
    // Check if it matches HypnoS signature (for file compatibility)
    bool isHypnoSSig = (inSize >= hypnoSSigLen && 
                        memcmp(sigBuffer, HypnoSSignature, hypnoSSigLen) == 0);

    if (!isPatriciaSig && !isHypnoSSig) {
      std::cout << "info string The file [" << fn
                << "] is not a valid experience file" << std::endl;
      return false;
    }

    // Use the actual signature length found
    const size_t actualSigLen = isPatriciaSig ? sigLen : hypnoSSigLen;

    // Seek to correct position after signature
    in.seekg(static_cast<std::streamoff>(actualSigLen), std::ios::beg);

    // Check entry count
    const size_t entriesDataLength = inSize - actualSigLen;
    const size_t expCount = entriesDataLength / sizeof(ExpEntry);

    if (expCount * sizeof(ExpEntry) != entriesDataLength) {
      std::cout << "info string The file [" << fn
                << "] has invalid size (not multiple of entry size)" << std::endl;
      return false;
    }

    // Allocate buffer for ExpEntryEx data
    auto* expData = static_cast<ExpEntryEx*>(
        malloc(expCount * sizeof(ExpEntryEx)));

    if (!expData) {
      std::cerr << "info string Failed to allocate "
                << expCount * sizeof(ExpEntryEx)
                << " bytes for experience data from file [" << fn << "]"
                << std::endl;
      return false;
    }

    const size_t prevPosCount = _mainExp.size();

    // Load experience entries
    size_t duplicateMoves = 0;
    ExpEntryEx* exp = expData;

    for (size_t i = 0; i < expCount; ++i, ++exp) {
      if (_abortLoading.load(std::memory_order_relaxed))
        break;

      exp->next = nullptr;

      // Read ExpEntry data
      if (!in.read(reinterpret_cast<char*>(exp), sizeof(ExpEntry))) {
        std::cout << "info string Failed to read experience entry #" << i + 1
                  << " of " << expCount << std::endl;
        free(expData);
        return false;
      }

      // Link entry
      if (!link_entry(exp))
        duplicateMoves++;
    }

    in.close();

    _expData.push_back(expData);

    if (_abortLoading.load(std::memory_order_relaxed))
      return false;

    // Show statistics
    if (prevPosCount) {
      std::cout << "info string " << fn
                << " -> Total new moves: " << expCount
                << ". Total new positions: " << (_mainExp.size() - prevPosCount)
                << ". Duplicate moves: " << duplicateMoves << std::endl;
    } else {
      const double frag = expCount > 0
                              ? 100.0 * static_cast<double>(duplicateMoves) /
                                    static_cast<double>(expCount)
                              : 0.0;

      std::cout << "info string " << fn << " -> Total moves: " << expCount
                << ". Total positions: " << _mainExp.size()
                << ". Duplicate moves: " << duplicateMoves
                << ". Fragmentation: " << std::fixed << std::setprecision(2)
                << frag << "%" << std::endl;
    }

    return true;
  }

  bool _save(const std::string& fn, bool saveAll) {
    std::fstream out;
    out.open(fn, std::ios::out | std::ios::binary | std::ios::app);

    if (!out.is_open()) {
      std::cout << "info string Failed to open experience file [" << fn
                << "] for writing" << std::endl;
      return false;
    }

    // Check if file is new
    out.seekg(0, std::fstream::end);
    const size_t length = out.tellg();
    out.seekg(0, std::fstream::beg);

    if (length == 0) {
      out.seekp(0, std::fstream::beg);
      out.write(ExperienceSignature, strlen(ExperienceSignature));
      if (!out) {
        std::cout << "info string Failed to write signature to experience file ["
                  << fn << "]" << std::endl;
        return false;
      }
    }

    out.seekp(0, std::ios::end);

    std::vector<char> writeBuffer;
    writeBuffer.reserve(WriteBufferSize);

    auto write_entry = [&](const ExpEntry* exp, bool force) -> bool {
      if (exp) {
        const char* data = reinterpret_cast<const char*>(exp);
        writeBuffer.insert(writeBuffer.end(), data,
                          data + sizeof(ExpEntry));
      }

      bool success = true;
      if (force || writeBuffer.size() >= WriteBufferSize) {
        out.write(writeBuffer.data(), writeBuffer.size());
        if (!out)
          success = false;
        writeBuffer.clear();
      }

      return success;
    };

    if (saveAll) {
      size_t allMoves = 0;
      size_t allPositions = 0;

      for (ExpEntryEx* expEx : _newPvExp)
        link_entry(expEx);

      for (ExpEntryEx* expEx : _newMultiPvExp)
        link_entry(expEx);

      for (auto& x : _mainExp) {
        allPositions++;
        ExpEntryEx* exp = x.second;

        // Scale counts (exactly as HypnoS)
        uint16_t maxCount = std::numeric_limits<uint8_t>::min();
        ExpEntryEx* exp1 = exp;
        while (exp1) {
          maxCount = std::max(maxCount, exp1->count);
          exp1 = exp1->next;
        }

        // Scale down (exactly as HypnoS)
        const uint16_t scale = 1 + maxCount / 128;
        exp1 = exp;
        while (exp1) {
          exp1->count = std::max(static_cast<uint16_t>(exp1->count / scale), static_cast<uint16_t>(1));
          exp1 = exp1->next;
        }

        // Save (exactly as HypnoS)
        while (exp) {
          if (exp->depth >= static_cast<int32_t>(MinDepth)) {
            allMoves++;
            if (!write_entry(exp, false)) {
              std::cout << "info string Failed to save experience entry to "
                           "experience file ["
                        << fn << "]" << std::endl;
              return false;
            }
          }
          exp = exp->next;
        }
      }

      std::cout << "info string Saved " << allPositions << " position(s) and "
                << allMoves << " moves to experience file: " << fn << std::endl;
    } else {
      // Deduplicate new entries
      std::unordered_set<uint64_t> seen;

      // NOTE: do NOT cast e->move; read its raw bytes instead (exactly as HypnoS)
      auto km_hash = [](const ExpEntryEx* e) -> uint64_t {
        uint64_t mv = 0;
        const size_t n = (sizeof(mv) < sizeof(e->move)) ? sizeof(mv) : sizeof(e->move);
        std::memcpy(&mv, &e->move, n);
        return static_cast<uint64_t>(e->key) ^ (mv * 0x9E3779B185EBCA87ULL);
      };

      size_t pvWritten = 0, mpvWritten = 0;

      for (auto* const expList : {&_newPvExp, &_newMultiPvExp}) {
        for (const ExpEntryEx* exp : *expList) {
          if (exp->depth < static_cast<int32_t>(MinDepth))
            continue;

          const uint64_t sig = km_hash(exp);
          if (!seen.insert(sig).second)
            continue;

          if (!write_entry(exp, false)) {
            std::cout << "info string Failed to save experience entry to "
                         "experience file ["
                      << fn << "]" << std::endl;
            return false;
          }

          if (expList == &_newPvExp)
            ++pvWritten;
          else
            ++mpvWritten;
        }
      }

      std::cout << "info string Saved " << pvWritten << " PV and "
                << mpvWritten << " MultiPV entries to experience file: " << fn
                << std::endl;
    }

    write_entry(nullptr, true);
    clear_new_exp();

    return true;
  }

public:
  ExperienceData()
      : _loading(false), _abortLoading(false), _loadingResult(false),
        _loaderThread(nullptr) {}

  ~ExperienceData() { clear(); }

  std::string filename() const { return _filename; }

  bool has_new_exp() const {
    return !_newPvExp.empty() || !_newMultiPvExp.empty();
  }

  bool load(const std::string& filename, bool synchronous) {
    wait_for_load_finished();

    _filename = filename;
    _loadingResult.store(false, std::memory_order_relaxed);

    {
      _loading = true;
      std::lock_guard lg1(_loaderMutex);

      _loaderThread = new std::thread([this, filename]() {
        const bool loadingResult = _load(filename);
        _loadingResult.store(loadingResult, std::memory_order_relaxed);

        std::thread* t = _loaderThread;
        _loaderThread = nullptr;

        {
          std::lock_guard lg2(_loaderMutex);
          _loading = false;
          _loadingCond.notify_one();
        }

        t->detach();
        delete t;
      });
    }

    return !synchronous || wait_for_load_finished();
  }

  bool wait_for_load_finished() {
    std::unique_lock ul(_loaderMutex);
    _loadingCond.wait(ul, [&] { return !_loading; });
    return loading_result();
  }

  bool loading_result() const {
    return _loadingResult.load(std::memory_order_relaxed);
  }

  void save(const std::string& fn, bool saveAll, bool ignoreLoadingCheck) {
    if (!ignoreLoadingCheck)
      wait_for_load_finished();

    if (!has_new_exp() && (!saveAll || _mainExp.empty()))
      return;

    // Create backup if saveAll
    const std::string expFilename = fn;
    std::string backupExpFilename;

    if (saveAll) {
      std::ifstream test(expFilename);
      if (test.good()) {
        test.close();
        backupExpFilename = expFilename + ".bak";

        // Delete existing backup
        std::remove(backupExpFilename.c_str());

        // Rename current file
        if (std::rename(expFilename.c_str(), backupExpFilename.c_str()) != 0) {
          std::cout << "info string Could not create backup of current "
                       "experience file"
                    << std::endl;
          backupExpFilename.clear();
        }
      }
    }

    // Save
    if (!_save(fn, saveAll)) {
      // Restore backup on failure
      if (!backupExpFilename.empty()) {
        std::rename(backupExpFilename.c_str(), expFilename.c_str());
      }
    }
  }

  const ExpEntryEx* probe(uint64_t k) const {
    auto itr = _mainExp.find(k);
    if (itr == _mainExp.end())
      return nullptr;

    assert(itr->second->key == k);
    return itr->second;
  }

  void add_pv_experience(uint64_t k, uint16_t m, int32_t v, uint8_t d) {
    auto* exp = new ExpEntryEx(k, m, v, d, 1);
    if (exp) {
      _newPvExp.emplace_back(exp);
      link_entry(exp);
    }
  }

  void add_multipv_experience(uint64_t k, uint16_t m, int32_t v, uint8_t d) {
    auto* exp = new ExpEntryEx(k, m, v, d, 1);
    if (exp) {
      _newMultiPvExp.emplace_back(exp);
      link_entry(exp);
    }
  }
};

// Global experience state
ExperienceData* currentExperience = nullptr;
bool experienceEnabled = true;
bool experienceReadonly = false;
bool learningPaused = false;
std::string experienceFilename = "patricia.exp";

// Global experience functions
std::atomic<bool> g_benchMode{false};
std::atomic<bool> g_benchSingleShot{false};

void touch() {
  if (!enabled())
    return;

  if (experienceFilename.empty())
    return;

  std::fstream out;
  out.open(experienceFilename, std::ios::out | std::ios::binary | std::ios::app);
  if (!out.is_open())
    return;

  out.seekg(0, std::fstream::end);
  const size_t length = out.tellg();
  out.seekg(0, std::fstream::beg);

  if (length == 0) {
    out.seekp(0, std::fstream::beg);
    out.write(ExperienceSignature, strlen(ExperienceSignature));
  }
}

void init() {
  if (!experienceEnabled) {
    unload();
    return;
  }

  if (experienceFilename.empty()) {
    experienceFilename = "patricia.exp";
  }

  if (currentExperience) {
    if (currentExperience->filename() == experienceFilename &&
        currentExperience->loading_result())
      return;
    unload();
  }

  currentExperience = new ExperienceData();
  currentExperience->load(experienceFilename, false);
}

bool enabled() { return experienceEnabled; }

void unload() {
  save();

  delete currentExperience;
  currentExperience = nullptr;
}

void save() {
  if (!currentExperience || !currentExperience->has_new_exp())
    return;
  
  if (experienceReadonly)
    return;

  currentExperience->save(currentExperience->filename(), false, false);
}

const ExpEntryEx* probe(uint64_t k) {
  assert(experienceEnabled);
  if (!currentExperience)
    return nullptr;

  return currentExperience->probe(k);
}

const ExpEntryEx* find_best_entry(uint64_t k) {
  const ExpEntryEx* bestEntry = nullptr;
  const ExpEntryEx* currentEntry = probe(k);

  while (currentEntry) {
    if (!bestEntry || currentEntry->compare(bestEntry) > 0)
      bestEntry = currentEntry;

    currentEntry = currentEntry->next;
  }

  return bestEntry;
}

void set_experience_enabled(bool enabled) {
  experienceEnabled = enabled;
  init();
  if (enabled) {
    resume_learning();
  }
}

void set_experience_file(const std::string& filename) {
  experienceFilename = filename;
  init();
}

void set_experience_readonly(bool readonly) {
  experienceReadonly = readonly;
}

void wait_for_loading_finished() {
  if (!currentExperience)
    return;

  currentExperience->wait_for_load_finished();
}

void pause_learning() { learningPaused = true; }
void resume_learning() { learningPaused = false; }
bool is_learning_paused() { return learningPaused; }

void add_pv_experience(uint64_t k, uint16_t m, int32_t v, uint8_t d) {
  if (!currentExperience || !experienceEnabled || learningPaused)
    return;

  // Bench: allow ONE single write (single-shot), then block all subsequent
  // writes
  if (g_benchMode.load(std::memory_order_relaxed)) {
    if (!g_benchSingleShot.exchange(false, std::memory_order_acq_rel))
      return;
  }

  currentExperience->add_pv_experience(k, m, v, d);
}

void add_multipv_experience(uint64_t k, uint16_t m, int32_t v, uint8_t d) {
  if (!currentExperience || g_benchMode.load(std::memory_order_relaxed) ||
      !experienceEnabled || learningPaused)
    return;

  currentExperience->add_multipv_experience(k, m, v, d);
}

// Experience Book option setters
void set_experience_book_enabled(bool enabled) {
  experienceBookEnabled = enabled;
}

void set_experience_book_width(int width) {
  experienceBookWidth = std::max(1, std::min(20, width));
}

void set_experience_book_eval_importance(int importance) {
  experienceBookEvalImportance = std::max(0, std::min(10, importance));
}

void set_experience_book_min_depth(int depth) {
  experienceBookMinDepth = std::max(static_cast<int>(MinDepth), std::min(64, depth));
}

void set_experience_book_max_moves(int moves) {
  experienceBookMaxMoves = std::max(1, std::min(100, moves));
}

// Experience Book option getters
bool get_experience_book_enabled() {
  return experienceBookEnabled;
}

int get_experience_book_width() {
  return experienceBookWidth;
}

int get_experience_book_eval_importance() {
  return experienceBookEvalImportance;
}

int get_experience_book_min_depth() {
  return experienceBookMinDepth;
}

int get_experience_book_max_moves() {
  return experienceBookMaxMoves;
}

//=============================================================================
// Utility Commands: defrag, merge, show_exp
//=============================================================================

// Helper function to strip quotes from filename
static std::string unquote(const std::string& s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

// Defrag command: defragments an experience file
// Usage: defrag <filename>
void defrag(const std::string& filename) {
  wait_for_loading_finished();
  
  std::string fn = unquote(filename);
  
  if (fn.empty()) {
    printf("info string Error: No filename provided\n");
    printf("info string Syntax: defrag <filename>\n");
    return;
  }
  
  printf("info string Defragmenting experience file: %s\n", fn.c_str());
  
  // Create a temporary ExperienceData to load and re-save
  ExperienceData exp;
  
  if (!exp.load(fn, true)) {
    printf("info string Error: Failed to load experience file\n");
    return;
  }
  
  // Save defragmented file
  exp.save(fn, true, false);
  printf("info string Defragmentation complete\n");
}

// Merge command: merges multiple experience files into one
// Usage: merge <target_file> <source_file1> [source_file2] ...
void merge(const std::vector<std::string>& filenames) {
  wait_for_loading_finished();
  
  if (filenames.size() < 2) {
    printf("info string Error: Incorrect merge command\n");
    printf("info string Syntax: merge <target_file> <source_file1> [source_file2] ...\n");
    printf("info string The first <target_file> is the destination file\n");
    printf("info string The files <source_file1> ... are the experience files to merge\n");
    return;
  }
  
  // Collect unquoted filenames
  std::vector<std::string> files;
  for (const auto& fn : filenames) {
    files.push_back(unquote(fn));
  }
  
  std::string targetFilename = files.front();
  
  printf("info string Merging experience files:\n");
  for (const auto& fn : files) {
    printf("info string   %s\n", fn.c_str());
  }
  printf("info string Target file: %s\n", targetFilename.c_str());
  
  // Load and merge all files
  ExperienceData exp;
  
  for (const auto& fn : files) {
    exp.load(fn, true);
  }
  
  // Save merged file
  exp.save(targetFilename, true, false);
  printf("info string Merge complete\n");
}

// Show_exp command: displays experience data for a position
// Usage: show_exp (displays experience for current position)
void show_exp(uint64_t key, bool extended) {
  wait_for_loading_finished();
  
  printf("info string Experience for position key: %llu\n", 
         static_cast<unsigned long long>(key));
  
  const ExpEntryEx* head = probe(key);
  if (!head) {
    printf("info string No experience data found for this position\n");
    return;
  }
  
  int evalImportance = get_experience_book_eval_importance();
  
  // Collect entries with quality scores
  std::vector<std::pair<const ExpEntryEx*, int>> qualityList;
  for (const ExpEntryEx* t = head; t; t = t->next) {
    qualityList.emplace_back(t, t->quality(evalImportance));
  }
  
  // Sort by quality (highest first)
  std::stable_sort(qualityList.begin(), qualityList.end(),
                   [](const auto& a, const auto& b) { return a.second > b.second; });
  
  printf("\n");
  int expCount = 0;
  
  for (const auto& pr : qualityList) {
    int32_t value = pr.first->value;
    int32_t depth = pr.first->depth;
    uint16_t count = pr.first->count;
    uint16_t move = static_cast<uint16_t>(pr.first->move);
    int quality = pr.second;
    
    // Extract move components using Patricia's format:
    // from: bits 10-15, to: bits 4-9, promo: bits 2-3, type: bits 0-1
    int from = (move >> 10) & 0x3F;
    int to = (move >> 4) & 0x3F;
    int promo = (move >> 2) & 0x3;
    int type = move & 0x3;
    
    // Convert to algebraic notation
    char from_file = 'a' + (from % 8);
    char from_rank = '1' + (from / 8);
    char to_file = 'a' + (to % 8);
    char to_rank = '1' + (to / 8);
    
    char moveStr[8];
    // type == 1 is promotion
    if (type == 1) {
      const char* promoChars = "nbrq";  // 0=knight, 1=bishop, 2=rook, 3=queen
      snprintf(moveStr, sizeof(moveStr), "%c%c%c%c%c", 
               from_file, from_rank, to_file, to_rank, promoChars[promo]);
    } else {
      snprintf(moveStr, sizeof(moveStr), "%c%c%c%c", 
               from_file, from_rank, to_file, to_rank);
    }
    
    // Eval string
    char evalStr[32];
    snprintf(evalStr, sizeof(evalStr), "cp %d", value);
    
    // Check for mate scores
    constexpr int32_t VALUE_MATE = 32000;
    constexpr int MAX_PLY = 256;
    if (value >= VALUE_MATE - MAX_PLY || value <= -VALUE_MATE + MAX_PLY) {
      int plies = (value >= VALUE_MATE - MAX_PLY) ? (VALUE_MATE - value) : (VALUE_MATE + value);
      int m = (plies + 1) / 2;
      if (value <= -VALUE_MATE + MAX_PLY) m = -m;
      char mateStr[16];
      snprintf(mateStr, sizeof(mateStr), " (mate %d)", m);
      strcat(evalStr, mateStr);
    }
    
    if (extended) {
      printf("%2d: %-5s, depth: %2d, eval: %-12s, count: %5u, quality: %d\n",
             ++expCount, moveStr, depth, evalStr, count, quality);
    } else {
      printf("%2d: %-5s, depth: %2d, eval: %s\n",
             ++expCount, moveStr, depth, evalStr);
    }
  }
  
  printf("\n");
}

} // namespace Learning

