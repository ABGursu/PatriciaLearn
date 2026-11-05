#pragma once
#include "draw_detect.h"
#include "fathom/src/tbprobe.h"
#include "learning.h"
#include "movepick.h"
#include "nnue.h"
#include "params.h"
#include "position.h"
#include "tm.h"
#include "utils.h"
#include <array>
#include <atomic>
#include <cassert>
#include <mutex>
#include <random>

constexpr int NormalizationFactor = 195;

// ============================================================================
// ARMAGEDDON MODE
// ============================================================================

// Global UCI option variables
inline bool armageddonMode =
    false; // UCI: Armageddon (Base: Anti-Draw penalties)
inline bool armageddonMcts =
    false; // UCI: Armageddon MCTS (Cardanobile/Joergoster visit-score + playout)
inline bool armageddonTbPenalty =
    false; // UCI: Armageddon TB Penalty (penalize even TB draws)
inline bool armageddonDrawDetection =
    false; // UCI: Armageddon Draw Detection (Crystal+BlackHole)

// Setters for UCI options
inline void set_armageddon_mode(bool enabled) { armageddonMode = enabled; }
inline void set_armageddon_mcts(bool enabled) { armageddonMcts = enabled; }
inline void set_armageddon_tb_penalty(bool enabled) {
  armageddonTbPenalty = enabled;
}
inline void set_armageddon_draw_detection(bool enabled) {
  armageddonDrawDetection = enabled;
}

// Getters for UCI options
inline bool get_armageddon_mode() { return armageddonMode; }
inline bool get_armageddon_mcts() { return armageddonMcts; }
inline bool get_armageddon_tb_penalty() { return armageddonTbPenalty; }
inline bool get_armageddon_draw_detection() { return armageddonDrawDetection; }

inline bool add_detection_active(const ThreadInfo &ti) {
  return armageddonDrawDetection;
}

// Sample an alpha-beta return into Armageddon MCTS root statistics.
inline void mcts_sample(ThreadInfo &ti, int ply, int score) {
  if (!armageddonMcts)
    return;
  ti.mctsVisits++;
  ti.mctsAllScores += (ply % 2 == 0) ? score : -score;
}

// Armageddon draw penalty constant
constexpr int ArmageddonPenalty = ScoreMate / 2;

// ============================================================================
// MECHANISM 1: Dynamic Contempt - Scale penalty based on game phase
// ============================================================================
// Calculate phase-based penalty scaling (Endgame = less penalty)
inline int get_armageddon_penalty_scaled(int phase) {
  if (!armageddonMode)
    return ArmageddonPenalty;

  // Phase: 0=Middlegame (full penalty), 1=Endgame (reduced), 2=Sacrifice (full)
  // Scale: Middlegame=100%, Endgame=40%, Sacrifice=100%
  int scale;
  if (phase == PhaseTypes::Endgame) {
    scale = 40; // Endgame: 40% penalty (6400 instead of 16000)
  } else {
    scale = 100; // Middlegame/Sacrifice: full penalty
  }

  return (ArmageddonPenalty * scale) / 100;
}

// Draw value with Armageddon penalty (perspective-aware)
// In Armageddon mode, draws are bad for US (root side), good for opponent
// At even ply (our turn): return negative (bad for us)
// At odd ply (opponent's turn): return positive (good for them, which negates
// to bad for us at parent) tb_draw: if true, no penalty applied (unless
// armageddonTbPenalty is enabled) phase: game phase for dynamic contempt
// (optional, defaults to Middlegame)
inline int get_draw_value_armageddon(int ply, bool tb_draw = false,
                                     int phase = PhaseTypes::Middlegame) {
  if (armageddonMode && (!tb_draw || armageddonTbPenalty)) {
    // MECHANISM 1: Dynamic Contempt - scale penalty by phase
    int penalty = get_armageddon_penalty_scaled(phase);
    // Even ply = our turn (root side), odd ply = opponent's turn
    return (ply % 2 == 0) ? -penalty : penalty;
  }
  return 0; // Normal draw value
}

// ============================================================================
// ARMAGEDDON: Rule 50 scaling - penalize positions approaching 50-move draw
// ============================================================================
// Returns scale factor 0-100. At 100 = no scaling, at 0 = full draw penalty
inline int armageddon_rule50_scale(int halfmoves) {
  if (!armageddonMode || halfmoves < 60)
    return 100; // No scaling until move 60
  // From halfmove 60 onwards, gradually reduce eval toward draw
  // At 100 halfmoves, scale is 0 (full draw penalty)
  return std::max(0, 100 - (halfmoves - 60) * 100 / 40);
}

// Apply rule50 scaling to evaluation (Armageddon only)
inline int apply_armageddon_rule50_scaling(int eval, int halfmoves, int ply,
                                           int phase) {
  if (!armageddonMode || halfmoves < 60)
    return eval;
  int scale = armageddon_rule50_scale(halfmoves);
  if (scale >= 100)
    return eval;
  // Blend evaluation toward draw penalty
  int draw_penalty = get_draw_value_armageddon(ply, false, phase);
  return (eval * scale + draw_penalty * (100 - scale)) / 100;
}

// ============================================================================
// CUCKOO HASHING FOR UPCOMING REPETITION DETECTION
// ============================================================================
// Based on Crystal/Stockfish implementation
// See:
// http://web.archive.org/web/20201107002606/https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf

// Hash functions for Cuckoo table indexing
inline int cuckoo_h1(uint64_t h) { return h & 0x1fff; }
inline int cuckoo_h2(uint64_t h) { return (h >> 16) & 0x1fff; }

// Cuckoo tables - 8192 entries each
inline std::array<uint64_t, 8192> cuckooKeys;
inline std::array<Move, 8192> cuckooMoves;
inline std::atomic<bool> cuckooInitialized{false};
inline std::mutex cuckooInitMutex;

// Get piece attacks for Cuckoo initialization
inline uint64_t get_cuckoo_piece_attacks(int piece_type, int sq, uint64_t occ) {
  switch (piece_type) {
  case PieceTypes::Knight:
    return KnightAttacks[sq];
  case PieceTypes::Bishop:
    return get_bishop_attacks(sq, occ);
  case PieceTypes::Rook:
    return get_rook_attacks(sq, occ);
  case PieceTypes::Queen:
    return get_bishop_attacks(sq, occ) | get_rook_attacks(sq, occ);
  case PieceTypes::King:
    return KingAttacks[sq];
  default:
    return 0;
  }
}

// Initialize Cuckoo tables at engine startup
// CRITICAL FIX: Thread-safe initialization using double-checked locking pattern
// This prevents race conditions when multiple threads try to initialize
// simultaneously
inline void init_cuckoo() {
  // Fast path: check if already initialized (atomic read, no lock needed)
  if (cuckooInitialized.load(std::memory_order_acquire))
    return;

  // Slow path: acquire lock and check again (double-checked locking)
  std::lock_guard<std::mutex> lock(cuckooInitMutex);

  // Check again after acquiring lock (another thread might have initialized it)
  if (cuckooInitialized.load(std::memory_order_relaxed))
    return;

  cuckooKeys.fill(0);
  cuckooMoves.fill(MoveNone);

  // Generate all reversible piece moves (non-pawn pieces)
  // For each piece type and color, generate all possible from-to pairs
  for (int piece = Pieces::WKnight; piece <= Pieces::BKing; piece++) {
    int piece_type = get_piece_type(piece);
    if (piece_type == PieceTypes::Pawn)
      continue; // Skip pawns

    for (int s1 = 0; s1 < 64; s1++) {
      uint64_t attacks = get_cuckoo_piece_attacks(piece_type, s1, 0ULL);

      while (attacks) {
        int s2 = pop_lsb(attacks);
        if (s2 <= s1)
          continue; // Avoid duplicates (only store s1 < s2)

        // Compute Zobrist key for moving piece from s1 to s2
        // Key = piece[s1] XOR piece[s2] XOR side_to_move
        uint64_t key = zobrist_keys[get_zobrist_key(piece, s1)] ^
                       zobrist_keys[get_zobrist_key(piece, s2)] ^
                       zobrist_keys[side_index];

        Move move = pack_move(s1, s2, MoveTypes::Normal);
        int i = cuckoo_h1(key);

        // Cuckoo insertion with displacement
        while (true) {
          std::swap(cuckooKeys[i], key);
          std::swap(cuckooMoves[i], move);
          if (move == MoveNone)
            break; // Found empty slot
          // Push victim to alternative slot
          i = (i == cuckoo_h1(key)) ? cuckoo_h2(key) : cuckoo_h1(key);
        }
      }
    }
  }

  // Mark as initialized (release semantics ensure all writes are visible)
  cuckooInitialized.store(true, std::memory_order_release);
}

// ============================================================================
// UPCOMING REPETITION DETECTION (Crystal port with Cuckoo hashing)
// ============================================================================
// Tests if there's an upcoming move that draws by repetition
// Uses Cuckoo hashing for accurate detection of reversible moves
inline bool upcoming_repetition(const Position &position,
                                ThreadInfo &thread_info) {
  int halfmoves = position.halfmoves;
  int game_ply = thread_info.game_ply;
  int search_ply = thread_info.search_ply;

  // CRITICAL FIX: Early return for initial position
  // Need at least 3 halfmoves for any repetition to be possible
  // game_ply starts at 6, so we need at least game_ply >= 8 (2 moves played) to
  // check repetition But also check that we have enough history entries
  if (halfmoves < 3)
    return false;

  // CRITICAL FIX: Ensure game_ply is within valid bounds for game_hist array
  // access game_ply starts at 6, so we need at least 2 moves (game_ply >= 8) to
  // have history Also ensure we don't access out-of-bounds (game_hist array
  // size is GameSize)
  if (game_ply < 8 || game_ply - 2 < 0 || game_ply >= GameSize)
    return false;

  // Ensure Cuckoo tables are initialized (thread-safe)
  if (!cuckooInitialized.load(std::memory_order_acquire)) {
    init_cuckoo();
  }

  uint64_t originalKey = position.zobrist_key;
  int end = std::min((int)halfmoves, game_ply);

  // Cuckoo-based detection for reversible moves
  // This detects if ANY legal reversing move would create a repetition
  if (end >= 3 && game_ply >= 3) {
    uint64_t other = originalKey;

    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (game_ply >= 1 && (game_ply - 1) < GameSize) {
      other ^= thread_info.game_hist[game_ply - 1].position_key ^
               zobrist_keys[side_index];
    }

    for (int i = 3; i <= end; i += 2) {
      int idx1 = game_ply - i;
      int idx2 = game_ply - i - 1;

      // CRITICAL FIX: Ensure indices are valid before accessing game_hist
      // Check both lower and upper bounds to prevent out-of-bounds access
      if (idx1 < 0 || idx2 < 0 || idx1 >= GameSize || idx2 >= GameSize)
        break;

      other ^= thread_info.game_hist[idx1].position_key ^
               thread_info.game_hist[idx2].position_key ^
               zobrist_keys[side_index];

      if (other != 0)
        continue;

      // Check Cuckoo table for reversible move
      uint64_t moveKey = originalKey ^ thread_info.game_hist[idx1].position_key;
      int j;

      if ((j = cuckoo_h1(moveKey), cuckooKeys[j] == moveKey) ||
          (j = cuckoo_h2(moveKey), cuckooKeys[j] == moveKey)) {

        Move move = cuckooMoves[j];
        if (move != MoveNone) {
          int from = extract_from(move);
          int to = extract_to(move);

          // Check if path between squares is clear
          uint64_t occ = position.colors_bb[Colors::White] |
                         position.colors_bb[Colors::Black];
          uint64_t between = BetweenBBs[from][to];

          // Path is clear if no pieces block it (excluding destination)
          if (!((between ^ (1ULL << to)) & occ)) {
            if (search_ply > i)
              return true;

            // For positions at or before root, check repetition count
            if (thread_info.game_hist[idx1].repetition > 0)
              return true;
          }
        }
      }
    }
  }

  return false;
}

// ============================================================================
// HELPER: Check if a move would cause repetition (for move ordering)
// ============================================================================
inline bool would_cause_repetition(const Position &pos,
                                   ThreadInfo &thread_info) {
  uint64_t key = pos.zobrist_key;
  int halfmoves = pos.halfmoves;
  int game_ply = thread_info.game_ply;

  // CRITICAL FIX: Ensure we have enough game history to check
  if (game_ply < 2 || halfmoves < 2)
    return false;

  // CRITICAL FIX: Ensure game_ply is within valid bounds
  if (game_ply >= GameSize)
    return false;

  int end_index = std::max(game_ply - halfmoves, 0);
  for (int i = game_ply - 2; i >= end_index; i -= 2) {
    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (i < 0 || i >= GameSize)
      break; // Safety check
    if (key == thread_info.game_hist[i].position_key) {
      return true;
    }
  }
  return false;
}

