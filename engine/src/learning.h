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
#include <utility>
#include <vector>

// Google dense_hash_map for optimized experience storage
#include "sparsehash/dense_hash_map"

namespace Learning {

// Empty and deleted keys for dense_hash_map
// These values should never appear as valid Zobrist keys
constexpr uint64_t EmptyKey = 0ULL;
constexpr uint64_t DeletedKey = 1ULL;

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
  uint64_t key; // Zobrist key (8 bytes)
  int32_t
      move; // Move stored as int32_t for file format compatibility (4 bytes)
  int32_t value; // Score (4 bytes)
  int32_t
      depth; // Depth stored as int32_t for file format compatibility (4 bytes)
  uint16_t count;     // Count (2 bytes) - scaled version of count
  uint8_t padding[2]; // Padding to make it 24 bytes total (2 bytes)

  ExpEntry() = delete;
  ExpEntry(const ExpEntry &) = delete;
  ExpEntry &operator=(const ExpEntry &) = delete;

  explicit ExpEntry(uint64_t k, uint16_t m, int32_t v, uint8_t d)
      : ExpEntry(k, m, v, d, 1) {}

  explicit ExpEntry(uint64_t k, uint16_t m, int32_t v, uint8_t d, uint16_t c)
      : key(k), move(static_cast<int32_t>(m)), value(v),
        depth(static_cast<int32_t>(d)), count(c) {
    padding[0] = padding[1] = 0x00;
  }

  void merge(const ExpEntry *exp) {
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

  int compare(const ExpEntry *exp) const {
    static constexpr int DepthScale = 10;
    static constexpr int CountScale = 3;

    auto scaledValue = [](const int v, const int d, const int c) -> int {
      return v * std::max(d / DepthScale, 1) * std::max(c / CountScale, 1);
    };

    int v = scaledValue(value, depth, count) -
            scaledValue(exp->value, exp->depth, exp->count);

    if (v)
      return v;

    if ((v = count - exp->count) != 0)
      return v;

    return depth - exp->depth;
  }
};

static_assert(sizeof(ExpEntry) == 24, "ExpEntry must be 24 bytes");

// Forward declaration for use in ExpEntryEx template
struct ExpEntryEx;
const ExpEntryEx *probe(uint64_t key);

// Extended entry with linked list support
struct ExpEntryEx : ExpEntry {
  ExpEntryEx *next = nullptr;

  ExpEntryEx() = delete;
  ExpEntryEx(const ExpEntryEx &) = delete;
  ExpEntryEx &operator=(const ExpEntryEx &) = delete;

  explicit ExpEntryEx(uint64_t k, uint16_t m, int32_t v, uint8_t d,
                      uint16_t c = 1)
      : ExpEntry(k, m, v, d, c), next(nullptr) {}

  // Simple quality calculation (non-positional, for basic sorting)
  // Matches HypnoS logic: count weighted by (10-evalImportance), value weighted
  // by evalImportance
  int quality(int evalImportance = 5) const {
    constexpr int QualityEvalImportanceMax = 10;

    // Quality based on move count (weight decreases as evalImportance
    // increases)
    int q =
        static_cast<int>(count) * (QualityEvalImportanceMax - evalImportance);

    // Quality based on evaluation (like HypnoS: value contributes
    // proportionally to evalImportance)
    if (evalImportance > 0) {
      // Use value directly scaled by evalImportance (matches HypnoS
      // calculation)
      q += value * evalImportance / QualityEvalImportanceMax;
    }

    return q / QualityEvalImportanceMax;
  }

