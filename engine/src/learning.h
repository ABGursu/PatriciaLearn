#pragma once
#include "defs.h"
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Learning {

// Minimum depth to store experience entries
constexpr uint8_t MinDepth = 4;

// Experience Book options (from HypnoS)
extern bool experienceBookEnabled;
extern int experienceBookWidth;
extern int experienceBookEvalImportance;
extern int experienceBookMinDepth;
extern int experienceBookMaxMoves;

// Experience Book option setters
void set_experience_book_enabled(bool enabled);
void set_experience_book_width(int width);
void set_experience_book_eval_importance(int importance);
void set_experience_book_min_depth(int depth);
void set_experience_book_max_moves(int moves);

// Experience Book option getters
bool get_experience_book_enabled();
int get_experience_book_width();
int get_experience_book_eval_importance();
int get_experience_book_min_depth();
int get_experience_book_max_moves();

// Experience entry structure (V2 format)
// Format matches HypnoS exactly: 8+4+4+4+2+2 = 24 bytes
struct ExpEntry {
  uint64_t key;   // Zobrist key (8 bytes)
  int32_t move;   // Move stored as int32_t for file format compatibility (4 bytes)
  int32_t value;  // Score (4 bytes)
  int32_t depth;  // Depth stored as int32_t for file format compatibility (4 bytes)
  uint16_t count; // Count (2 bytes) - scaled version of count
  uint8_t padding[2]; // Padding to make it 24 bytes total (2 bytes)

  ExpEntry() = delete;
  ExpEntry(const ExpEntry&) = delete;
  ExpEntry& operator=(const ExpEntry&) = delete;

  explicit ExpEntry(uint64_t k, uint16_t m, int32_t v, uint8_t d)
      : ExpEntry(k, m, v, d, 1) {}

  explicit ExpEntry(uint64_t k, uint16_t m, int32_t v, uint8_t d, uint16_t c)
      : key(k), move(static_cast<int32_t>(m)), value(v), depth(static_cast<int32_t>(d)), count(c) {
    padding[0] = padding[1] = 0x00;
  }

  void merge(const ExpEntry* exp) {
    // Merge the count (exactly as HypnoS)
    count = static_cast<uint16_t>(std::min<uint32_t>(
        static_cast<uint32_t>(count) + static_cast<uint32_t>(exp->count),
        static_cast<uint32_t>(std::numeric_limits<uint16_t>::max())));

    // Merge value and depth only if 'exp' is better or equal
    if (depth == exp->depth) {
      value = (value + exp->value) / 2;
    } else if (depth < exp->depth) {
      value = exp->value;
      depth = exp->depth;
    }
  }

  int compare(const ExpEntry* exp) const {
    static constexpr int DepthScale = 10;
    static constexpr int CountScale = 3;

    auto scaledValue = [](const int v, const int d, const int c) -> int {
      return v * std::max(d / DepthScale, 1) * std::max(c / CountScale, 1);
    };

    int v = scaledValue(value, depth, count) - scaledValue(exp->value, exp->depth, exp->count);

    if (v)
      return v;

    if ((v = count - exp->count) != 0)
      return v;

    return depth - exp->depth;
  }
};

static_assert(sizeof(ExpEntry) == 24, "ExpEntry must be 24 bytes");

// Extended entry with linked list support
struct ExpEntryEx : ExpEntry {
  ExpEntryEx* next = nullptr;

  ExpEntryEx() = delete;
  ExpEntryEx(const ExpEntryEx&) = delete;
  ExpEntryEx& operator=(const ExpEntryEx&) = delete;

  explicit ExpEntryEx(uint64_t k, uint16_t m, int32_t v, uint8_t d,
                      uint16_t c = 1)
      : ExpEntry(k, m, v, d, c), next(nullptr) {}

  // Simple quality calculation for Experience Book
  // Returns quality score based on count, depth, and value
  int quality(int evalImportance = 5) const {
    constexpr int QualityEvalImportanceMax = 10;
    int q = static_cast<int>(count) * (QualityEvalImportanceMax - evalImportance);
    
    // Add value contribution based on evalImportance
    if (evalImportance > 0 && depth > 0) {
      // Positive value = good for us, scale by depth importance
      q += (value * evalImportance) / (QualityEvalImportanceMax * 10);
    }
    
    return q / QualityEvalImportanceMax;
  }

  ExpEntryEx* find(uint16_t m) const {
    auto* exp = const_cast<ExpEntryEx*>(this);
    do {
      if (exp->move == static_cast<int32_t>(m))
        return exp;
      exp = exp->next;
    } while (exp);
    return nullptr;
  }

  ExpEntryEx* find(uint16_t mv, uint8_t minDepth) const {
    auto* temp = const_cast<ExpEntryEx*>(this);
    do {
      if (temp->move == static_cast<int32_t>(mv)) {
        if (temp->depth < static_cast<int32_t>(minDepth))
          temp = nullptr;
        break;
      }
      temp = temp->next;
    } while (temp);
    return temp;
  }
};

// Experience file signature (same as HypnoS for full compatibility)
constexpr const char* ExperienceSignature = "SugaR Experience version 2";
constexpr int ExperienceVersion = 2;

// Global functions
void init();
bool enabled();
void unload();
void save();

// UCI option setters
void set_experience_enabled(bool enabled);
void set_experience_file(const std::string& filename);
void set_experience_readonly(bool readonly);

void wait_for_loading_finished();

const ExpEntryEx* probe(uint64_t key);
const ExpEntryEx* find_best_entry(uint64_t key);

void pause_learning();
void resume_learning();
bool is_learning_paused();

void add_pv_experience(uint64_t key, uint16_t move, int32_t value,
                      uint8_t depth);
void add_multipv_experience(uint64_t key, uint16_t move, int32_t value,
                           uint8_t depth);

// Bench mode flags
extern std::atomic<bool> g_benchMode;
extern std::atomic<bool> g_benchSingleShot;

void touch();

// Utility commands (defrag, merge, show_exp)
void defrag(const std::string& filename);
void merge(const std::vector<std::string>& filenames);
void show_exp(uint64_t key, bool extended = false);

} // namespace Learning