// ============================================================================
// OPTIMIZATION: Calculate zobrist key after a move without making the move
// ============================================================================
// This is a simplified version that handles most cases (normal moves, captures)
// For complex moves (castling, en passant, promotion), we fall back to
// make_move Returns true in *success if key was calculated, false if we need
// make_move fallback
inline uint64_t key_after_move(const Position &position, Move move,
                               bool *success) {
  *success = true;

  if (move == MoveNone) {
    return position.zobrist_key ^ zobrist_keys[side_index];
  }

  uint64_t key = position.zobrist_key;
  int from = extract_from(move);
  int to = extract_to(move);
  int from_piece = position.board[from];
  int to_piece = position.board[to];
  int move_type = extract_type(move);

  // Handle special move types (castling, en passant, promotion) - fall back to
  // make_move
  if (move_type == MoveTypes::Castling || move_type == MoveTypes::EnPassant ||
      move_type == MoveTypes::Promotion) {
    *success = false; // Need to use make_move for complex moves
    return 0;
  }

  // Normal move: update key for piece moving
  key ^= zobrist_keys[get_zobrist_key(from_piece, from)];
  key ^= zobrist_keys[get_zobrist_key(from_piece, to)];

  // If capture, remove captured piece
  if (to_piece != Pieces::Blank) {
    key ^= zobrist_keys[get_zobrist_key(to_piece, to)];
  }

  // Flip side to move
  key ^= zobrist_keys[side_index];

  return key;
}

// OPTIMIZED: Check if move would cause repetition using key_after (avoids
// position copy)
inline bool would_cause_repetition_after_move(const Position &position,
                                              Move move,
                                              ThreadInfo &thread_info) {
  bool key_calculated;
  uint64_t key_after = key_after_move(position, move, &key_calculated);

  // If key calculation failed (complex move), fall back to make_move
  if (!key_calculated) {
    // Fallback: use make_move (but this is rare for most moves)
    Position temp = position;
    make_move(temp, move);
    bool result = would_cause_repetition(temp, thread_info);
    // Note: We can't undo_move, but this is only for complex moves (rare)
    return result;
  }

  // Check if this key appears in history
  int halfmoves = position.halfmoves + 1;  // Move will increment halfmoves
  int game_ply = thread_info.game_ply + 1; // After move

  // CRITICAL FIX: Ensure we have enough game history to check and bounds are
  // valid
  if (game_ply < 2 || halfmoves < 2 || game_ply >= GameSize)
    return false;

  int end_index = std::max(game_ply - halfmoves, 0);
  for (int i = game_ply - 2; i >= end_index; i -= 2) {
    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (i < 0 || i >= GameSize)
      break; // Safety check
    if (key_after == thread_info.game_hist[i].position_key) {
      return true;
    }
  }

  return false;
}

void update_history(int16_t &entry, int score) { // Update history score
  entry += score - entry * abs(score) / 16384;
}
void update_corrhist(int16_t &entry, int score) { // Update history score
  entry += score - entry * abs(score) / 1024;
}

TbResult probe_tb(Position &pos) {
  if (pop_count(pos.colors_bb[Colors::White] | pos.colors_bb[Colors::Black]) >
      TB_LARGEST)
    return TB_RESULT_FAILED;

  int castling_flags = 0;
  if (pos.castling_squares[Colors::White][Sides::Kingside] != SquareNone)
    castling_flags |= 0x1;
  if (pos.castling_squares[Colors::White][Sides::Queenside] != SquareNone)
    castling_flags |= 0x2;
  if (pos.castling_squares[Colors::Black][Sides::Kingside] != SquareNone)
    castling_flags |= 0x4;
  if (pos.castling_squares[Colors::Black][Sides::Queenside] != SquareNone)
    castling_flags |= 0x8;

  return tb_probe_wdl(
      pos.colors_bb[Colors::White], pos.colors_bb[Colors::Black],
      pos.pieces_bb[PieceTypes::King], pos.pieces_bb[PieceTypes::Queen],
      pos.pieces_bb[PieceTypes::Rook], pos.pieces_bb[PieceTypes::Bishop],
      pos.pieces_bb[PieceTypes::Knight], pos.pieces_bb[PieceTypes::Pawn],
      pos.halfmoves, castling_flags,
      pos.ep_square == SquareNone ? 0 : pos.ep_square,
      pos.color == Colors::White);
}

bool out_of_time(ThreadInfo &thread_info) {
  if (thread_data.stop || thread_info.datagen_stop) {
    return true;
  } else if (thread_info.thread_id != 0 || thread_info.current_iter == 1) {
    return false;
  }

  if (thread_data.ponder)
    return false;

  if (thread_info.nodes >= thread_info.max_nodes_searched) {

    if (thread_info.max_time < INT32_MAX / 3) {

      std::this_thread::sleep_for(std::chrono::milliseconds(
          thread_info.opt_time - time_elapsed(thread_info.start_time)));
    }

    if (thread_info.doing_datagen) {
      thread_info.datagen_stop = true;
    } else {
      thread_data.stop = true;
    }
    return true;
  }
  thread_info.time_checks++;
  if (thread_info.time_checks == 512) {
    thread_info.time_checks = 0;
    if (thread_data.stopOnPonderhit ||
        time_elapsed(thread_info.start_time) > thread_info.max_time) {
      thread_data.stop = true;
      return true;
    }
  }
  return false;
}

// Base function - calculates material evaluation
inline int16_t material_eval_uncached(const Position &position) {
  int m = (position.material_count[0] - position.material_count[1]) * 100 +
          (position.material_count[2] - position.material_count[3]) * 300 +
          (position.material_count[4] - position.material_count[5]) * 300 +
          (position.material_count[6] - position.material_count[7]) * 500 +
          (position.material_count[8] - position.material_count[9]) * 900;

  return position.color ? -m : m;
}

// Cached version - use this in production
inline int16_t material_eval(const Position &position,
                             ThreadInfo &thread_info) {
  // Cache check
  if (thread_info.materialCache.key == position.zobrist_key &&
      thread_info.materialCache.valid) {
    return thread_info.materialCache.material_eval;
  }

  // Calculate
  int16_t result = material_eval_uncached(position);

  // Update cache
  thread_info.materialCache.key = position.zobrist_key;
  thread_info.materialCache.material_eval = result;
  thread_info.materialCache.valid = true;

  return result;
}

// Overload for backward compatibility (without ThreadInfo)
inline int16_t material_eval(const Position &position) {
  return material_eval_uncached(position);
}

// Single door for draw conclusions (ADD + Armageddon + normal Patricia).
// Armageddon ON -> penalty (TB rules via tb_draw). OFF -> Patricia normal draw.
inline int current_draw_score(const Position &pos, ThreadInfo &ti, int ply,
                              bool tb_draw = false) {
  if (armageddonMode)
    return get_draw_value_armageddon(ply, tb_draw, ti.phase);

  int draw_score = 1 - (ti.nodes & 3);
  int material = material_eval(pos, ti);
  if (material < -100)
    draw_score += 50;
  else if (material > 100)
    draw_score -= 50;
  return draw_score;
}

// ============================================================================
// MATERIAL CACHE: total_mat cached version
// ============================================================================
// Base function - calculates total material
inline int16_t total_mat_uncached(const Position &position) {
  int m = (position.material_count[0] + position.material_count[1]) * 100 +
          (position.material_count[2] + position.material_count[3]) * 300 +
          (position.material_count[4] + position.material_count[5]) * 300 +
          (position.material_count[6] + position.material_count[7]) * 500 +
          (position.material_count[8] + position.material_count[9]) * 900;
  return m;
}

// Cached version - use this in production
inline int16_t total_mat(const Position &position, ThreadInfo &thread_info) {
  // Cache check
  if (thread_info.materialCache.key == position.zobrist_key &&
      thread_info.materialCache.valid) {
    return thread_info.materialCache.total_mat;
  }

  // Calculate (or get from material_eval cache if available)
  int16_t result = total_mat_uncached(position);

  // Update cache
  thread_info.materialCache.key = position.zobrist_key;
  thread_info.materialCache.total_mat = result;
  // material_eval might not be cached yet, but that's OK - it will be
  // calculated when needed
  thread_info.materialCache.valid = true;

  return result;
}

// Overload for backward compatibility (without ThreadInfo) - uses position.h
// version Note: This will use the inline version from position.h, not cached

// ============================================================================
// SEE CACHE: Performance optimization for Static Exchange Evaluation
// ============================================================================
// Cached version of SEE - use this in production
inline bool SEE_cached(Position &position, Move move, int threshold,
                       ThreadInfo &thread_info) {
  // Cache check
  if (thread_info.seeCache.key == position.zobrist_key &&
      thread_info.seeCache.move == move && thread_info.seeCache.valid) {
    return thread_info.seeCache.result >= threshold;
  }

  // Calculate
  bool result = SEE(position, move, threshold);

  // Update cache
  thread_info.seeCache.key = position.zobrist_key;
  thread_info.seeCache.move = move;
  thread_info.seeCache.result = result;
  thread_info.seeCache.valid = true;

  return result;
}

bool has_non_pawn_material(const Position &position, int color) {
  int s_indx = 2 + color;
  return (position.material_count[s_indx] ||
          position.material_count[s_indx + 2] ||
          position.material_count[s_indx + 4] ||
          position.material_count[s_indx + 6]);
}

int16_t total_mat_color(const Position &position, int color) {
  // total material for one color

  int m = 0;
  for (int i = 0; i < 5; i++) {
    m += position.material_count[i * 2 + color] * SeeValues[i + 1];
  }
  return m;
}

// Track if current position is singular (for parent bonus)
inline bool is_singular_position(int best_score, int second_best_score,
                                 int depth) {
  if (!armageddonMode)
    return false;

  // If best move is significantly better than second best, it's singular
  // Threshold: best should be at least (depth * 2) centipawns better
  int threshold = depth * 2;
  return (best_score - second_best_score) >= threshold;
}

int eval(Position &position, ThreadInfo &thread_info) {
  int color = position.color;
  int root_color = thread_info.search_ply % 2 ? color ^ 1 : color;
  int eval = thread_info.nnue_state.evaluate(color, thread_info.phase);

  // Patricia is much less dependent on explicit eval twiddling than before, but
  // there are still a few things I do.

  int bonus1 = 0, bonus2 = 0;

  /*
    // Give a small bonus if the position is much better than what material
    would
    // suggest
    if (eval > 0 && eval > m_eval + m_threshold) {
      bonus1 += 25 + (eval - m_eval - m_threshold) / 10;
    } else if (eval < 0 && eval < m_eval - m_threshold) {
      bonus1 -= 25 + (m_eval - eval - m_threshold) / 10;
    }
  */

  bool our_side = (thread_info.search_ply % 2 == 0);

  int start_index = std::max(thread_info.game_ply - thread_info.search_ply, 0);

  int s_m = thread_info.game_hist[start_index].m_diff;
  int s = 0;

  // Give a small bonus if we have sacrificed material at some point in the
  // search tree If we are completely winning, give a bigger bonus to
  // incentivize finding the most stylish move when everything wins

  for (int idx = start_index + 2; idx < thread_info.game_ply - 4; idx += 2) {

    if (thread_info.game_hist[idx].m_diff < 0 &&
        thread_info.game_hist[idx + 1].m_diff > 0 &&
        thread_info.game_hist[idx + 2].m_diff < 0 &&
        thread_info.game_hist[idx + 3].m_diff > 0 &&
        thread_info.game_hist[idx + 4].m_diff < 0) {

      s = thread_info.game_hist[idx + 4].m_diff;
      break;
    }

    if (thread_info.game_hist[idx].m_diff < 0 &&
        thread_info.game_hist[idx].m_diff ==
            material_eval(position, thread_info)) {

      s = thread_info.game_hist[idx].m_diff;
      break;
    }
  }
  if (s && total_mat(position, thread_info) > 4500) {

    if (thread_info.search_ply % 2) {
      bonus2 = -50 *
               (eval < -500 ? 2
                : eval < 0  ? 1
                            : 0) *
               10 /
               (s < -300   ? 5
                : s < -100 ? 10
                           : 20);
    } else {
      bonus2 = 50 *
               (eval > 500 ? 2
                : eval > 0 ? 1
                           : 0) *
               10 /
               (s < -300   ? 5
                : s < -100 ? 10
                           : 20);
    }
  }

  // If we're winning, scale eval by material; we don't want to trade off to an
  // easily won endgame, but instead should continue the attack.

  float multiplier =
      ((float)750 + total_mat(position, thread_info) / 25) / 1024;

  if (!position.material_count[root_color + 8] ||
      total_mat(position, thread_info) < 4000 &&
          ((eval > 0 && our_side) || (eval < 0 && !our_side))) {
    multiplier -= 0.1;
  }

  eval = eval * multiplier;

  return std::clamp(eval + bonus1 + bonus2, ScoreLost + 1, ScoreWin - 1);
}