  // Positional quality calculation with lookahead (like HypnoS)
  // Returns {quality_score, maybe_draw}
  // pos: current position (will be copied for lookahead, not modified)
  // evalImportance: 0-10, higher = more weight on evaluation differences
  // MakeMoveFunc: function to apply move to position (e.g., make_move)
  // IsDrawFunc: optional function to check if position is a draw (e.g.,
  // is_repetition)
  template <typename TPosition, typename MakeMoveFunc,
            typename IsDrawFunc = std::nullptr_t>
  std::pair<int, bool>
  quality_positional(TPosition pos, int evalImportance,
                     MakeMoveFunc make_move_fn,
                     IsDrawFunc is_draw_fn = nullptr) const {
    constexpr int QualityEvalImportanceMax = 10;
    constexpr int QualityExperienceMovesAhead = 10;

    bool maybeDraw = false;
    int q =
        static_cast<int>(count) * (QualityEvalImportanceMax - evalImportance);

    if (evalImportance > 0) {
      const int us = pos.color;
      const int them = us ^ 1;

      std::array<int64_t, 2> sum = {0, 0};
      std::array<int64_t, 2> weight = {0, 0};

      sum[us] = count;
      weight[us] = 1;

      int me = us;
      const ExpEntryEx *lastExp[2] = {nullptr, nullptr};
      const ExpEntryEx *temp1 = this;
      int moveCount = 0;

      while (temp1 && moveCount < QualityExperienceMovesAhead) {
        lastExp[me] = temp1;

        // Apply move to position copy
        uint16_t mv = static_cast<uint16_t>(temp1->move);
        make_move_fn(pos, mv);
        me ^= 1;
        ++moveCount;

        // Draw detection after move (like HypnoS: pos.is_draw(pos.game_ply()))
        // If is_draw_fn is provided, use it to detect draws
        if constexpr (!std::is_same_v<IsDrawFunc, std::nullptr_t>) {
          if (!maybeDraw && is_draw_fn != nullptr) {
            maybeDraw = is_draw_fn(pos);
          }
        }

        // Probe next position
        temp1 = probe(pos.zobrist_key);
        if (!temp1)
          break;

        // Find best entry for opponent
        const ExpEntryEx *temp2 = temp1->next;
        while (temp2) {
          if (temp2->compare(temp1) > 0)
            temp1 = temp2;
          temp2 = temp2->next;
        }

        if (lastExp[me]) {
          sum[me] += static_cast<int64_t>(temp1->value - lastExp[me]->value);
          ++weight[me];
        }
      }

      int64_t s = sum[us];
      int64_t w = weight[us];

      if (weight[them] > 0) {
        s -= sum[them];
        w += weight[them];
      }

      if (w > 0) {
        q += static_cast<int>(s * evalImportance / w);
      }
    } else {
      // Shallow draw detection when evalImportance is zero (like HypnoS)
      // Make the move and check for draw, then undo
      if constexpr (!std::is_same_v<IsDrawFunc, std::nullptr_t>) {
        if (is_draw_fn != nullptr) {
          TPosition tempPos = pos;
          make_move_fn(tempPos, static_cast<uint16_t>(move));
          maybeDraw = is_draw_fn(tempPos);
        }
      }
    }

    return {q / QualityEvalImportanceMax, maybeDraw};
  }

  ExpEntryEx *find(uint16_t m) const {
    auto *exp = const_cast<ExpEntryEx *>(this);
    do {
      if (exp->move == static_cast<int32_t>(m))
        return exp;
      exp = exp->next;
    } while (exp);
    return nullptr;
  }

  ExpEntryEx *find(uint16_t mv, uint8_t minDepth) const {
    auto *temp = const_cast<ExpEntryEx *>(this);
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
constexpr const char *ExperienceSignature = "SugaR Experience version 2";
constexpr int ExperienceVersion = 2;

// Global functions
void init();
bool enabled();
void unload();
void save();

// UCI option setters
void set_experience_enabled(bool enabled);
void set_experience_file(const std::string &filename);
void set_experience_readonly(bool readonly);

void wait_for_loading_finished();

const ExpEntryEx *probe(uint64_t key);
const ExpEntryEx *find_best_entry(uint64_t key);

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

// Utility commands (defrag, merge)
// Note: exp and expex commands are implemented in uci.h to avoid LTO multiple
// definition errors
void defrag(const std::string &filename);
void merge(const std::vector<std::string> &filenames);

} // namespace Learning