int correct_eval(const Position &position, ThreadInfo &thread_info, int eval) {

  eval = eval * (200 - position.halfmoves) / 200;

  int corr =
      thread_info
          .PawnCorrHist[position.color][get_corrhist_index(position.pawn_key)];

  corr +=
      thread_info
          .NonPawnCorrHist[position.color][Colors::White][get_corrhist_index(
              position.non_pawn_key[Colors::White])];
  corr +=
      thread_info
          .NonPawnCorrHist[position.color][Colors::Black][get_corrhist_index(
              position.non_pawn_key[Colors::Black])];

  GameHistory *ss = &(thread_info.game_hist[thread_info.game_ply]);

  if ((ss - 1)->played_move != MoveNone && (ss - 2)->played_move != MoveNone) {
    corr += thread_info.ContCorrHist[(ss - 2)->piece_moved][extract_to(
        (ss - 2)->played_move)][(ss - 1)->piece_moved]
                                    [extract_to((ss - 1)->played_move)];
  }

  return std::clamp(eval + (CorrWeight * corr / 512), ScoreLost + 1,
                    ScoreWin - 1);
}

void ss_push(Position &position, ThreadInfo &thread_info, Move move) {
  // update search stack after makemove
  thread_info.search_ply++;

  thread_info.game_hist[thread_info.game_ply].position_key =
      position.zobrist_key;
  thread_info.game_hist[thread_info.game_ply].played_move = move;
  thread_info.game_hist[thread_info.game_ply].piece_moved =
      position.board[extract_from(move)];
  bool cap = is_cap(position, move);
  thread_info.game_hist[thread_info.game_ply].is_cap = cap;
  uint8_t cap_type = PieceTypes::PieceNone;
  if (extract_type(move) == MoveTypes::EnPassant) {
    cap_type = PieceTypes::Pawn;
  } else if (cap) {
    cap_type = get_piece_type(position.board[extract_to(move)]);
  }
  thread_info.game_hist[thread_info.game_ply].captured_type = cap_type;
  thread_info.game_hist[thread_info.game_ply].m_diff =
      material_eval(position, thread_info);

  // IMPROVEMENT 5: Track repetition count for Cuckoo hashing
  int8_t rep_count = 0;
  int halfmoves = position.halfmoves;
  int game_ply = thread_info.game_ply;
  uint64_t key = position.zobrist_key;
  int end_index = std::max(game_ply - halfmoves, 0);
  for (int i = game_ply - 2; i >= end_index; i -= 2) {
    if (thread_info.game_hist[i].position_key == key) {
      rep_count = thread_info.game_hist[i].repetition + 1;
      break;
    }
  }
  thread_info.game_hist[thread_info.game_ply].repetition = rep_count;

  // ============================================================================
  // CACHE INVALIDATION: Position changed, invalidate all caches (OPTIMIZED)
  // ============================================================================
  // After making a move, the position key changes, so all caches are invalid
  // OPTIMIZATION: Soft invalidation (just set key=0) instead of full clear()
  // Cache check will detect key mismatch and automatically invalidate
  thread_info.materialCache.key = 0;
  thread_info.materialCache.valid = false;
  thread_info.seeCache.key = 0;
  thread_info.seeCache.valid = false;

  thread_info.game_ply++;
}

void ss_pop(ThreadInfo &thread_info) {
  // associated with unmake
  thread_info.search_ply--, thread_info.game_ply--;

  thread_info.nnue_state.pop();
}

inline bool
material_draw(const Position &position) { // Is there not enough material on the
                                          // position for one side to win?
  for (int i : {0, 1, 6, 7, 8,
                9}) { // Do we have pawns, rooks, or queens on the position?
    if (position.material_count[i]) {
      return false;
    }
  }
  if (position.material_count[4] > 1 || position.material_count[2] > 2 ||
      (position.material_count[2] &&
       position.material_count[4])) { // Do we have three knights, two bishops,
                                      // or a bishop and knight for either side?
    return false;
  }
  if (position.material_count[5] > 1 || position.material_count[3] > 2 ||
      (position.material_count[3] &&
       position.material_count[5])) { // Do we have three knights, two bishops,
                                      // or a bishop and knight for either side?
    return false;
  }
  return true;
}

inline bool
is_draw(const Position &position,
        const ThreadInfo &thread_info) { // Detects if the position is a draw.

  uint64_t hash = position.zobrist_key;

  int halfmoves = position.halfmoves, game_ply = thread_info.game_ply;
  if (halfmoves >= 100) {
    int color = position.color;

    if (!attacks_square(position, get_king_pos(position, color), color ^ 1)) {
      return true;
    }

    MoveInfo moves;
    if (legal_movegen(position, moves.moves)) {
      return true;
    }
  }
  if (material_draw(position)) {
    return true;
  }
  int start_index =
      game_ply -
      4; // game_ply - 1: last played move, game_ply - 2: your last played move,
         // game_ply - 4 is the first opportunity a repetition is possible
  int end_indx = std::max(game_ply - halfmoves, 0);
  for (int i = start_index; i >= end_indx; i -= 2) {
    if (hash == thread_info.game_hist[i].position_key) {
      return true;
    }
  }
  return false;
}


inline int qsearch(int alpha, int beta, Position &position,
                   ThreadInfo &thread_info,
                   std::vector<TTBucket> &TT) { // Performs a quiescence search
                                                // on the given position.
  if (out_of_time(thread_info)) {
    // return if out of time
    return correct_eval(position, thread_info, eval(position, thread_info));
  }
  int color = position.color;

  GameHistory *ss = &(thread_info.game_hist[thread_info.game_ply]);

  thread_info.nodes++;

  int ply = thread_info.search_ply;

  const bool addActive = add_detection_active(thread_info);
  bool gameCycle = false;



  // NOTE: Original Patricia qsearch has NO is_draw check here
  // Draw detection is only in main search function

  if (ply > thread_info.seldepth) {
    thread_info.seldepth = ply;
  }
  if (ply >= MaxSearchDepth - 1) {
    return correct_eval(
        position, thread_info,
        eval(position,
             thread_info)); // if we're about to overflow stack return
  }

  uint64_t hash = position.zobrist_key;
  uint8_t phase = thread_info.phase;
  bool tt_hit;
  TTEntry &entry = probe_entry(hash, tt_hit, thread_info.searches, TT);

  int entry_type = EntryTypes::None, tt_static_eval = ScoreNone,
      tt_score = ScoreNone;
  Move tt_move = MoveNone; // Initialize TT variables and check for a hash hit

  if (tt_hit) {
    entry_type = entry.get_type();
    tt_static_eval = entry.static_eval;
    tt_score = score_from_tt(entry.score, ply);
    tt_move = entry.best_move;
  }

  if (tt_score != ScoreNone && !gameCycle) {
    if ((entry_type == EntryTypes::Exact) ||
        (entry_type == EntryTypes::LBound && tt_score >= beta) ||
        (entry_type == EntryTypes::UBound && tt_score <= alpha)) {
      // if we get an "accurate" tt score then return
      return tt_score;
    }
  }

  uint64_t in_check =
      attacks_square(position, get_king_pos(position, color), color ^ 1);
  int best_score = ScoreNone, raised_alpha = false;
  Move best_move = MoveNone;

  int static_eval = ScoreNone;
  int raw_eval = ScoreNone;

  if (!in_check) { // If we're not in check and static eval beats beta, we can
                   // immediately return
    if (tt_static_eval == ScoreNone) {
      raw_eval = eval(position, thread_info);
      best_score = static_eval = correct_eval(position, thread_info, raw_eval);
    } else {
      raw_eval = tt_static_eval;
      best_score = static_eval = correct_eval(position, thread_info, raw_eval);
    }

    if (tt_score != ScoreNone) {
      if (entry_type == EntryTypes::Exact ||
          (entry_type == EntryTypes::UBound && tt_score < static_eval) ||
          (entry_type == EntryTypes::LBound && tt_score > static_eval)) {

        best_score = tt_score;
      }
    }

    if (best_score >= beta) {
      return best_score;
    }
    if (best_score > alpha) {
      alpha = best_score;
    }
  }

  MovePicker picker;
  init_picker(picker, position, -107, in_check, ss);

  if (!is_cap(position, tt_move)) {
    tt_move = MoveNone;
  }
  int moves_played = 0;

  while (Move move =
             next_move(picker, position, thread_info, tt_move, !in_check)) {

    if (picker.stage > Stages::Captures && !in_check) {
      break;
    }
    if (!is_legal_fast_unchecked(position, move)) {
      continue;
    }
    moves_played++;
    if (moves_played > 2) {
      break;
    }

    Position moved_position = position;
    make_move(moved_position, move);

    update_nnue_state(thread_info, move, position, moved_position);

    ss_push(position, thread_info, move);
    int score = -qsearch(-beta, -alpha, moved_position, thread_info, TT);
    ss_pop(thread_info);

    thread_info.phase = phase;

    if (thread_data.stop || thread_info.datagen_stop) {
      // return if we ran out of time for search
      return best_score;
    }

    if (score > best_score) {
      best_score = score;
      if (score > alpha) {
        best_move = move;
        raised_alpha = true;
        alpha = score;
      }
      if (score >= beta) { // failing high
        break;
      }
    }
  }

  if (best_score == ScoreNone) { // handle no legal moves (stalemate/checkmate)
    // In qsearch with no moves: if in check = checkmate, else = stalemate
    if (in_check) {
      return ply - ScoreMate; // Checkmate
    }
    return current_draw_score(position, thread_info, ply);
  }

  // insert entries and return

  entry_type = best_score >= beta ? EntryTypes::LBound : EntryTypes::UBound;

  insert_entry(entry, hash, 0, best_move, raw_eval,
               score_to_tt(best_score, ply), entry_type, thread_info.searches);
  return best_score;
}

template <bool is_pv>
int search(int alpha, int beta, int depth, bool cutnode, Position &position,
           ThreadInfo &thread_info,
           std::vector<TTBucket> &TT) { // Performs an alpha-beta search.

  GameHistory *ss = &(thread_info.game_hist[thread_info.game_ply]);

  if (!thread_info.search_ply) {
    thread_info.current_iter = depth;
    thread_info.seldepth = 0;
    std::memset(&thread_info.pv, 0, sizeof(thread_info.pv));
  }

  int ply = thread_info.search_ply, pv_index = ply * MaxSearchDepth;
  bool root = !ply;
  int color = position.color;

  if (ply > thread_info.seldepth) {
    thread_info.seldepth = ply;
  }

  if (out_of_time(thread_info) || ply >= MaxSearchDepth - 1) {
    // check for timeout
    return correct_eval(position, thread_info, eval(position, thread_info));
  }

  const bool addActive = add_detection_active(thread_info);
  bool gameCycle = false;
  bool kingDanger = false;
  bool kingDangerThem = false;



  // Crystal king_danger (ADD only)
  if (addActive && !root) {
    bool ourMove = !(ply & 1);
    kingDanger = !ourMove && DrawDetect::king_danger(position, color);
    kingDangerThem = ourMove && DrawDetect::king_danger(position, color ^ 1);
  }

  // ADD BlackHole: score confirmed drawish shallow branches
  if (addActive && !root && depth < 4 &&
      DrawDetect::drawish_branch(position, thread_info, ply,
                                 thread_info.addBlackHole,
                                 thread_info.addStrongerSide,
                                 thread_info.addTestPassivePlay != 0)) {
    int drawValue = current_draw_score(position, thread_info, ply);
    mcts_sample(thread_info, ply, drawValue);
    return drawValue;
  }

  // Standard draw detection (50-move, material, repetition)
  if (ply && is_draw(position, thread_info)) {
    int drawValue = current_draw_score(position, thread_info, ply);
    mcts_sample(thread_info, ply, drawValue);
    return drawValue;
  }

  if (depth <= 0) {
    int qs = qsearch(alpha, beta, position, thread_info, TT);
    mcts_sample(thread_info, ply, qs);
    return qs; // drop into qsearch if depth is too low.
  }
  thread_info.nodes++;

  bool raised_alpha = false;

  Move best_move = MoveNone;
  Move excluded_move = thread_info.excluded_move;

  bool singular_search = (excluded_move != MoveNone);

  if (!singular_search) {
    thread_info.pv[pv_index] = MoveNone;
  }

  thread_info.excluded_move =
      MoveNone; // If we currently are in singular search, this sets it so moves
                // *after* it are not in singular search
  int score = ScoreNone;
  int best_score = ScoreNone;
  int max_score = -ScoreNone;

  uint64_t hash = position.zobrist_key;
  uint8_t phase = thread_info.phase;

  int mate_distance = ScoreMate - ply;
  if (mate_distance <
      beta) // Mate distance pruning; if we're at depth 10 but we've already
            // found a mate in 3, there's no point searching this.
  {
    beta = mate_distance;
    if (alpha >= beta) {
      mcts_sample(thread_info, ply, beta);
      return beta;
    }
  }

  bool tt_hit;
  TTEntry &entry = probe_entry(hash, tt_hit, thread_info.searches, TT);

  if (singular_search) {
    tt_hit = false;
  }

  int entry_type = EntryTypes::None, tt_static_eval = ScoreNone,
      tt_score = ScoreNone, tt_depth = 0;
  Move tt_move = MoveNone;

  if (tt_hit) { // TT probe
    entry_type = entry.get_type();
    tt_static_eval = entry.static_eval;
    tt_score = score_from_tt(entry.score, ply);
    tt_move = entry.best_move;
    tt_depth = entry.depth;
  }

  if (tt_score != ScoreNone && !is_pv && !gameCycle && tt_depth >= depth) {
    // If we get a useful score from the TT and it's
    // searched to at least the same depth we would
    // have searched, then we can return
    if ((entry_type == EntryTypes::Exact) ||
        (entry_type == EntryTypes::LBound && tt_score >= beta) ||
        (entry_type == EntryTypes::UBound && tt_score <= alpha)) {
      mcts_sample(thread_info, ply, tt_score);
      return tt_score;
    }
  }

  // Probe experience data
  const Learning::ExpEntryEx *expEx =
      (!singular_search && Learning::enabled())
          ? Learning::probe(position.zobrist_key)
          : nullptr;
  const Learning::ExpEntryEx *tempExp = expEx;
  const Learning::ExpEntryEx *bestExp = nullptr;

  // Update history from experience data
  int expCount = 0;

  while (tempExp) {
    if (tempExp->depth >= depth) {
      ++expCount;

      // Better than current TT entry?
      if (!bestExp && (!tt_hit || tempExp->depth > tt_depth)) {
        bestExp = tempExp;
        tt_hit = true;
        tt_move = static_cast<Move>(tempExp->move);
        tt_score = tempExp->value;
        tt_depth = tempExp->depth;
        entry_type =
            (tt_score >= beta ? EntryTypes::LBound : EntryTypes::Exact);

        // Write to TT
        insert_entry(entry, hash, static_cast<int>(tt_depth), tt_move,
                     ScoreNone, score_to_tt(tt_score, ply), entry_type,
                     thread_info.searches);
      }

      // Update history for experience moves (bonus for good moves, malus for
      // bad)
      if (!is_pv) {
        const int32_t expValue = tempExp->value;
        const Move expMove = static_cast<Move>(tempExp->move);

        if (expValue >= beta) {
          // Good move - give bonus
          if (!is_cap(position, expMove)) {
            const int bonus = std::min(130 * tempExp->depth - 71, 1043);
            int piece = position.board[extract_from(expMove)];
            int sq = extract_to(expMove);
            update_history(thread_info.HistoryScores[piece][sq], bonus);
          }
        } else if (!is_cap(position, expMove)) {
          // Bad move - give malus
          const int penalty = -(std::min(130 * tempExp->depth - 71, 1043));
          int piece = position.board[extract_from(expMove)];
          int sq = extract_to(expMove);
          update_history(thread_info.HistoryScores[piece][sq], penalty);
        }
      }
    }

    tempExp = tempExp->next;
  }

  // Increment tbHits for experience hits
  if (expCount)
    thread_info.tb_hits += expCount;

  // Armageddon MCTS + Experience: per-node Exp-singular quiet pruning
  // (Kelly SE idea). Local only — never sticky across nodes.
  bool expSingular = false;
  Move expSingularMove = MoveNone;
  if (armageddonMcts && Learning::enabled() && !singular_search && expEx) {
    int deepSons = 0;
    Move onlyMove = MoveNone;
    uint16_t onlyCount = 0;
    for (const auto *e = expEx; e; e = e->next) {
      if (e->depth >= depth) {
        ++deepSons;
        onlyMove = static_cast<Move>(e->move);
        onlyCount = e->count;
        if (deepSons > 1)
          break;
      }
    }
    if (deepSons == 1 && onlyCount > 6) {
      expSingular = true;
      expSingularMove = onlyMove;
    }
  }

  // Experience lookup with priority if deeper than TT
  if (!singular_search && Learning::enabled()) {
    const auto *bestExpEntry = Learning::find_best_entry(position.zobrist_key);
    // When MCTS is on, still allow Exp cutoff/materialize after the first loop
    // warmed TT from the same Exp data (otherwise Exp→mcts_sample never fires).
    const bool expLookupOk =
        bestExpEntry &&
        (!tt_hit || bestExpEntry->depth > tt_depth ||
         (armageddonMcts && bestExpEntry->depth >= depth));
    if (expLookupOk) {
      const uint8_t expDepth = static_cast<uint8_t>(bestExpEntry->depth);
      const Move expMove = static_cast<Move>(bestExpEntry->move);
      const int32_t expValue = bestExpEntry->value;

      // Early cutoff with experience depth
      if (!is_pv &&
          expDepth > static_cast<uint8_t>(depth - (expValue <= beta ? 1 : 0)) &&
          expValue != ScoreNone &&
          (tt_score == ScoreNone ||
           (expValue >= beta ? (entry_type & EntryTypes::LBound)
                             : (entry_type & EntryTypes::UBound))) &&
          (cutnode == (expValue >= beta) || depth > 5) &&
          (depth > 8 || expMove == MoveNone || position.halfmoves < 80 ||
           (!is_cap(position, expMove) &&
            get_piece_type(position.board[extract_from(expMove)]) !=
                PieceTypes::Pawn))) {
        mcts_sample(thread_info, ply, expValue);
        return expValue;
      }

      // Materialize the Experience entry in TT
      tt_hit = true;
      tt_move = expMove;
      tt_score = expValue;
      tt_depth = static_cast<uint8_t>(expDepth);
      entry_type = (expValue >= beta ? EntryTypes::LBound : EntryTypes::Exact);

      insert_entry(entry, hash, static_cast<int>(tt_depth), tt_move, ScoreNone,
                   score_to_tt(tt_score, ply), entry_type,
                   thread_info.searches);
    }
  }

  const TbResult tb_result =
      (root || singular_search) ? TB_RESULT_FAILED : probe_tb(position);
  if (tb_result != TB_RESULT_FAILED) {

    thread_info.tb_hits++;
    int tb_score;
    uint8_t tb_bound;

    if (tb_result == TB_LOSS) {
      tb_score = ply - ScoreTbWin;
      tb_bound = EntryTypes::UBound;
    } else if (tb_result == TB_WIN) {
      tb_score = ScoreTbWin - ply;
      tb_bound = EntryTypes::LBound;
    } else {
      // TB Draw
      uint32_t tb_wdl = TB_GET_WDL(tb_result);

      // ============================================================================
      // MECHANISM 2: Syzygy TB "Risk-Taking" (DTZ Gambling)
      // ============================================================================
      // If Armageddon mode and TB says DRAW but DTZ > 80, encourage prolonging
      // game
      if (armageddonMode && tb_wdl == TB_DRAW) {
        uint32_t dtz = TB_GET_DTZ(tb_result);
        if (dtz > 80) {
          // Game is far from ending - return small advantage to encourage
          // continuation
          tb_score = 10; // Small advantage instead of 0
          tb_bound = EntryTypes::Exact;
        } else if (armageddonTbPenalty) {
          // Close to endgame - apply full penalty
          tb_score = get_draw_value_armageddon(ply, false, thread_info.phase);
          tb_bound = EntryTypes::Exact;
        } else {
          // Normal: TB draws are exempt from Armageddon penalty
          tb_score = 0;
          tb_bound = EntryTypes::Exact;
        }
      } else if (armageddonMode && armageddonTbPenalty) {
        // Armageddon TB Penalty enabled for other TB draw cases
        tb_score = get_draw_value_armageddon(ply, false, thread_info.phase);
        tb_bound = EntryTypes::Exact;
      } else {
        // Normal: TB draws are exempt from Armageddon penalty
        tb_score = 0;
        tb_bound = EntryTypes::Exact;
      }
    }

    if ((tb_bound == EntryTypes::Exact) ||
        (tb_bound == EntryTypes::LBound ? tb_score >= beta
                                        : tb_score <= alpha)) {
      insert_entry(entry, hash, depth, MoveNone, ScoreNone,
                   score_to_tt(tb_score, ply), tb_bound, thread_info.searches);
      mcts_sample(thread_info, ply, tb_score);
      return tb_score;
    }

    if (is_pv) {
      if (tb_bound == EntryTypes::LBound) {
        best_score = tb_score;
        alpha = std::max(alpha, best_score);
      } else {
        max_score = tb_score;
      }
    }
  }

  uint64_t in_check =
      attacks_square(position, get_king_pos(position, color), color ^ 1);

  // We can't do any eval-based pruning if in check.

  int32_t static_eval;
  int32_t raw_eval;

  if (in_check) {
    static_eval = raw_eval = ScoreNone;
  } else if (singular_search) {
    static_eval = raw_eval = ss->static_eval;
  } else {
    if (tt_static_eval == ScoreNone) {
      raw_eval = eval(position, thread_info);
      static_eval = correct_eval(position, thread_info, raw_eval);

    } else {
      raw_eval = tt_static_eval;
      static_eval = correct_eval(position, thread_info, raw_eval);
    }

    if (!tt_hit) {
      insert_entry(entry, hash, 0, MoveNone, raw_eval, ScoreNone,
                   EntryTypes::None, thread_info.searches);
    }
  }

  ss->static_eval = static_eval;

  // ============================================================================
  // ARMAGEDDON: Rule 50 scaling - penalize positions approaching 50-move draw
  // (FIX 4) OPTIMIZED: Early exit if halfmoves < 60 (function returns eval
  // unchanged)
  // ============================================================================
  if (armageddonMode && !in_check && static_eval != ScoreNone &&
      position.halfmoves >= 60) {
    static_eval = apply_armageddon_rule50_scaling(
        static_eval, position.halfmoves, ply, thread_info.phase);
  }

  bool improving = false;

  // Improving: Is our eval better than it was last turn? If so we can prune
  // less in certain circumstances (or prune more if it's not)

  if (ply > 1 && !in_check &&
      static_eval > ((ss - 2)->static_eval != ScoreNone
                         ? (ss - 2)->static_eval
                         : (ss - 4)->static_eval)) {
    improving = true;
  }

  if (tt_score != ScoreNone) {
    if (entry_type == EntryTypes::Exact ||
        (entry_type == EntryTypes::UBound && tt_score < static_eval) ||
        (entry_type == EntryTypes::LBound && tt_score > static_eval)) {

      static_eval = tt_score;
    }
  }

  // Razoring: if our position is really bad compared to alpha and we're at low depths, we can probably return if qsearch fails as well.
  if (!is_pv && !in_check && !singular_search) {

    const bool skipForGameCycle = addActive && gameCycle;

    if (!skipForGameCycle && alpha < 2000 && depth < 5 &&
        static_eval + 400 * depth < alpha) {
      score = qsearch(alpha, beta, position, thread_info, TT);
      if (score <= alpha) {
        mcts_sample(thread_info, ply, score);
        return score;
      }
    }

    // Reverse Futility Pruning (RFP): If our position is way better than beta,
    // we're likely good to stop searching the node.

    if (!skipForGameCycle && !kingDanger && depth <= RFPMaxDepth &&
        static_eval - RFPMargin * (depth - improving) >= beta) {
      int rfp = (static_eval + beta) / 2;
      mcts_sample(thread_info, ply, rfp);
      return rfp;
    }

    // Null Move Pruning (NMP): If we can give our opponent a free move and
    // still beat beta on a reduced search, we can prune the node.

    if (!skipForGameCycle && static_eval >= beta && depth >= NMPMinDepth &&
        has_non_pawn_material(position, color) &&
        (ss - 1)->played_move != MoveNone && !kingDanger) {

      Position temp_pos = position;
      make_move(temp_pos, MoveNone);

      ss_push(position, thread_info, MoveNone);

      int R = NMPBase + depth / NMPDepthDiv +
              std::min(3, (static_eval - beta) / NMPEvalDiv);
      score = -search<false>(-alpha - 1, -alpha, depth - R, !cutnode, temp_pos,
                             thread_info, TT);

      thread_info.search_ply--, thread_info.game_ply--;
      // we don't call ss_pop because the nnue state was never pushed

      if (score >= beta) {
        if (score >= ScoreWin) {
          score = beta;
        }
        mcts_sample(thread_info, ply, score);
        return score;
      }
    }
  }

  // Internal Iterative Reduction: If we are in a PV node and have no TT move,
  // reduce the depth.
  if ((is_pv || cutnode) && tt_move == MoveNone && depth > IIRMinDepth &&
      !(addActive && gameCycle)) {
    depth--;
  }

  int p_beta = beta + ProbcutMargin;
  // Probcut: if a low depth search of only captures beats beta by a significant margin, we're good to return early.

  if (cutnode && !(addActive && gameCycle) && abs(beta) < ScoreWin &&
      depth > 4 &&
      (tt_hit ? (tt_score >= p_beta && is_cap(position, tt_move))
              : (static_eval >= beta))) {

    int threshold = p_beta - static_eval;
    MovePicker probcut_p;
    init_picker(probcut_p, position, threshold, in_check, ss);
    // Original Patricia: use SEE check (CACHED)
    Move p_tt_move =
        (tt_move != MoveNone &&
         SEE_cached(position, tt_move, threshold, thread_info))
            ? tt_move
            : MoveNone;

    while (Move move =
               next_move(probcut_p, position, thread_info, p_tt_move, true)) {

      if (probcut_p.stage > Stages::Captures) {
        break;
      }
      if (move == excluded_move || !is_legal_fast_unchecked(position, move)) {
        continue;
      }

      Position moved_position = position;
      make_move(moved_position, move);
      update_nnue_state(thread_info, move, position, moved_position);
      ss_push(position, thread_info, move);

      int score =
          -qsearch(-p_beta, -p_beta + 1, moved_position, thread_info, TT);

      if (score >= p_beta) {
        score = -search<is_pv>(-p_beta, -p_beta + 1, depth - 4, false,
                               moved_position, thread_info, TT);
      }

      ss_pop(thread_info);
      thread_info.phase = phase;

      if (score >= p_beta) {
        if (!singular_search) {
          insert_entry(entry, hash, depth - 4, best_move, raw_eval,
                       score_to_tt(best_score, ply), EntryTypes::LBound,
                       thread_info.searches);
        }
        mcts_sample(thread_info, ply, score);
        return score;
      }
    }
  }
  Move quiets[64];
  int num_quiets = 0;
  Move captures[64];
  int num_captures = 0;
  thread_info.KillerMoves[ply + 1] = MoveNone;
  thread_info.FailHighCount[ply + 2] = 0;

  MovePicker picker;
  init_picker(picker, position, -107, in_check, ss);

  int moves_played = 0; // Generate and score moves
  bool is_capture = false, skip = false;

  while (Move move = next_move(picker, position, thread_info, tt_move, skip)) {
    // skip various excluded moves

    if (root) {
      if (!std::count(thread_info.root_moves.begin() +
                          thread_info.multipv_index,
                      thread_info.root_moves.begin() + thread_info.pvLast,
                      move)) {
        continue;
      }
    }

    if (move == excluded_move) {
      continue;
    }
    if (!is_legal_fast_unchecked(position, move)) {
      continue;
    }

    uint64_t curr_nodes = thread_info.nodes;

    int hist_score =
        thread_info.HistoryScores[position.board[extract_from(move)]]
                                 [extract_to(move)];

    // ============================================================================
    // IMPROVEMENT 6: Armageddon-aware Move Ordering (OPTIMIZED)
    // ============================================================================
    // Penalize moves that would lead to repetition when Armageddon is enabled
    // OPTIMIZATION: Use key_after_move to avoid position copy for most moves
    if (armageddonMode && !root) {
      if (would_cause_repetition_after_move(position, move, thread_info)) {
        hist_score -=
            3000; // Heavy penalty in move ordering for repetition moves
      }
    }

    is_capture = is_cap(position, move);

    // Armageddon MCTS + Experience: skip non-singular quiets after a few moves
    if (!is_capture && !root && expSingular && move != expSingularMove &&
        moves_played > 3) {
      continue;
    }

    if (!is_capture && !is_pv && best_score > ScoreLost) {

      int lmr_depth = std::max(1, depth - LMRTable[depth][moves_played]);

      // Late Move Pruning (LMP): If we've searched enough moves, we can skip
      // the rest.

      if (depth < LMPDepth &&
          moves_played >= LMPBase + depth * depth / (2 - improving)) {
        skip = true;
      }

      // Futility Pruning (FP): If we're far worse than alpha and our move isn't
      // a good capture, we can skip the rest.

      if (!in_check && depth < FPDepth && picker.stage > Stages::Captures &&
          static_eval + FPMargin1 + FPMargin2 * lmr_depth < alpha) {
        skip = true;
      }

      // History Pruning: at low depths, we can prune quiets with very bad history (failed low many times)
      if (!is_pv && !is_capture && lmr_depth < HistPruningDepth &&
          hist_score < -4096 * lmr_depth) {
        skip = true;
      }
    }

    if (!root && best_score > ScoreLost && depth < SeePruningDepth) {

      int margin = is_capture ? SeePruningNoisyMargin : SeePruningQuietMargin;

      if (!SEE_cached(position, move, depth * margin, thread_info)) {
        // SEE pruning: if we are hanging material, prune under certain
        // conditions.
        continue;
      }
    }

    int extension = 0;

    // Crystal gameCycle extension (ADD): search harder for an escape
    if (addActive && gameCycle) {
      int drawValue = current_draw_score(position, thread_info, ply);
      if (is_pv || (ply > 2 && drawValue < 0))
        extension = 2;
    }

    // ============================================================================
    // IMPROVEMENT 7: Armageddon Extension for Draw-Avoiding Moves (OPTIMIZED)
    // ============================================================================
    // When in Armageddon mode and position is drawish, extend to find winning
    // resources OPTIMIZATION: Use key_after_move instead of Position copy to
    // avoid expensive copy
    if (armageddonMode && extension == 0) {
      // Check if position is drawish (high halfmoves or upcoming repetition)
      bool drawish = position.halfmoves > 50;
      if (!drawish && !root) {
        // Check if this move avoids a repetition using key_after_move (avoids
        // position copy)
        bool key_calculated;
        uint64_t key_after = key_after_move(position, move, &key_calculated);

        if (key_calculated) {
          // Use key-based repetition check (fast path)
          int halfmoves_after = position.halfmoves + 1;
          int game_ply = thread_info.game_ply;
          int end_index = std::max(game_ply - halfmoves_after, 0);
          bool causes_repetition = false;
          for (int i = game_ply - 1; i >= end_index; i -= 2) {
            if (key_after == thread_info.game_hist[i].position_key) {
              causes_repetition = true;
              break;
            }
          }
          if (!causes_repetition) {
            // This move doesn't cause repetition - might be an escape
            if (position.halfmoves > 30 || material_draw(position)) {
              drawish = true;
            }
          }
        } else {
          // Fallback: complex move (castling, en passant, promotion) - use
          // would_cause_repetition_after_move OPTIMIZATION: This function
          // already uses key_after_move internally and falls back to make_move
          // if needed
          if (!would_cause_repetition_after_move(position, move, thread_info)) {
            // This move doesn't cause repetition - might be an escape
            if (position.halfmoves > 30 || material_draw(position)) {
              drawish = true;
            }
          }
        }
      }
      if (drawish && depth < 6 && !is_capture) {
        extension = 1; // Extend to find winning resources
      }
    }

    // Singular Extensions (SE): If a search finds that the TT move is way
    // better than all other moves, extend it under certain conditions.
    // Skip when ADD gameCycle (Crystal).
    if (!root && ply < thread_info.current_iter * 2 &&
        !(addActive && gameCycle)) {
      if (!singular_search && depth >= SEDepth && move == tt_move &&
          abs(tt_score) < ScoreWin && tt_depth >= depth - 3 &&
          entry_type != EntryTypes::UBound) {

        int sBeta = tt_score - depth;
        thread_info.excluded_move = move;
        int sScore = search<false>(sBeta - 1, sBeta, (depth - 1) / 2, cutnode,
                                   position, thread_info, TT);

        if (sScore < sBeta) {
          if (!is_pv && sScore + SEDoubleExtMargin < sBeta &&
              ply < thread_info.current_iter) {

            // In some cases we can even double extend
            extension = 2 + (!is_capture && sScore < sBeta - SETripleExtMargin);
          } else {
            extension = 1;
          }
        } else if (sBeta >= beta) {
          // Multicut: If there was another move that beat beta, it's a sign
          // that we'll probably beat beta with a full search too.

          mcts_sample(thread_info, ply, sBeta);
          return sBeta;
        } else if (cutnode) {
          extension = -1;
        }
      }
    }

    Position moved_position = position;
    make_move(moved_position, move);

    update_nnue_state(thread_info, move, position, moved_position);

    ss_push(position, thread_info, move);

    bool full_search = false;
    int newdepth = std::min(depth - 1 + extension, 126);

    // Late Move Reductions (LMR): Moves ordered later in search and at high
    // depths can be searched to a lesser depth than normal. If the reduced
    // search beats alpha, we'll have to search again, but most moves don't,
    // making this technique more than worth it.
    // If that beats alpha, we search at normal depth with null window
    // If that also beats alpha, we search at normal depth with full window.

    // Original Patricia LMR; ADD/Crystal restricts LMR under gameCycle / king danger
    bool allowLMR = depth >= LMRMinDepth;
    if (addActive) {
      allowLMR = depth >= LMRMinDepth && !gameCycle &&
                 (!kingDangerThem || ply > 6) && (!is_pv || ply > 1);
    }

    if (allowLMR && moves_played > is_pv) {
      int R = LMRTable[depth][moves_played];
      if (is_capture) {
        // Captures get LMRd less because they're the most likely moves to beat
        // alpha/beta
        R /= 2;
      } else {
        R -= hist_score / HistDiv;
      }

      // Decrease reduction if in pv
      R -= is_pv;

      // Decrease reduction if we have searched this position at a higher depth before
      R -= (tt_hit && tt_depth >= depth);

      // Increase reduction if not improving
      R += !improving;

      // Increase reduction in an expected cutnode
      R += cutnode;

      // Decrease reduction for checks
      R -= (attacks_square(moved_position, get_king_pos(position, color ^ 1),
                           color) != 0);

      // Increase reduction if opponent has had many refutations so far
      R += (thread_info.FailHighCount[ply + 1] > 4);

      R = std::clamp(R, 0, newdepth - 1);

      int lmr_depth = newdepth - R;

      // Reduced search, reduced window
      score = -search<false>(-alpha - 1, -alpha, lmr_depth, true,
                             moved_position, thread_info, TT);
      if (score > alpha) {
        full_search = R > 0;
        newdepth += (score > (best_score + 60 + newdepth * 2));
        newdepth -= (score < best_score + newdepth && !root);
      }
    } else {
      full_search = moves_played || !is_pv;
    }
    if (full_search) {
      // Full search, null window
      score = -search<false>(-alpha - 1, -alpha, newdepth, !cutnode,
                             moved_position, thread_info, TT);
    }
    if ((score > alpha || !moves_played) && is_pv) {
      // Full search, full window
      score = -search<true>(-beta, -alpha, newdepth, false, moved_position,
                            thread_info, TT);
    }

    ss_pop(thread_info);
    thread_info.phase = phase;

    if (thread_data.stop || thread_info.datagen_stop) {
      // return if we ran out of time for search
      return best_score;
    }

    if (root) {
      auto *rm = find_root_move(thread_info, move);
      rm->nodes += (thread_info.nodes - curr_nodes);
      rm->effort += (thread_info.nodes - curr_nodes);

      // Armageddon MCTS: flush per-move visit/score samples into root move
      if (armageddonMcts) {
        rm->visits += thread_info.mctsVisits;
        rm->zScore += thread_info.mctsAllScores;
        thread_info.mctsVisits = 0;
        thread_info.mctsAllScores = 0;
      }

      rm->averageScore = rm->averageScore != ScoreNone
                             ? (score + rm->averageScore) / 2
                             : score;
      constexpr int64_t MSS_UNSET =
          static_cast<int64_t>(-ScoreNone) * ScoreNone;
      rm->meanSquaredScore =
          rm->meanSquaredScore != MSS_UNSET
              ? (static_cast<int64_t>(score) * abs(score) +
                 rm->meanSquaredScore) /
                    2
              : static_cast<int64_t>(score) * abs(score);

      if (moves_played == 0 || score > alpha) {
        rm->score = score;
        rm->selDepth = thread_info.seldepth;
        rm->pv.resize(1);
        rm->pv[0] = move;
        for (int j = 0; j < MaxSearchDepth - 1; j++) {
          Move pm = thread_info.pv[MaxSearchDepth + j];
          if (pm == MoveNone)
            break;
          rm->pv.push_back(pm);
        }
        if (moves_played > 0 && thread_info.multipv_index == 0)
          thread_info.bestMoveChanges++;
      } else {
        rm->score = ScoreNone;
      }
    }

    if (score > best_score) {
      best_score = score;

      if (score > alpha) {
        best_move = move;
        raised_alpha = true;
        alpha = score;

        if (score >= beta) {
          break;
        }

        else {

          thread_info.pv[pv_index] = best_move;
          for (int n = 0; n < MaxSearchDepth + ply + 1; n++) {
            thread_info.pv[pv_index + 1 + n] =
                thread_info.pv[pv_index + MaxSearchDepth + n];
          }
        }
      }
    }

    if (is_capture) {
      if (num_captures < 64)
        captures[num_captures++] = move;
    } else {
      if (num_quiets < 64)
        quiets[num_quiets++] = move;
    }

    moves_played++;
  }

  if (root) {
    if (best_move != MoveNone) {
      thread_info.best_moves[thread_info.multipv_index] = best_move;
    }
    thread_info.best_scores[thread_info.multipv_index] = best_score;

    std::stable_sort(thread_info.root_moves.begin() +
                         thread_info.multipv_index,
                     thread_info.root_moves.begin() + thread_info.pvLast);
  }

  if (best_score == ScoreNone) { // handle no legal moves (stalemate/checkmate)
    if (singular_search) {
      mcts_sample(thread_info, ply, alpha);
      return alpha;
    }
    if (in_check) {
      int mate = ply - ScoreMate;
      mcts_sample(thread_info, ply, mate);
      return mate; // Checkmate
    }
    int drawValue = armageddonMode ? current_draw_score(position, thread_info, ply) : 0;
    mcts_sample(thread_info, ply, drawValue);
    return drawValue;
  }

  if (best_score >= beta) {

    thread_info.FailHighCount[ply]++;

    int piece = position.board[extract_from(best_move)],
        sq = extract_to(best_move);

    int bonus = std::min(
        (int)HistBonus * (depth - 1 + (best_score > beta + 125)), (int)HistMax);

    // Update history scores and the killer move.

    if (is_capture) {

      update_history(thread_info.CapHistScores[piece][sq], bonus);

    } else {

      Move their_last = extract_to((ss - 1)->played_move);

      int their_piece = (ss - 1)->piece_moved;

      Move our_last = extract_to((ss - 2)->played_move);
      int our_piece = (ss - 2)->piece_moved;

      for (int i = 0; i < num_quiets; i++) {

        // Every quiet move that *didn't* raise beta gets its history score
        // reduced

        Move move = quiets[i];

        int piece_m = position.board[extract_from(move)],
            sq_m = extract_to(move);

        int malus = -bonus * 15 / (10 + std::min(i, 30));

        update_history(thread_info.HistoryScores[piece_m][sq_m], malus);

        update_history(
            thread_info.ContHistScores[their_piece][their_last][piece_m][sq_m],
            malus);

        update_history(
            thread_info.ContHistScores[our_piece][our_last][piece_m][sq_m],
            malus);

        update_history(
            thread_info.ContHistScores[(ss - 4)->piece_moved][extract_to(
                (ss - 4)->played_move)][piece_m][sq_m],
            malus / 2);
      }

      update_history(thread_info.HistoryScores[piece][sq], bonus);

      update_history(
          thread_info.ContHistScores[their_piece][their_last][piece][sq],
          bonus);
      update_history(thread_info.ContHistScores[our_piece][our_last][piece][sq],
                     bonus);
      update_history(
          thread_info.ContHistScores[(ss - 4)->piece_moved][extract_to(
              (ss - 4)->played_move)][piece][sq],
          bonus / 2);

      thread_info.KillerMoves[ply] = best_move;
    }

    for (int i = 0; i < num_captures; i++) {
      Move move = captures[i];

      int piece_m = position.board[extract_from(move)], sq_m = extract_to(move);

      update_history(thread_info.CapHistScores[piece_m][sq_m], -bonus);
    }
  }

  if (is_pv)
    score = std::min(score, max_score);

  entry_type = best_score >= beta ? EntryTypes::LBound
               : raised_alpha     ? EntryTypes::Exact
                                  : EntryTypes::UBound;

  bool best_capture = is_cap(position, best_move);

  // update various correction histories
  
  if (!in_check && (!best_move || !best_capture) &&
      !(best_score >= beta && best_score <= ss->static_eval) &&
      !(!best_move && best_score >= ss->static_eval)) {

    int bonus =
        std::clamp((best_score - ss->static_eval) * depth / 8, -256, 256);

    update_corrhist(
        thread_info.PawnCorrHist[color][get_corrhist_index(position.pawn_key)],
        bonus);
    update_corrhist(
        thread_info.NonPawnCorrHist[color][Colors::White][get_corrhist_index(
            position.non_pawn_key[Colors::White])],
        bonus);
    update_corrhist(
        thread_info.NonPawnCorrHist[color][Colors::Black][get_corrhist_index(
            position.non_pawn_key[Colors::Black])],
        bonus);

    if ((ss - 1)->played_move && (ss - 2)->played_move) {
      update_corrhist(
          thread_info.ContCorrHist[(ss - 2)->piece_moved][extract_to(
              (ss - 2)->played_move)][(ss - 1)->piece_moved]
                                  [extract_to((ss - 1)->played_move)],
          bonus);
    }
  }

  // Add the search results to the TT, accounting for mate scores
  if (!singular_search) {
    insert_entry(entry, hash, depth, best_move, raw_eval,
                 score_to_tt(best_score, ply), entry_type,
                 thread_info.searches);
  }

  mcts_sample(thread_info, ply, best_score);
  return best_score;
}

// Cardanobile playout: follow TT line with reduced-depth searches to fill TT
// beyond the search horizon. Does not change bestmove selection.
inline int armageddon_mcts_playout(Position &position, ThreadInfo &thread_info,
                                   std::vector<TTBucket> &TT, Move playMove,
                                   int playoutValue, int rootDepth) {
  if (!armageddonMcts || thread_data.stop || thread_info.datagen_stop)
    return ScoreNone;
  if (playMove == MoveNone || !is_legal_fast_unchecked(position, playMove))
    return ScoreNone;
  if (is_draw(position, thread_info))
    return current_draw_score(position, thread_info, thread_info.search_ply);

  // ADD BlackHole: score confirmed drawish branches in playout
  if (add_detection_active(thread_info) &&
      DrawDetect::drawish_branch(position, thread_info, thread_info.search_ply,
                                 thread_info.addBlackHole,
                                 thread_info.addStrongerSide,
                                 thread_info.addTestPassivePlay != 0))
    return current_draw_score(position, thread_info, thread_info.search_ply);

  const uint8_t phase = thread_info.phase;
  Position moved = position;
  make_move(moved, playMove);
  update_nnue_state(thread_info, playMove, position, moved);
  ss_push(position, thread_info, playMove);

  int d = rootDepth * rootDepth / (rootDepth + 4) - 2;
  int newDepth = std::max(1, d);

  bool tt_hit = false;
  TTEntry *tte =
      &probe_entry(moved.zobrist_key, tt_hit, thread_info.searches, TT);

  if (!tt_hit) {
    std::array<Move, ListSize> legal_moves;
    if (legal_movegen(moved, legal_moves) > 0) {
      playoutValue =
          -search<false>(-playoutValue, -playoutValue + 1, newDepth, true,
                         moved, thread_info, TT);
      tte = &probe_entry(moved.zobrist_key, tt_hit, thread_info.searches, TT);
    }
  }

  Move ttMove = tt_hit ? tte->best_move : MoveNone;
  if (ttMove != MoveNone && thread_info.search_ply < MaxSearchDepth - 2 &&
      abs(playoutValue) < ScoreWin) {
    playoutValue = -armageddon_mcts_playout(moved, thread_info, TT, ttMove,
                                            -playoutValue, rootDepth);
  }

  ss_pop(thread_info);
  thread_info.phase = phase;
  return playoutValue;
}

void print_pv(Position &position, ThreadInfo &thread_info) {
  Position temp_pos = position;

  int indx = 0;

  while (thread_info.pv[indx] != MoveNone) {

    if (indx == 3 && thread_info.is_human) {
      thread_info.pv_material[thread_info.multipv_index] =
          -material_eval(temp_pos);
    }

    Move best_move = thread_info.pv[indx];

    // Verify that the pv move is possible and legal by generating moves

    MoveInfo moves;
    int movelen = legal_movegen(temp_pos, moves.moves);

    bool found_move = false;

    for (int i = 0; i < movelen; i++) {
      if (moves.moves[i] == best_move) {
        found_move = true;
        break;
      }
    }

    if (!found_move) {
      break;
    }

    printf("%s ", internal_to_uci(temp_pos, best_move).c_str());

    make_move(temp_pos, best_move);

    indx++;
  }

  printf("\n");
}

// ADD BlackHole: per-thread root toggle + TestPassivePlay.
// Activation (Patricia target behavior):
//   - near positional 0 (catch 0.00 / equal fortresses even under Armageddon)
//   - OR near current_draw_score (already scored as draw / Armageddon draw)
//   - sticky while aspiration window still covers 0 or scored-draw
// Scoring of drawish branches remains current_draw_score (Armageddon-aware).
inline void add_prepare_root_depth(Position &position, ThreadInfo &thread_info,
                                   int depth, int alpha, int beta) {
  if (!add_detection_active(thread_info))
    return;

  const int npm = DrawDetect::npm_color(position, Colors::White) +
                  DrawDetect::npm_color(position, Colors::Black);
  const int prevScore =
      thread_info.addBhBestValues[std::max(0, depth - 1)];
  const int scoreDepth2 =
      thread_info.addBhBestValues[std::max(0, depth - 2)];
  const int scoredDraw = current_draw_score(position, thread_info, 0);

  if (npm <= DrawDetect::PassivePlayMaterialBorder) {
    if (thread_info.addTestPassivePlay != 0) {
      if (thread_info.addBlackHole &&
          ((thread_info.addTestPassivePlay > 0 &&
            prevScore > thread_info.addTestPassivePlay) ||
           (thread_info.addTestPassivePlay < 0 &&
            prevScore < thread_info.addTestPassivePlay)))
        thread_info.addTestPassivePlay = 0;
    }

    if (!thread_info.addTestPassivePlay && !thread_info.addBlackHole &&
        depth <= 40 && depth >= 20 &&
        thread_info.addBhBestValues[std::max(0, depth - 3)] >= prevScore - 30 &&
        (prevScore >= 2 * MaterialValues[PieceTypes::Pawn] ||
         prevScore <= -2 * MaterialValues[PieceTypes::Pawn]))
      thread_info.addTestPassivePlay = prevScore;

    if (std::abs(prevScore) >= ScoreWin / 2)
      thread_info.addTestPassivePlay = 0;

    if (thread_info.addTestPassivePlay != 0)
      thread_info.addStrongerSide =
          thread_info.addTestPassivePlay > 0 ? position.color
                                             : (position.color ^ 1);
  }

  const bool nearPositionalDraw =
      std::abs(prevScore) < DrawDetect::BhDrawBand;
  const bool nearScoredDraw =
      std::abs(prevScore - scoredDraw) < DrawDetect::BhDrawBand;
  const bool sticky =
      thread_info.addBlackHole &&
      ((alpha <= 0 && beta >= 0) ||
       (alpha <= scoredDraw && beta >= scoredDraw));

  thread_info.addBlackHole =
      (!thread_info.addTestPassivePlay || std::abs(scoreDepth2) < 16) &&
      (nearPositionalDraw || nearScoredDraw || sticky);

  if (thread_info.addBlackHole && !thread_info.addTestPassivePlay &&
      depth >= MaxSearchDepth / 2)
    thread_info.searches = (thread_info.searches + 1) % MaxAge;

  // Stronger side from positional sign (0-centered), so Armageddon penalty
  // magnitude does not invert fortress side detection.
  if (thread_info.addTestPassivePlay == 0 && prevScore != 0)
    thread_info.addStrongerSide =
        prevScore > 0 ? position.color : (position.color ^ 1);
}

inline void add_finish_root_depth(ThreadInfo &thread_info, Move currentBest) {
  if (!add_detection_active(thread_info))
    return;
  if (thread_info.addBlackHole && currentBest != MoveNone &&
      currentBest != thread_info.addBhLastBest)
    thread_info.searches = (thread_info.searches + 1) % MaxAge;
  if (currentBest != MoveNone)
    thread_info.addBhLastBest = currentBest;
}

void iterative_deepen(
    Position &position, ThreadInfo &thread_info,
    std::vector<TTBucket> &TT) { // Performs an iterative deepening search.

  thread_info.original_opt = thread_info.opt_time;
  thread_info.datagen_stop = false;
  thread_info.bestMoveChanges = 0;

  // Every thread resets its own ADD/BH state (SMP-safe, identical algorithm).
  if (add_detection_active(thread_info)) {
    thread_info.addBlackHole = false;
    thread_info.addTestPassivePlay = 0;
    thread_info.addStrongerSide = position.color;
    thread_info.addBhBestValues.fill(0);
    thread_info.addBhLastBest = MoveNone;
  } else {
    thread_info.addBlackHole = false;
    thread_info.addTestPassivePlay = 0;
  }
  thread_info.phase = total_mat(position, thread_info) < PhaseBound ? PhaseTypes::Endgame
                                                       : PhaseTypes::Sacrifice;
  thread_info.nnue_state.reset_nnue(position, thread_info.phase);
  calculate(position);
  thread_info.nodes = 0;
  thread_info.tb_hits = 0;
  thread_info.time_checks = 0;

  if (thread_info.bestPreviousScore == ScoreNone)
    thread_info.iterValue.fill(0);
  else
    thread_info.iterValue.fill(thread_info.bestPreviousScore);
  thread_info.search_ply = 0; // reset all relevant thread_info
  thread_info.excluded_move = MoveNone;
  thread_info.best_moves = {0};
  thread_info.best_scores = {ScoreNone, ScoreNone, ScoreNone, ScoreNone,
                             ScoreNone};
  std::memset(&thread_info.KillerMoves, 0, sizeof(thread_info.KillerMoves));
  std::memset(&thread_info.FailHighCount, 0, sizeof(thread_info.FailHighCount));

  // Prepare root moves
  thread_info.root_moves.reserve(ListSize);
  thread_info.root_moves.clear();
  {
    std::array<Move, ListSize> raw_root_moves;
    int nmoves = legal_movegen(position, raw_root_moves);
    for (int i = 0; i < nmoves; i++) {
      thread_info.root_moves.emplace_back(raw_root_moves[i]);
    }
  }
  thread_info.pvLast = static_cast<uint16_t>(thread_info.root_moves.size());

  // Experience Book: Book move selection from HypnoS
  // If Experience Book is enabled and position is in the book, skip search
  Move expBookMove = MoveNone;
  bool useExpBookMove = false;

  if (Learning::enabled() && !thread_info.doing_datagen &&
      Learning::get_experience_book_enabled() &&
      thread_info.game_ply / 2 < Learning::get_experience_book_max_moves()) {

    const int expBookMinDepth = Learning::get_experience_book_min_depth();
    const int expBookWidth = Learning::get_experience_book_width();
    const int evalImportance = Learning::get_experience_book_eval_importance();

    const auto *exp = Learning::probe(position.zobrist_key);

    if (exp) {
      // Collect all entries that meet min depth requirement with their quality
      // Using positional quality calculation with lookahead (like HypnoS)
      std::vector<std::pair<const Learning::ExpEntryEx *, int>> qualityList;
      const auto *temp = exp;

      while (temp) {
        if (temp->depth >= static_cast<int32_t>(expBookMinDepth)) {
          // Use positional quality with lookahead and draw detection (like
          // HypnoS) - now uses full is_draw with repetition detection
          auto [q, maybeDraw] = temp->quality(
              position, evalImportance,
              [](Position &pos, uint16_t mv) {
                make_move(pos, static_cast<Move>(mv));
              },
              [&thread_info](const Position &pos) {
                return is_draw(pos, thread_info);
              });
          if (q > 0 && !maybeDraw) {
            qualityList.emplace_back(temp, q);
          }
        }
        temp = temp->next;
      }

      if (!qualityList.empty()) {
        // Sort by quality (highest first)
        std::sort(
            qualityList.begin(), qualityList.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });

        // Select move based on width
        if (expBookWidth > 1 && qualityList.size() > 1) {
          // Random selection from top 'width' moves
          static std::mt19937 rng(static_cast<unsigned>(
              std::chrono::steady_clock::now().time_since_epoch().count()));
          int maxIndex =
              std::min<int>(expBookWidth, static_cast<int>(qualityList.size()));
          std::uniform_int_distribution<int> dist(0, maxIndex - 1);
          expBookMove = static_cast<Move>(qualityList[dist(rng)].first->move);
        } else {
          // Best move only
          expBookMove = static_cast<Move>(qualityList.front().first->move);
        }

        // Verify move is in root moves (legal)
        for (size_t i = 0; i < thread_info.root_moves.size(); i++) {
          if (thread_info.root_moves[i].move == expBookMove) {
            useExpBookMove = true;
            // Move to front
            std::swap(thread_info.root_moves[0], thread_info.root_moves[i]);
            break;
          }
        }
      }
    }
  }

  // If we have an Experience Book move, output it and skip search
  if (useExpBookMove && thread_info.thread_id == 0) {
    printf("info depth 1 seldepth 1 score cp 0 nodes 0 nps 0 time 0 pv %s\n",
           internal_to_uci(position, expBookMove).c_str());
    printf("bestmove %s\n", internal_to_uci(position, expBookMove).c_str());
    return;
  }

  // If experience enabled but not book mode, still use experience for move
  // ordering
  if (Learning::enabled() && !thread_info.doing_datagen && !useExpBookMove) {
    const auto *bestExp = Learning::find_best_entry(position.zobrist_key);
    if (bestExp && bestExp->depth >= static_cast<int32_t>(Learning::MinDepth)) {
      Move expMove = static_cast<Move>(bestExp->move);
      // Move to front of root moves for better ordering
      for (size_t i = 0; i < thread_info.root_moves.size(); i++) {
        if (thread_info.root_moves[i].move == expMove) {
          std::swap(thread_info.root_moves[0], thread_info.root_moves[i]);
          break;
        }
      }
    }
  }

  int lastBestMoveDepth = 0;
  Move lastBestMove = MoveNone;
  int iterIdx = 0;
  double totBestMoveChanges = 0;
  double lastTimeReduction = 1.0;
  int searchAgainCounter = 0;

  // Short rolling window of recently completed iteration scores, used to
  // smooth out single-iteration noise in the score-drop time factor.
  std::array<int, 3> score_hist{};
  int score_hist_count = 0;

  for (int depth = 1; depth <= thread_info.max_iter_depth; depth++) {

    if (!thread_data.increaseDepth)
      searchAgainCounter++;

    if (thread_info.thread_id == 0)
      totBestMoveChanges /= 2;

    thread_info.current_iter = depth;

    // Armageddon MCTS: reset per-iteration thread sample counters
    if (armageddonMcts) {
      thread_info.mctsVisits = 0;
      thread_info.mctsAllScores = 0;
    }

    int real_multi_pv =
        std::min<int>(thread_info.multipv, thread_info.root_moves.size());

    // Save previous iteration's scores for aspiration window centering
    for (auto &rm : thread_info.root_moves)
      rm.previousScore = rm.score;

    int pvFirst = 0;
    thread_info.pvLast =
        static_cast<uint16_t>(thread_info.root_moves.size());

    for (thread_info.multipv_index = 0;
         thread_info.multipv_index < real_multi_pv;
         thread_info.multipv_index++) {

      int temp_depth = depth;

      // FIX #1: Per-PV aspiration window based on this line's own statistics
      auto &curRM = thread_info.root_moves[thread_info.multipv_index];
      int delta;
      int alpha, beta;

      if (depth > 6 && curRM.averageScore != ScoreNone) {
        constexpr int64_t MSS_UNSET =
            static_cast<int64_t>(-ScoreNone) * ScoreNone;
        delta = AspStartWindow + (thread_info.thread_id % 6);
        if (curRM.meanSquaredScore != MSS_UNSET)
          delta += static_cast<int>(
              (curRM.meanSquaredScore < 0 ? -curRM.meanSquaredScore
                                          : curRM.meanSquaredScore) /
              9000);
        int avg = curRM.averageScore;
        alpha = std::max(avg - delta, static_cast<int>(ScoreNone));
        beta = std::min(avg + delta, static_cast<int>(-ScoreNone));
      } else {
        delta = AspStartWindow;
        alpha = ScoreNone;
        beta = -ScoreNone;
      }

      if (add_detection_active(thread_info) &&
          thread_info.multipv_index == 0)
        add_prepare_root_depth(position, thread_info, depth, alpha, beta);

      thread_info.seldepth = 0;

      int score =
          search<true>(alpha, beta, depth, false, position, thread_info, TT);

      // Aspiration Windows: HypnoS-style while(true) loop with explicit breaks.
      // Critical: on stop, we BREAK (not goto finish) so that the sort and
      // output code after the loop still runs.
      int asp_iterations = 0;
      const int max_asp_iterations = 50;

      while (true) {
        if (score > alpha && thread_info.thread_id == 0)
          thread_data.stopOnPonderhit = false;

        if (thread_data.stop || thread_info.datagen_stop)
          break;

        // Armageddon MCTS: also fail low if visit-averaged score is weak
        const size_t rmIdx = static_cast<size_t>(thread_info.multipv_index);
        const bool mctsFailLow =
            armageddonMcts && rmIdx < thread_info.root_moves.size() &&
            thread_info.root_moves[rmIdx].visits > 0 &&
            (thread_info.root_moves[rmIdx].zScore /
             thread_info.root_moves[rmIdx].visits) <= (alpha - 50);

        if (score > alpha && score < beta && !mctsFailLow)
          break;

        if (asp_iterations >= max_asp_iterations)
          break;

        asp_iterations++;

        if (asp_iterations > 20) {
          alpha = ScoreNone;
          beta = -ScoreNone;
          temp_depth = depth;
          delta = AspStartWindow;
        }

        if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
            real_multi_pv == 1 &&
            !(thread_info.is_human && thread_info.multipv_index)) {
          std::string bound_string =
              score >= beta ? "lowerbound" : "upperbound";

          uint64_t nodes = thread_info.nodes;
          for (auto &td : thread_data.thread_infos)
            nodes += td.nodes;

          int64_t search_time = time_elapsed(thread_info.start_time);
          int64_t nps = search_time
                            ? static_cast<int64_t>(nodes) * 1000 / search_time
                            : 123456789;

          auto &pvRM = thread_info.root_moves[thread_info.multipv_index];
          Move asp_move = pvRM.pv.empty() ? MoveNone : pvRM.pv[0];

          printf("info multipv %i depth %i seldepth %i score cp %i %s nodes "
                 "%" PRIu64 " nps %" PRIi64 " time %" PRIi64 " pv %s\n",
                 thread_info.multipv_index + 1, depth, thread_info.seldepth,
                 score * 100 / NormalizationFactor, bound_string.c_str(),
                 nodes, nps, search_time,
                 internal_to_uci(position, asp_move).c_str());
        }

        if (!add_detection_active(thread_info) && !armageddonMcts) {
          // Pristine baseline Patricia aspiration loop (zero modifications when options are OFF)
          if (score <= alpha) {
            beta = (alpha + beta) / 2;
            alpha -= delta;
            temp_depth = depth;
          } else if (score >= beta) {
            beta += delta;
            temp_depth = std::max(temp_depth - 1, 1);
          }
          delta += delta / 3;
        } else {
          // Armageddon Draw Detection (BlackHole) & Armageddon MCTS aspiration logic
          if (score <= alpha || mctsFailLow) {
            if (score <= alpha && thread_info.addBlackHole &&
                add_detection_active(thread_info) && alpha < 1) {
              beta = 1;
            } else {
              beta = (alpha + beta) / 2;
            }
            alpha = std::max(alpha - delta, ScoreNone);
            temp_depth = depth;
          } else if (score >= beta) {
            if (thread_info.addBlackHole && add_detection_active(thread_info) &&
                beta > -1) {
              alpha = -1;
            }
            beta = std::min(beta + delta, -ScoreNone);
            temp_depth = std::max(temp_depth - 1, 1);
          }
          delta += delta / 3;

          if (alpha >= beta) {
            alpha = ScoreNone;
            beta = -ScoreNone;
          }
        }

        score = search<true>(alpha, beta, temp_depth, false, position,
                             thread_info, TT);
      }

      if (add_detection_active(thread_info) &&
          thread_info.multipv_index == 0) {
        thread_info.addBhBestValues[depth] = score;
        Move best = thread_info.root_moves.empty() ||
                            thread_info.root_moves[0].pv.empty()
                        ? MoveNone
                        : thread_info.root_moves[0].pv[0];
        add_finish_root_depth(thread_info, best);
      }

      // Sort PV lines searched so far by score (like HypnoS line 846)
      std::stable_sort(thread_info.root_moves.begin() + pvFirst,
                       thread_info.root_moves.begin() +
                           thread_info.multipv_index + 1);

      // Sync best_moves and best_scores for ALL requested PV lines
      // (non-searched lines retain previous depth data via previousScore)
      for (int i = 0; i < real_multi_pv; i++) {
        auto &rm_sync = thread_info.root_moves[i];
        thread_info.best_moves[i] =
            rm_sync.pv.empty() ? MoveNone : rm_sync.pv[0];
        thread_info.best_scores[i] = rm_sync.score;
      }

      if (thread_info.thread_id == 0) {
        uint64_t tb_hits = thread_info.tb_hits;
        uint64_t nodes = thread_info.nodes;

        for (auto &td : thread_data.thread_infos) {
          nodes += td.nodes;
          tb_hits += td.tb_hits;
        }

        int64_t search_time = time_elapsed(thread_info.start_time);
        int64_t nps;
        if (search_time) {
          nps = static_cast<int64_t>(nodes) * 1000 / search_time;
        } else {
          int wezly = 10000000;
          wezly += (wezly / 7);
          nps = wezly;
        }

        // Output ALL MultiPV lines in sorted order (like HypnoS pv())
        // HypnoS outputs all lines (0..multiPV-1), using previousScore for
        // lines not yet searched at this depth.
        if (!thread_info.doing_datagen &&
            (thread_data.stop ||
             thread_info.multipv_index + 1 == real_multi_pv ||
             search_time > 5000)) {

          int saved_mpv_idx = thread_info.multipv_index;

          for (int i = 0; i < real_multi_pv; i++) {
            auto &rm_out = thread_info.root_moves[i];
            bool updated = rm_out.score != ScoreNone;

            if (depth == 1 && !updated && i > 0)
              continue;

            int d = updated ? depth : std::max(1, depth - 1);
            int v = updated ? rm_out.score : rm_out.previousScore;
            if (v == ScoreNone)
              v = 0;

            std::string eval_string;
            if (abs(v) <= ScoreTbWin) {
              eval_string =
                  "cp " + std::to_string(v * 100 / NormalizationFactor);
            } else if (v > 0) {
              eval_string =
                  "mate " + std::to_string((ScoreMate - v + 1) / 2);
            } else {
              eval_string =
                  "mate " + std::to_string((-ScoreMate - v) / 2);
            }

            thread_info.multipv_index = i;
            std::memset(&thread_info.pv, 0, sizeof(thread_info.pv));
            for (size_t j = 0;
                 j < rm_out.pv.size() &&
                 j < static_cast<size_t>(MaxSearchDepth);
                 j++) {
              thread_info.pv[j] = rm_out.pv[j];
            }

            printf(
                "info multipv %i depth %i seldepth %i score %s nodes %" PRIu64
                " nps %" PRIi64 " tbhits %" PRIu64 " time %" PRIi64 " pv ",
                i + 1, d,
                rm_out.selDepth > 0 ? rm_out.selDepth : thread_info.seldepth,
                eval_string.c_str(), nodes, nps, tb_hits, search_time);
            print_pv(position, thread_info);
          }

          thread_info.multipv_index = saved_mpv_idx;
        }

        if (thread_info.doing_datagen) {
          thread_info.best_scores[0] =
              thread_info.root_moves[0].score * 100 / NormalizationFactor;
        }

        if (nodes > thread_info.opt_nodes_searched) {
          if (thread_info.doing_datagen) {
            thread_info.datagen_stop = true;
          } else {
            thread_data.stop = true;
          }
        }

        else if (thread_info.multipv == 1 && score != ScoreNone) {
          totBestMoveChanges += thread_info.bestMoveChanges;
          thread_info.bestMoveChanges = 0;
          for (auto &td : thread_data.thread_infos) {
            totBestMoveChanges += td.bestMoveChanges;
            td.bestMoveChanges = 0;
          }

          double timeReductionResult;
          const int tm_score = thread_info.root_moves[0].score;
          int avg_prev_score = tm_score;
          if (score_hist_count > 0) {
            int n = std::min<int>(score_hist_count, score_hist.size());
            int sum = 0;
            for (int i = 0; i < n; i++) {
              sum += score_hist[i];
            }
            avg_prev_score = sum / n;
          }
          double totalTime = compute_soft_limit(
              thread_info, depth, tm_score, iterIdx,
              lastBestMoveDepth, totBestMoveChanges,
              thread_data.num_threads, timeReductionResult, tm_score,
              avg_prev_score);
          lastTimeReduction = timeReductionResult;

          thread_info.opt_time = static_cast<uint32_t>(
              std::min(totalTime, (double)thread_info.max_time));

          if (search_time >
              std::min(totalTime, (double)thread_info.max_time)) {
            if (thread_data.ponder) {
              thread_data.stopOnPonderhit = true;
              thread_data.increaseDepth = true;
            } else if (thread_info.doing_datagen) {
              thread_info.datagen_stop = true;
            } else {
              thread_data.stop = true;
            }
          } else {
            thread_data.increaseDepth =
                thread_data.ponder.load() || search_time <= totalTime * 0.50;
          }
        }

        else if (search_time > thread_info.opt_time) {
          if (thread_data.ponder) {
            thread_data.stopOnPonderhit = true;
          } else if (thread_info.doing_datagen) {
            thread_info.datagen_stop = true;
          } else {
            thread_data.stop = true;
          }
        }

        score_hist[score_hist_count % score_hist.size()] = score;
        score_hist_count++;

        if (depth >= 6 && total_mat(position) >= PhaseBound) {
          if (thread_info.best_scores[0] < -20) {
            thread_info.phase = PhaseTypes::Endgame;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          } else if (thread_info.best_scores[0] > 400) {
            thread_info.phase = PhaseTypes::Sacrifice;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          } else {
            thread_info.phase = PhaseTypes::Middlegame;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          }
        }
      }

      if (thread_data.stop || thread_info.datagen_stop) {
        goto finish;
      }

      if (score == ScoreNone) {
        break;
      }
    }

    // Track lastBestMoveDepth outside the multipv loop (like HypnoS line 877)
    if (!thread_info.root_moves.empty() &&
        !thread_info.root_moves[0].pv.empty() &&
        thread_info.root_moves[0].pv[0] != lastBestMove) {
      lastBestMove = thread_info.root_moves[0].pv[0];
      lastBestMoveDepth = depth;
    }

    // Armageddon MCTS: Cardanobile playout after completed ID iteration
    if (armageddonMcts && thread_info.thread_id == 0 &&
        !thread_data.stop && !thread_info.datagen_stop &&
        lastBestMove != MoveNone) {
      int playoutScore = thread_info.root_moves.empty()
                             ? 0
                             : thread_info.root_moves[0].score;
      if (playoutScore == ScoreNone)
        playoutScore = 0;
      armageddon_mcts_playout(position, thread_info, TT, lastBestMove,
                              playoutScore, depth);
    }

    // Mate search stop (DIFF 17, HypnoS line 888)
    if (thread_info.thread_id == 0 && thread_info.mate_limit > 0 &&
        !thread_info.root_moves.empty()) {
      int s = thread_info.root_moves[0].score;
      if ((s >= ScoreWin && ScoreMate - s <= 2 * thread_info.mate_limit) ||
          (s != ScoreNone && s <= ScoreLost &&
           ScoreMate + s <= 2 * thread_info.mate_limit))
        thread_data.stop = true;
    }

    // Update iterValue unconditionally (like HypnoS line 953)
    if (thread_info.thread_id == 0 && !thread_info.root_moves.empty()) {
      thread_info.iterValue[iterIdx] = thread_info.root_moves[0].score;
      iterIdx = (iterIdx + 1) % 4;
    }
  }

  // Busy wait for stop/ponderhit when pondering (HypnoS line 435)
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen) {
    while (!thread_data.stop && thread_data.ponder)
      ;
  }

finish:
  // Wait for all threads to finish BEFORE reading their data
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen) {
    thread_data.stop = true;
  }
  search_end_barrier.arrive_and_wait();

  // Persist cross-search state from the best thread (like HypnoS line 570)
  if (thread_info.thread_id == 0 && !thread_info.root_moves.empty()) {
    int bestScore = thread_info.root_moves[0].score;
    int bestDepth = thread_info.current_iter;
    ThreadInfo *bestSource = &thread_info;

    for (auto &td : thread_data.thread_infos) {
      if (td.root_moves.empty())
        continue;
      int s = td.root_moves[0].score;
      int d = td.current_iter;
      if (d > bestDepth || (d == bestDepth && s > bestScore)) {
        bestScore = s;
        bestDepth = d;
        bestSource = &td;
      }
    }

    thread_info.bestPreviousScore = bestSource->root_moves[0].score;
    thread_info.bestPreviousAverageScore =
        bestSource->root_moves[0].averageScore;
    thread_info.previousTimeReduction = lastTimeReduction;
  }

  // Save experience data (PV and MultiPV) - AFTER barrier to avoid race
  // condition. Matches HypnoS conditions exactly.
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
      !Learning::is_learning_paused() && Learning::enabled() &&
      !thread_data.is_frc &&   // Don't save for Chess960 (like HypnoS)
      !thread_info.is_human) { // Don't save for Skill Level mode (like HypnoS
                               // UCI_LimitStrength)
    // Depth fallback: if GUI requested fixed depth, use that as fallback
    // (like HypnoS limits.depth fallback)
    int effDepth = thread_info.current_iter;
    if (thread_info.max_iter_depth > 0 &&
        thread_info.max_iter_depth < MaxSearchDepth) {
      effDepth =
          std::max(effDepth, static_cast<int>(thread_info.max_iter_depth));
    }

    // Save PV experience
    if (!thread_info.best_moves.empty() &&
        thread_info.best_moves[0] != MoveNone) {
      if (effDepth >= static_cast<int>(Learning::MinDepth)) {
        Learning::add_pv_experience(
            position.zobrist_key, thread_info.best_moves[0],
            thread_info.best_scores[0], static_cast<uint8_t>(effDepth));
      }
    }

    // Save MultiPV experience
    if (thread_info.multipv > 1) {
      struct UniqueMoveInfo {
        Move move;
        uint8_t depth;
        int32_t scoreSum;
        int count;
      };

      std::vector<UniqueMoveInfo> uniqueMoves;
      Move bestMv = thread_info.best_moves[0];

      auto add_candidate_move = [&](Move m, uint8_t depth, int32_t score) {
        if (m == MoveNone || m == bestMv)
          return;

        UniqueMoveInfo thisMove{m, depth, score, 1};
        for (auto &um : uniqueMoves) {
          if (um.move == thisMove.move) {
            if (thisMove.depth > um.depth)
              um = thisMove;
            else if (thisMove.depth == um.depth) {
              um.scoreSum += thisMove.scoreSum;
              um.count++;
            }
            return;
          }
        }
        uniqueMoves.push_back(thisMove);
      };

      // 1. Primary source: thread 0's own MultiPV lines (root_moves lines 1..multipv-1)
      const int numRootMoves = static_cast<int>(thread_info.root_moves.size());
      const int maxPv = std::min(static_cast<int>(thread_info.multipv), numRootMoves);
      for (int i = 1; i < maxPv; ++i) {
        Move m = (i < static_cast<int>(thread_info.best_moves.size()))
                     ? thread_info.best_moves[i]
                     : MoveNone;
        if (m == MoveNone && !thread_info.root_moves[i].pv.empty())
          m = thread_info.root_moves[i].pv[0];

        int32_t score = (i < static_cast<int>(thread_info.best_scores.size()))
                            ? thread_info.best_scores[i]
                            : thread_info.root_moves[i].score;
        add_candidate_move(m, static_cast<uint8_t>(effDepth), score);
      }

      // 2. Secondary source: helper threads' best moves in SMP mode (like Stockfish-sf_19)
      for (auto &td : thread_data.thread_infos) {
        if (td.thread_id == 0)
          continue;

        if (!td.best_moves.empty() && td.best_moves[0] != MoveNone) {
          add_candidate_move(td.best_moves[0], static_cast<uint8_t>(td.current_iter),
                             td.best_scores[0]);
        }
      }

      // Commit MultiPV entries
      for (const auto &info : uniqueMoves) {
        if (info.depth >= Learning::MinDepth) {
          Learning::add_multipv_experience(position.zobrist_key, info.move,
                                           info.scoreSum / info.count,
                                           info.depth);
        }
      }
    }

    // Save experience to file
    Learning::save();
  }

  if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
      !thread_info.is_human) {
    std::string ponderMove;
    if (!thread_info.root_moves.empty() &&
        thread_info.root_moves[0].pv.size() > 1)
      ponderMove =
          " ponder " +
          internal_to_uci(position, thread_info.root_moves[0].pv[1]);

    printf("bestmove %s%s\n",
           internal_to_uci(position, thread_info.best_moves[0]).c_str(),
           ponderMove.c_str());
  }
}

void search_position(Position &position, ThreadInfo &thread_info,
                     std::vector<TTBucket> &TT) {

  thread_info.position = position;
  thread_info.thread_id = 0;
  thread_info.nodes = 0;
  thread_info.tb_hits = 0;

  int num_threads = thread_data.num_threads;

  // Wait for threads to be ready
  reset_barrier.arrive_and_wait();

  for (int i = 0; i < thread_data.thread_infos.size(); i++) {
    thread_data.thread_infos[i].copy_from(thread_info);
    thread_data.thread_infos[i].thread_id = i + 1;
  }

  // Tell threads to start
  idle_barrier.arrive_and_wait();

  thread_data.stop = false;
  thread_data.increaseDepth = true;
  thread_data.stopOnPonderhit = false;
  iterative_deepen(position, thread_info, TT);
  if (!thread_info.doing_datagen) {
    thread_data.stop = true;
  }

  thread_info.searches = (thread_info.searches + 1) % MaxAge;
}

void loop(int i) {
  while (true) {
    reset_barrier.arrive_and_wait();
    idle_barrier.arrive_and_wait();
    if (thread_data.terminate) {
      return;
    }
    thread_data.thread_infos[i].searching = true;
    iterative_deepen(thread_data.thread_infos[i].position,
                     thread_data.thread_infos[i], TT);
    thread_data.thread_infos[i].searching = false;
  }
}
