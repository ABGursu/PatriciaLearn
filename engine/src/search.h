#pragma once
#include "fathom/src/tbprobe.h"
#include "learning.h"
#include "movepick.h"
#include "nnue.h"
#include "params.h"
#include "position.h"
#include "tm.h"
#include "utils.h"
#include <cassert>
#include <random>
#include <array>
#include <atomic>
#include <mutex>

constexpr int NormalizationFactor = 195;

// ============================================================================
// FORTRESS DETECTION & ARMAGEDDON MODE (Crystal/HypnoS port)
// ============================================================================

// Global UCI option variables
inline bool fortressDetectionEnabled = false;  // UCI: Fortress Detection
inline bool armageddonMode = false;            // UCI: Armageddon (Base: Anti-Draw penalties)
inline bool armageddonPsychology = false;     // UCI: Armageddon Psychology (Complexity/Sharpness bonuses)
inline bool armageddonTbPenalty = false;       // UCI: Armageddon TB Penalty (penalize even TB draws)

// Setters for UCI options
inline void set_fortress_detection(bool enabled) { fortressDetectionEnabled = enabled; }
inline void set_armageddon_mode(bool enabled) { armageddonMode = enabled; }
inline void set_armageddon_psychology(bool enabled) { armageddonPsychology = enabled; }
inline void set_armageddon_tb_penalty(bool enabled) { armageddonTbPenalty = enabled; }

// Getters for UCI options
inline bool get_fortress_detection() { return fortressDetectionEnabled; }
inline bool get_armageddon_mode() { return armageddonMode; }
inline bool get_armageddon_psychology() { return armageddonPsychology; }
inline bool get_armageddon_tb_penalty() { return armageddonTbPenalty; }

// Armageddon draw penalty constant
constexpr int ArmageddonPenalty = ScoreMate / 2;

// ============================================================================
// MECHANISM 1: Dynamic Contempt - Scale penalty based on game phase
// ============================================================================
// Calculate phase-based penalty scaling (Endgame = less penalty)
inline int get_armageddon_penalty_scaled(int phase) {
  if (!armageddonMode) return ArmageddonPenalty;
  
  // Phase: 0=Middlegame (full penalty), 1=Endgame (reduced), 2=Sacrifice (full)
  // Scale: Middlegame=100%, Endgame=40%, Sacrifice=100%
  int scale;
  if (phase == PhaseTypes::Endgame) {
    scale = 40;  // Endgame: 40% penalty (6400 instead of 16000)
  } else {
    scale = 100;  // Middlegame/Sacrifice: full penalty
  }
  
  return (ArmageddonPenalty * scale) / 100;
}

// Draw value with Armageddon penalty (perspective-aware)
// In Armageddon mode, draws are bad for US (root side), good for opponent
// At even ply (our turn): return negative (bad for us)
// At odd ply (opponent's turn): return positive (good for them, which negates to bad for us at parent)
// tb_draw: if true, no penalty applied (unless armageddonTbPenalty is enabled)
// phase: game phase for dynamic contempt (optional, defaults to Middlegame)
inline int get_draw_value_armageddon(int ply, bool tb_draw = false, int phase = PhaseTypes::Middlegame) {
  if (armageddonMode && (!tb_draw || armageddonTbPenalty)) {
    // MECHANISM 1: Dynamic Contempt - scale penalty by phase
    int penalty = get_armageddon_penalty_scaled(phase);
    // Even ply = our turn (root side), odd ply = opponent's turn
    return (ply % 2 == 0) ? -penalty : penalty;
  }
  return 0;  // Normal draw value
}

// ============================================================================
// ARMAGEDDON: Rule 50 scaling - penalize positions approaching 50-move draw
// ============================================================================
// Returns scale factor 0-100. At 100 = no scaling, at 0 = full draw penalty
inline int armageddon_rule50_scale(int halfmoves) {
  if (!armageddonMode || halfmoves < 60) return 100;  // No scaling until move 60
  // From halfmove 60 onwards, gradually reduce eval toward draw
  // At 100 halfmoves, scale is 0 (full draw penalty)
  return std::max(0, 100 - (halfmoves - 60) * 100 / 40);
}

// Apply rule50 scaling to evaluation (Armageddon only)
inline int apply_armageddon_rule50_scaling(int eval, int halfmoves, int ply, int phase) {
  if (!armageddonMode || halfmoves < 60) return eval;
  int scale = armageddon_rule50_scale(halfmoves);
  if (scale >= 100) return eval;
  // Blend evaluation toward draw penalty
  int draw_penalty = get_draw_value_armageddon(ply, false, phase);
  return (eval * scale + draw_penalty * (100 - scale)) / 100;
}

// ============================================================================
// CUCKOO HASHING FOR UPCOMING REPETITION DETECTION
// ============================================================================
// Based on Crystal/Stockfish implementation
// See: http://web.archive.org/web/20201107002606/https://marcelk.net/2013-04-06/paper/upcoming-rep-v2.pdf

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
    case PieceTypes::Knight: return KnightAttacks[sq];
    case PieceTypes::Bishop: return get_bishop_attacks(sq, occ);
    case PieceTypes::Rook: return get_rook_attacks(sq, occ);
    case PieceTypes::Queen: return get_bishop_attacks(sq, occ) | get_rook_attacks(sq, occ);
    case PieceTypes::King: return KingAttacks[sq];
    default: return 0;
  }
}

// Initialize Cuckoo tables at engine startup
// CRITICAL FIX: Thread-safe initialization using double-checked locking pattern
// This prevents race conditions when multiple threads try to initialize simultaneously
inline void init_cuckoo() {
  // Fast path: check if already initialized (atomic read, no lock needed)
  if (cuckooInitialized.load(std::memory_order_acquire)) return;
  
  // Slow path: acquire lock and check again (double-checked locking)
  std::lock_guard<std::mutex> lock(cuckooInitMutex);
  
  // Check again after acquiring lock (another thread might have initialized it)
  if (cuckooInitialized.load(std::memory_order_relaxed)) return;
  
  cuckooKeys.fill(0);
  cuckooMoves.fill(MoveNone);
  
  // Generate all reversible piece moves (non-pawn pieces)
  // For each piece type and color, generate all possible from-to pairs
  for (int piece = Pieces::WKnight; piece <= Pieces::BKing; piece++) {
    int piece_type = get_piece_type(piece);
    if (piece_type == PieceTypes::Pawn) continue;  // Skip pawns
    
    for (int s1 = 0; s1 < 64; s1++) {
      uint64_t attacks = get_cuckoo_piece_attacks(piece_type, s1, 0ULL);
      
      while (attacks) {
        int s2 = pop_lsb(attacks);
        if (s2 <= s1) continue;  // Avoid duplicates (only store s1 < s2)
        
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
          if (move == MoveNone) break;  // Found empty slot
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
inline bool upcoming_repetition(const Position &position, ThreadInfo &thread_info) {
  int halfmoves = position.halfmoves;
  int game_ply = thread_info.game_ply;
  int search_ply = thread_info.search_ply;
  
  // CRITICAL FIX: Early return for initial position
  // Need at least 3 halfmoves for any repetition to be possible
  // game_ply starts at 6, so we need at least game_ply >= 8 (2 moves played) to check repetition
  // But also check that we have enough history entries
  if (halfmoves < 3)
    return false;
  
  // CRITICAL FIX: Ensure game_ply is within valid bounds for game_hist array access
  // game_ply starts at 6, so we need at least 2 moves (game_ply >= 8) to have history
  // Also ensure we don't access out-of-bounds (game_hist array size is GameSize)
  if (game_ply < 8 || game_ply - 2 < 0 || game_ply >= GameSize)
    return false;
  
  // Ensure Cuckoo tables are initialized (thread-safe)
  if (!cuckooInitialized.load(std::memory_order_acquire)) {
    init_cuckoo();
  }
    
  uint64_t originalKey = position.zobrist_key;
  int end = std::min((int)halfmoves, game_ply);
  
  // Simple hash-based check first (fast path)
  // Check if current position matches any position 2, 4, 6... moves ago
  int start_index = game_ply - 2;
  int end_index = std::max(game_ply - end, 0);
  
  // CRITICAL FIX: Ensure indices are valid (game_hist array bounds)
  // game_ply starts at 6, so valid indices are 0-5 initially
  if (start_index < 0 || end_index < 0)
    return false;
  
  for (int i = start_index; i >= end_index; i -= 2) {
    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (i < 0 || i >= GameSize) break;  // Safety check
    if (originalKey == thread_info.game_hist[i].position_key) {
      return true;
    }
  }
  
  // Cuckoo-based detection for reversible moves
  // This detects if ANY legal reversing move would create a repetition
  if (end >= 3 && game_ply >= 3) {
    uint64_t other = originalKey;
    
    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (game_ply >= 1 && (game_ply - 1) < GameSize) {
      other ^= thread_info.game_hist[game_ply - 1].position_key ^ zobrist_keys[side_index];
    }
    
    for (int i = 3; i <= end; i += 2) {
      int idx1 = game_ply - i;
      int idx2 = game_ply - i - 1;
      
      // CRITICAL FIX: Ensure indices are valid before accessing game_hist
      // Check both lower and upper bounds to prevent out-of-bounds access
      if (idx1 < 0 || idx2 < 0 || idx1 >= GameSize || idx2 >= GameSize) break;
      
      other ^= thread_info.game_hist[idx1].position_key ^ 
               thread_info.game_hist[idx2].position_key ^ 
               zobrist_keys[side_index];
      
      if (other != 0) continue;
      
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
          uint64_t occ = position.colors_bb[Colors::White] | position.colors_bb[Colors::Black];
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
inline bool would_cause_repetition(const Position &pos, ThreadInfo &thread_info) {
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
    if (i < 0 || i >= GameSize) break;  // Safety check
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
// For complex moves (castling, en passant, promotion), we fall back to make_move
// Returns true in *success if key was calculated, false if we need make_move fallback
inline uint64_t key_after_move(const Position &position, Move move, bool *success) {
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
  
  // Handle special move types (castling, en passant, promotion) - fall back to make_move
  if (move_type == MoveTypes::Castling || 
      move_type == MoveTypes::EnPassant ||
      move_type == MoveTypes::Promotion) {
    *success = false;  // Need to use make_move for complex moves
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

// OPTIMIZED: Check if move would cause repetition using key_after (avoids position copy)
inline bool would_cause_repetition_after_move(const Position &position, Move move, ThreadInfo &thread_info) {
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
  int game_ply = thread_info.game_ply + 1;  // After move
  
  // CRITICAL FIX: Ensure we have enough game history to check and bounds are valid
  if (game_ply < 2 || halfmoves < 2 || game_ply >= GameSize)
    return false;
  
  int end_index = std::max(game_ply - halfmoves, 0);
  for (int i = game_ply - 2; i >= end_index; i -= 2) {
    // CRITICAL FIX: Ensure index is within bounds before accessing game_hist
    if (i < 0 || i >= GameSize) break;  // Safety check
    if (key_after == thread_info.game_hist[i].position_key) {
      return true;
    }
  }
  
  return false;
}

// ============================================================================
// KING DANGER DETECTION (Crystal port - Fortress Detection helper)
// ============================================================================
// Checks if the king is in a dangerous position (few legal squares, many attackers)
// This helps detect "fortress" positions where draws are likely
// IMPROVEMENT 4: Enhanced detection with attacker position analysis
inline bool king_danger(const Position &position, int color) {
  if (!fortressDetectionEnabled)
    return false;
    
  int ksq = get_king_pos(position, color);
  uint64_t occ = position.colors_bb[Colors::White] | position.colors_bb[Colors::Black];
  uint64_t own_pieces = position.colors_bb[color];
  int enemy = color ^ 1;
  
  // Get king ring (squares around the king)
  uint64_t king_ring = KingAttacks[ksq];
  uint64_t original_king_ring = king_ring;
  
  int king_attackers_count = 0;
  int legal_king_count = 0;
  
  while (king_ring) {
    int to = pop_lsb(king_ring);
    
    // Get all enemy attackers to this square (as bitboard)
    uint64_t enemy_attackers = attacks_square(position, to, enemy);
    
    if (!enemy_attackers) {
      // Square is safe, check if we can move there
      if (!(own_pieces & (1ULL << to))) {
        legal_king_count++;
      }
    } else {
      // Count attackers that are OUTSIDE king ring OR multiple attackers
      uint64_t temp_attackers = enemy_attackers;
      while (temp_attackers) {
        int attacker_sq = pop_lsb(temp_attackers);
        bool outside_ring = !(original_king_ring & (1ULL << attacker_sq));
        // Check if there are multiple attackers (more than just this one)
        bool multiple_attackers = (enemy_attackers & ~(1ULL << attacker_sq)) != 0;
        
        if (outside_ring || multiple_attackers) {
          king_attackers_count++;
        }
      }
    }
  }
  
  // King is in danger if restricted AND under attack
  return (king_attackers_count > 1 && legal_king_count < 2) ||
         (king_attackers_count > 0 && legal_king_count == 0);
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
  if (thread_info.nodes >= thread_info.max_nodes_searched) {

    if (thread_info.max_time < INT32_MAX / 3) {

      std::this_thread::sleep_for(std::chrono::milliseconds(
          thread_info.opt_time - time_elapsed(thread_info.start_time)));
      // If there's a max time limit and a node limit, it means we're using a
      // skill level. In that case, we don't want the engine to move instantly.
    }

    if (thread_info.doing_datagen) {
      thread_info.datagen_stop = true;
    } else {
      thread_data.stop = true;
    }
    return true;
  }
  thread_info.time_checks++;
  if (thread_info.time_checks == 1024) {
    thread_info.time_checks = 0;
    if (time_elapsed(thread_info.start_time) > thread_info.max_time) {
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
inline int16_t material_eval(const Position &position, ThreadInfo &thread_info) {
  // Cache check
  if (thread_info.materialCache.key == position.zobrist_key && thread_info.materialCache.valid) {
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
  if (thread_info.materialCache.key == position.zobrist_key && thread_info.materialCache.valid) {
    return thread_info.materialCache.total_mat;
  }
  
  // Calculate (or get from material_eval cache if available)
  int16_t result = total_mat_uncached(position);
  
  // Update cache
  thread_info.materialCache.key = position.zobrist_key;
  thread_info.materialCache.total_mat = result;
  // material_eval might not be cached yet, but that's OK - it will be calculated when needed
  thread_info.materialCache.valid = true;
  
  return result;
}

// Overload for backward compatibility (without ThreadInfo) - uses position.h version
// Note: This will use the inline version from position.h, not cached

// ============================================================================
// SEE CACHE: Performance optimization for Static Exchange Evaluation
// ============================================================================
// Cached version of SEE - use this in production
inline bool SEE_cached(Position &position, Move move, int threshold, ThreadInfo &thread_info) {
  // Cache check
  if (thread_info.seeCache.key == position.zobrist_key && 
      thread_info.seeCache.move == move && 
      thread_info.seeCache.valid) {
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

// ============================================================================
// HIGH STAKES, HIGH NOISE: Pressure Index (Armageddon Mode Only)
// ============================================================================

// ============================================================================
// ADIM 1: Complexity Faktörü (The Noise) - Returns 1-6 - CACHED VERSION
// ============================================================================
// Calculate position complexity (noise level) - normalized to 1-6 scale
// NOTE: This is called from cached version, which already checks armageddonPsychology
// NOTE: This function is only called when armageddonPsychology is enabled
// Caller must check armageddonPsychology before calling
inline int calculate_complexity_noise_uncached(Position &position) {
  int complexity = 0;
  
  // 1. Vezir Faktörü: Vezirler tahtada ise kaos artar
  int white_queens = position.material_count[8];  // WQueen
  int black_queens = position.material_count[9];  // BQueen
  if (white_queens > 0 && black_queens > 0) {
    complexity += 2;  // Both sides have queens
  }
  
  // 2. Açık Hatlar (Open Files): Taktiksel fırsatlar yaratır
  uint64_t white_pawns_bb = position.pieces_bb[PieceTypes::Pawn] & position.colors_bb[Colors::White];
  uint64_t black_pawns_bb = position.pieces_bb[PieceTypes::Pawn] & position.colors_bb[Colors::Black];
  uint64_t all_pawns = white_pawns_bb | black_pawns_bb;
  
  int open_files = 0;
  for (int file = 0; file < 8; file++) {
    if ((all_pawns & Files[file]) == 0) {
      open_files++;  // Açık hat - her iki tarafta da piyon yok
    }
  }
  if (open_files >= 2) {
    complexity += 2;  // Çoklu açık hatlar
  } else if (open_files == 1) {
    complexity += 1;  // Tek açık hat
  }
  
  // 3. Merkez Kontrolü: Merkezdeki taşlar pozisyonu daha karmaşık yapar
  uint64_t center = (1ULL << e4) | (1ULL << e5) | (1ULL << d4) | (1ULL << d5);
  uint64_t center_pieces = (position.colors_bb[Colors::White] | position.colors_bb[Colors::Black]) & center;
  int center_count = pop_count(center_pieces);
  if (center_count >= 3) {
    complexity += 1;  // Merkezde çok taş var
  }
  
  // 4. Materyal Dengesizliği: Taş sayıları eşit değilse (Fil vs At gibi)
  int white_bishops = position.material_count[4];
  int black_bishops = position.material_count[5];
  int white_knights = position.material_count[2];
  int black_knights = position.material_count[3];
  
  // Sadece farklılık değil, büyük dengesizlik de önemli
  int bishop_imbalance = abs(white_bishops - black_bishops);
  int knight_imbalance = abs(white_knights - black_knights);
  if (bishop_imbalance >= 2 || knight_imbalance >= 2) {
    complexity += 2;  // Büyük dengesizlik
  } else if (bishop_imbalance > 0 || knight_imbalance > 0) {
    complexity += 1;  // Küçük dengesizlik
  }
  
  // 5. Piyon Tansiyonu: Karşılıklı vuruşan piyonlar
  // Calculate pawn attacks
  uint64_t white_pawn_attacks = 0;
  uint64_t black_pawn_attacks = 0;
  
  uint64_t temp_white = white_pawns_bb;
  while (temp_white) {
    int sq = pop_lsb(temp_white);
    white_pawn_attacks |= PawnAttacks[Colors::White][sq];
  }
  
  uint64_t temp_black = black_pawns_bb;
  while (temp_black) {
    int sq = pop_lsb(temp_black);
    black_pawn_attacks |= PawnAttacks[Colors::Black][sq];
  }
  
  // Check if pawns attack each other (tension)
  if ((white_pawn_attacks & black_pawns_bb) || (black_pawn_attacks & white_pawns_bb)) {
    complexity += 1;  // Pawn tension detected
  }
  
  // Base Complexity (Min 1, Max 8) - NNUE ile uyumlu aralık
  return std::max(1, std::min(complexity, 8));
}

// Cached version - use this in production
// NOTE: Caller must check armageddonPsychology before calling this function
// NOTE: This function is only called when armageddonPsychology is enabled
// Caller must check armageddonPsychology before calling
inline int calculate_complexity_noise(Position &position, ThreadInfo &thread_info) {
  // Cache Kontrolü
  if (thread_info.armaCache.key == position.zobrist_key) {
    if (thread_info.armaCache.complexity != -1) {
      return thread_info.armaCache.complexity;
    }
  } else {
    // Yeni pozisyon -> Key güncelle ve diğer değerleri resetle
    thread_info.armaCache.clear();
    thread_info.armaCache.key = position.zobrist_key;
  }
  
  // Ağır Hesaplama (Sadece buraya düşerse yapılır)
  int complexity = calculate_complexity_noise_uncached(position);
  
  // Cache Yaz
  thread_info.armaCache.complexity = complexity;
  return complexity;
}

// Overload for backward compatibility (without ThreadInfo)
inline int calculate_complexity_noise(Position &position) {
  if (!armageddonPsychology) return 1;  // Default complexity when disabled
  return calculate_complexity_noise_uncached(position);
}

// ============================================================================
// PERFORMANCE OPTIMIZATION: Bitboard-based Sharpness Estimation
// ============================================================================
// Replaces expensive count_opponent_safe_moves() with fast bitboard operations
// Calculates "Normalized Pseudo-Mobility" (AMP: Average Mobility Per Piece)
// to estimate how "sharp" (restricted) the opponent's position is.
// ============================================================================
// Returns sharpness level: 0 (LOW) to 4 (EXTREME)
// Uses only bitboard operations - NO move generation, NO SEE calculations
// NOTE: This is called from cached version, which already checks armageddonPsychology
inline int estimate_sharpness_bitboard(const Position &position) {
  // Get opponent's color (them)
  int them = position.color ^ 1;
  int us = position.color;
  int sharpness = 0;
  
  // 1. Mobility Restriction (mevcut - iyi)
  // Payda: Piyon Olmayan Taş Sayısı (Non-Pawn Piece Count)
  uint64_t their_pieces = position.colors_bb[them];
  uint64_t their_pawns = their_pieces & position.pieces_bb[PieceTypes::Pawn];
  uint64_t their_non_pawns = their_pieces & ~their_pawns;
  
  int piece_count = pop_count(their_non_pawns);
  
  // Eğer hiç subay yoksa sadece king safety'ye bak (Endgame veya Pawn War)
  if (piece_count > 0) {
    // Pay: Hareket Alanı (Mobility Area) - Bitboard Operations Only
    uint64_t all_attacks = 0;
    uint64_t occ = position.colors_bb[Colors::White] | position.colors_bb[Colors::Black];
    
    // Knights
    uint64_t knights = their_non_pawns & position.pieces_bb[PieceTypes::Knight];
    while (knights) {
      int sq = pop_lsb(knights);
      all_attacks |= KnightAttacks[sq];
    }
    
    // Bishops
    uint64_t bishops = their_non_pawns & position.pieces_bb[PieceTypes::Bishop];
    while (bishops) {
      int sq = pop_lsb(bishops);
      all_attacks |= get_bishop_attacks(sq, occ);
    }
    
    // Rooks
    uint64_t rooks = their_non_pawns & position.pieces_bb[PieceTypes::Rook];
    while (rooks) {
      int sq = pop_lsb(rooks);
      all_attacks |= get_rook_attacks(sq, occ);
    }
    
    // Queens (both bishop and rook attacks)
    uint64_t queens = their_non_pawns & position.pieces_bb[PieceTypes::Queen];
    while (queens) {
      int sq = pop_lsb(queens);
      all_attacks |= get_bishop_attacks(sq, occ) | get_rook_attacks(sq, occ);
    }
    
    // Kings
    uint64_t kings = their_non_pawns & position.pieces_bb[PieceTypes::King];
    while (kings) {
      int sq = pop_lsb(kings);
      all_attacks |= KingAttacks[sq];
    }
    
    // Remove squares occupied by opponent's own pieces (can't move there)
    uint64_t safe_mobility = all_attacks & ~their_pieces;
    
    // Count mobility squares
    int mobility_squares = pop_count(safe_mobility);
    
    // Hesaplama: Average Mobility Per Piece (AMP)
    // AMP düştükçe sıkışıklık (Sharpness) artar
    int amp = (mobility_squares > 0 && piece_count > 0) ? (mobility_squares / piece_count) : 0;
    
    // Mobility-based sharpness
    if (amp <= 2) sharpness += 3;      // EXTREME: Taşlar kımıldayamıyor
    else if (amp <= 4) sharpness += 2; // HIGH: Çok dar alan
    else if (amp <= 6) sharpness += 1;  // MODERATE: Biraz baskı var
  }
  
  // 2. King Safety: Rakibin kralı ne kadar güvensiz?
  int their_king_sq = get_king_pos(position, them);
  if (their_king_sq != SqNone) {
    uint64_t their_king_area = KingAttacks[their_king_sq] | (1ULL << their_king_sq);
    
    // Bizim taşlarımızın kral alanını tehdit eden saldırıları
    uint64_t our_attacks_to_king = 0;
    uint64_t our_pieces = position.colors_bb[us];
    
    // Knights attacking king area
    uint64_t our_knights = our_pieces & position.pieces_bb[PieceTypes::Knight];
    while (our_knights) {
      int sq = pop_lsb(our_knights);
      if (KnightAttacks[sq] & their_king_area) {
        our_attacks_to_king |= (1ULL << sq);
      }
    }
    
    // Pawns attacking king area
    uint64_t our_pawns = our_pieces & position.pieces_bb[PieceTypes::Pawn];
    uint64_t our_pawn_attacks = 0;
    uint64_t temp_pawns = our_pawns;
    while (temp_pawns) {
      int sq = pop_lsb(temp_pawns);
      our_pawn_attacks |= PawnAttacks[us][sq];
    }
    if (our_pawn_attacks & their_king_area) {
      sharpness += 1;  // Piyonlar kralı tehdit ediyor
    }
    
    // Count pieces attacking king area
    int attackers = pop_count(our_attacks_to_king);
    if (attackers >= 2) {
      sharpness += 1;  // Çoklu taşlar kralı tehdit ediyor
    }
  }
  
  // 3. Weak Pawns: Zayıf piyonlar pozisyonu keskinleştirir (basitleştirilmiş)
  // Backward/isolated pawns kontrolü pahalı olabilir, bu yüzden sadece
  // piyon sayısı dengesizliğine bakıyoruz
  int white_pawn_count = pop_count(position.pieces_bb[PieceTypes::Pawn] & position.colors_bb[Colors::White]);
  int black_pawn_count = pop_count(position.pieces_bb[PieceTypes::Pawn] & position.colors_bb[Colors::Black]);
  int pawn_imbalance = abs(white_pawn_count - black_pawn_count);
  if (pawn_imbalance >= 2) {
    sharpness += 1;  // Büyük piyon dengesizliği
  }
  
  return std::min(sharpness, 5);  // Max 5 (NNUE ile uyumlu)
}

// ============================================================================
// ADIM 2: Sharpness Faktörü (The Blade) - Returns 0-4 - CACHED VERSION
// ============================================================================
// Calculate how "sharp" the position is using fast bitboard estimation
// OPTIMIZED: Uses estimate_sharpness_bitboard() instead of expensive move generation
// NOTE: This function is only called when armageddonPsychology is enabled
// Caller must check armageddonPsychology before calling
inline int calculate_sharpness_level(Position &position, ThreadInfo &thread_info) {
  // Cache Kontrolü
  if (thread_info.armaCache.key == position.zobrist_key) {
    if (thread_info.armaCache.sharpness != -1) {
      return thread_info.armaCache.sharpness;
    }
  } else {
    // Yeni pozisyon -> Key güncelle ve diğer değerleri resetle
    thread_info.armaCache.clear();
    thread_info.armaCache.key = position.zobrist_key;
  }
  
  // Fast bitboard-based calculation (NO move generation, NO SEE)
  // No redundant armageddonPsychology check - caller already verified
  int sharpness = estimate_sharpness_bitboard(position);
  
  // Cache Yaz
  thread_info.armaCache.sharpness = sharpness;
  return sharpness;
}

// Overload for backward compatibility
inline int calculate_sharpness_level(Position &position) {
  if (!armageddonPsychology) return 0;
  return estimate_sharpness_bitboard(position);
}

// ============================================================================
// ADIM 3: The Pressure Score (Büyük Birleşme) - CACHED VERSION
// ============================================================================
// Calculate pressure bonus: Sharpness * Complexity * Multiplier
// NOTE: This function is only called when armageddonPsychology is enabled
// Caller must check armageddonPsychology before calling
inline int calculate_pressure_bonus(Position &position, ThreadInfo &thread_info) {
  // Cache Kontrolü
  if (thread_info.armaCache.key == position.zobrist_key) {
    if (thread_info.armaCache.pressure_bonus != -1) {
      return thread_info.armaCache.pressure_bonus;
    }
  } else {
    // Yeni pozisyon -> Key güncelle ve diğer değerleri resetle
    thread_info.armaCache.clear();
    thread_info.armaCache.key = position.zobrist_key;
  }
  
  // Formül: Baskı = Keskinlik * Karmaşıklık (both cached)
  // No redundant armageddonPsychology checks - caller already verified
  int sharpness = calculate_sharpness_level(position, thread_info);
  int complexity = calculate_complexity_noise(position, thread_info);
  
  // Base pressure calculation
  int base_pressure = sharpness * complexity;
  
  // NNUE-compatible scaling: Daha sofistike ama makul aralıkta
  // Eski: sharpness * complexity * 8 (max: 4*6*8 = 192 cp - çok yüksek!)
  // Yeni: Daha dengeli scaling
  
  int pressure_bonus = base_pressure * 4;  // Base multiplier (daha konservatif)
  
  // Exponential bonus for very high pressure (ama sınırlı)
  if (base_pressure > 15) {
    pressure_bonus += 20;  // High pressure bonus (küçük ekstra)
  }
  if (base_pressure > 20) {
    pressure_bonus += 15;  // Extreme pressure bonus
  }
  
  // Game phase adjustment: Endgame'de daha az önemli
  // NNUE zaten endgame'i iyi değerlendiriyor, bu yüzden psychology bonusunu azalt
  if (thread_info.phase == PhaseTypes::Endgame) {
    pressure_bonus = pressure_bonus * 3 / 4;  // %25 azalt
  }
  
  // Cap the bonus to prevent it from overwhelming NNUE evaluation
  // NNUE genellikle -1000 ile +1000 cp arasında değerler üretir
  // Bu yüzden bonus maksimum 120 cp olmalı (NNUE'nun %12'si)
  pressure_bonus = std::min(pressure_bonus, 120);
  
  // Cache Yaz
  thread_info.armaCache.pressure_bonus = pressure_bonus;
  return pressure_bonus;
}

// Overload for backward compatibility
inline int calculate_pressure_bonus(Position &position) {
  if (!armageddonPsychology) return 0;
  int sharpness = calculate_sharpness_level(position);
  int complexity = calculate_complexity_noise(position);
  
  // Use same improved scaling as cached version (but without phase adjustment)
  int base_pressure = sharpness * complexity;
  int pressure_bonus = base_pressure * 4;
  
  if (base_pressure > 15) pressure_bonus += 20;
  if (base_pressure > 20) pressure_bonus += 15;
  
  return std::min(pressure_bonus, 120);
}

// ============================================================================
// MECHANISM 3: Advanced Complexity Bias (The "Chaos" Index) - DEPRECATED
// ============================================================================
// This is kept for backward compatibility but replaced by calculate_pressure_bonus
// NOTE: This function is only called when armageddonPsychology is enabled
// Caller must check armageddonPsychology before calling
inline int calculate_complexity(const Position &position, ThreadInfo &thread_info) {
  // Use new pressure system instead (cached)
  Position &pos = const_cast<Position&>(position);
  return calculate_pressure_bonus(pos, thread_info);
}

// ============================================================================
// THE RAZOR'S EDGE: Sharpness Heuristic (Armageddon Mode Only)
// ============================================================================


// Calculate sharpness bonus based on opponent's safe move count
// DEPRECATED: Use calculate_pressure_bonus instead (which combines sharpness + complexity)
// Note: This version doesn't have ThreadInfo, so it uses uncached version
inline int calculate_sharpness_bonus(Position &position) {
  if (!armageddonMode) return 0;
  
  // Use new pressure system instead (uncached - for backward compatibility)
  return calculate_pressure_bonus(position);
}

// ============================================================================
// KATMAN 2: Singularity Exploitation - Detect when opponent is in singular position
// ============================================================================
// This is handled in search() by checking if we're in a singular search
// and if the opponent's best move is much better than alternatives
// We'll add a bonus to the parent move when we detect singularity

// Track if current position is singular (for parent bonus)
inline bool is_singular_position(int best_score, int second_best_score, int depth) {
  if (!armageddonMode) return false;
  
  // If best move is significantly better than second best, it's singular
  // Threshold: best should be at least (depth * 2) centipawns better
  int threshold = depth * 2;
  return (best_score - second_best_score) >= threshold;
}

// ============================================================================
// KATMAN 3: Tactical Volatility Index - Measure position chaos - CACHED VERSION
// ============================================================================
// Calculate volatility based on hanging pieces, king exposure, passed pawns
// OPTIMIZED: Only check major/minor pieces (skip pawns) to reduce computation
// NOTE: This is called from cached version, which already checks armageddonPsychology
inline int calculate_tactical_volatility_uncached(Position &position) {
  int volatility = 0;
  
  // 1. Hanging Pieces (Boştaki Taşlar) - OPTIMIZED: Only check major/minor pieces
  // Skip pawns as they're less critical and checking all pieces is expensive
  uint64_t white_majors = (position.pieces_bb[PieceTypes::Rook] | position.pieces_bb[PieceTypes::Queen]) & position.colors_bb[Colors::White];
  uint64_t white_minors = (position.pieces_bb[PieceTypes::Knight] | position.pieces_bb[PieceTypes::Bishop]) & position.colors_bb[Colors::White];
  uint64_t black_majors = (position.pieces_bb[PieceTypes::Rook] | position.pieces_bb[PieceTypes::Queen]) & position.colors_bb[Colors::Black];
  uint64_t black_minors = (position.pieces_bb[PieceTypes::Knight] | position.pieces_bb[PieceTypes::Bishop]) & position.colors_bb[Colors::Black];
  
  int hanging_white = 0;
  int hanging_black = 0;
  
  // Check white major pieces (Rooks, Queens)
  uint64_t pieces_to_check = white_majors;
  while (pieces_to_check) {
    int sq = pop_lsb(pieces_to_check);
    uint64_t attackers = attacks_square(position, sq, Colors::Black);
    uint64_t defenders = attacks_square(position, sq, Colors::White);
    
    if (pop_count(attackers) > pop_count(defenders)) {
      hanging_white++;
    }
  }
  
  // Check white minor pieces (Knights, Bishops)
  pieces_to_check = white_minors;
  while (pieces_to_check) {
    int sq = pop_lsb(pieces_to_check);
    uint64_t attackers = attacks_square(position, sq, Colors::Black);
    uint64_t defenders = attacks_square(position, sq, Colors::White);
    
    if (pop_count(attackers) > pop_count(defenders)) {
      hanging_white++;
    }
  }
  
  // Check black major pieces
  pieces_to_check = black_majors;
  while (pieces_to_check) {
    int sq = pop_lsb(pieces_to_check);
    uint64_t attackers = attacks_square(position, sq, Colors::White);
    uint64_t defenders = attacks_square(position, sq, Colors::Black);
    
    if (pop_count(attackers) > pop_count(defenders)) {
      hanging_black++;
    }
  }
  
  // Check black minor pieces
  pieces_to_check = black_minors;
  while (pieces_to_check) {
    int sq = pop_lsb(pieces_to_check);
    uint64_t attackers = attacks_square(position, sq, Colors::White);
    uint64_t defenders = attacks_square(position, sq, Colors::Black);
    
    if (pop_count(attackers) > pop_count(defenders)) {
      hanging_black++;
    }
  }
  
  volatility += (hanging_white + hanging_black) * 8;  // Reduced from 10 (NNUE compatibility)
  
  // 2. King Exposure (Şah Açıklığı) - King attacks
  int white_king_pos = get_king_pos(position, Colors::White);
  int black_king_pos = get_king_pos(position, Colors::Black);
  
  if (white_king_pos != SqNone && black_king_pos != SqNone) {
    int white_king_attacks = pop_count(attacks_square(position, white_king_pos, Colors::Black));
    int black_king_attacks = pop_count(attacks_square(position, black_king_pos, Colors::White));
    
    volatility += (white_king_attacks + black_king_attacks) * 4;  // Reduced from 5
  }
  
  // 3. Passed Pawns (Geçer Piyonlar) - Simplified check
  // Count pawns that are advanced and not blocked
  int white_pawns = position.material_count[0];
  int black_pawns = position.material_count[1];
  
  // Asymmetry in passed pawns increases volatility
  int pawn_diff = abs(white_pawns - black_pawns);
  if (pawn_diff >= 2) {
    volatility += 12;  // Reduced from 15, only for significant imbalance
  } else if (pawn_diff > 0) {
    volatility += 6;   // Small imbalance
  }
  
  // Cap volatility to prevent it from overwhelming NNUE evaluation
  // Max volatility should be around 80-100 cp (NNUE'nun %8-10'u)
  return std::min(volatility, 100);
}

// Cached version - use this in production
// NOTE: Caller must check armageddonPsychology before calling this function
inline int calculate_tactical_volatility(Position &position, ThreadInfo &thread_info) {
  // Cache Kontrolü
  if (thread_info.armaCache.key == position.zobrist_key) {
    if (thread_info.armaCache.volatility != -1) {
      return thread_info.armaCache.volatility;
    }
  } else {
    // Yeni pozisyon -> Key güncelle ve diğer değerleri resetle
    thread_info.armaCache.clear();
    thread_info.armaCache.key = position.zobrist_key;
  }
  
  // Ağır Hesaplama (Sadece buraya düşerse yapılır)
  int volatility = calculate_tactical_volatility_uncached(position);
  
  // Cache Yaz
  thread_info.armaCache.volatility = volatility;
  return volatility;
}

// Overload for backward compatibility
inline int calculate_tactical_volatility(Position &position) {
  if (!armageddonPsychology) return 0;
  return calculate_tactical_volatility_uncached(position);
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

    if (thread_info.game_hist[idx].m_diff < s_m &&
        thread_info.game_hist[idx + 1].m_diff > s_m &&
        thread_info.game_hist[idx + 2].m_diff < s_m &&
        thread_info.game_hist[idx + 3].m_diff > s_m &&
        thread_info.game_hist[idx + 4].m_diff < s_m) {

      s = s_m + thread_info.game_hist[idx + 4].m_diff;
      break;
    }

    if (thread_info.game_hist[idx].m_diff < 0 &&
        thread_info.game_hist[idx].m_diff == material_eval(position, thread_info)) {

      s = 1;
      break;
    }
  }
  if (s && total_mat(position, thread_info) > 3500) {

    if (thread_info.search_ply % 2) {
      bonus2 = -40 * (eval < -300 ? 2 : eval < 0 ? 1 : 0);
    } else {
      bonus2 = 40 * (eval > 300 ? 2 : eval > 0 ? 1 : 0);
    }
  }

  // If we're winning, scale eval by material; we don't want to trade off to an
  // easily won endgame, but instead should continue the attack.

  float multiplier = ((float)750 + total_mat(position, thread_info) / 25) / 1024;

  eval = eval * multiplier;

  // ============================================================================
  // ARMAGEDDON PSYCHOLOGY: Opponent Mobility Squeeze (Quiet Forcing Moves)
  // ============================================================================
  // NEW APPROACH: No panic thresholds - Psychology is always active when enabled
  // Goal: Reward positions where opponent has restricted mobility (quiet forcing moves)
  // This is applied in move scoring, not in static evaluation
  // (Static eval doesn't know which move led to this position)

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

  if ((ss - 1)->played_move != MoveNone) {
    corr += thread_info.ContCorrHist[(ss - 2)->piece_moved][extract_to(
        (ss - 2)->played_move)][(ss - 1)->piece_moved]
                                    [extract_to((ss - 2)->played_move)];
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
  thread_info.game_hist[thread_info.game_ply].is_cap = is_cap(position, move);
  thread_info.game_hist[thread_info.game_ply].m_diff = material_eval(position, thread_info);

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
  thread_info.armaCache.key = 0;
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

inline bool material_draw(
    const Position &position) { // Is there not enough material on the
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

inline bool is_draw(const Position &position,
             ThreadInfo &thread_info) { // Detects if the position is a draw.

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

// ExpEntryEx::quality - Calculate quality of an experience entry
// Returns pair: (quality_score, maybe_draw)
// Port from HypnoS, adapted for Patricia's structures
inline std::pair<int, bool> exp_quality(const Learning::ExpEntryEx* entry,
                                        Position& pos, ThreadInfo& thread_info,
                                        int evalImportance) {
  static constexpr int QualityEvalImportanceMax = 10;
  static constexpr int QualityExperienceMovesAhead = 10;
  static constexpr int COLOR_NB = 2;

  assert(evalImportance >= 0 && evalImportance <= QualityEvalImportanceMax);

  // Draw detection
  bool maybeDraw = false;

  // Quality based on move count
  int q = entry->count * (QualityEvalImportanceMax - evalImportance);

  // Quality based on difference in evaluation
  if (evalImportance) {
    const uint8_t us = pos.color;
    const uint8_t them = us ^ 1;

    // Calculate quality based on evaluation improvement of next moves
    std::vector<Move> moves;  // Used for doing/undoing of experience moves

    std::array<int64_t, COLOR_NB> sum{};
    std::array<int64_t, COLOR_NB> weight{};

    // Start our sum/weight with something positive!
    sum[us] = entry->count;
    weight[us] = 1;

    // Save original position and thread_info for restoration
    Position originalPos = pos;
    ThreadInfo originalThreadInfo = thread_info;

    // Look ahead
    uint8_t me = us;
    const Learning::ExpEntryEx* lastExp[COLOR_NB] = {nullptr, nullptr};
    const Learning::ExpEntryEx* temp1 = entry;

    while (true) {
      // To be used later
      lastExp[me] = temp1;

      // Do the move
      Move moveToPlay = static_cast<Move>(temp1->move);
      moves.push_back(moveToPlay);
      make_move(pos, moveToPlay);
      thread_info.game_ply++;
      me = me ^ 1;

      if (!maybeDraw) {
        maybeDraw = is_draw(pos, thread_info);
      }

      if (moves.size() >= QualityExperienceMovesAhead) {
        break;
      }

      // Probe the new position
      temp1 = Learning::probe(pos.zobrist_key);

      if (!temp1) {
        break;
      }

      // Find best next experience move (shallow search)
      const Learning::ExpEntryEx* temp2 = temp1->next;

      while (temp2) {
        if (temp2->compare(temp1) > 0) {
          temp1 = temp2;
        }
        temp2 = temp2->next;
      }

      if (lastExp[me]) {
        sum[me] += static_cast<int64_t>(temp1->value - lastExp[me]->value);
        ++weight[me];
      }
    }

    // Restore position and thread_info by undoing all moves
    pos = originalPos;
    thread_info = originalThreadInfo;

    // Calculate quality
    int64_t s = sum[us];
    int64_t w = weight[us];

    if (weight[them]) {
      s -= sum[them];
      w += weight[them];
    }

    if (w > 0) {
      q += static_cast<int>(s * evalImportance / w);
    }
  } else {
    // Shallow draw detection when 'evalImportance' is zero!
    Position tempPos = pos;
    ThreadInfo tempThreadInfo = thread_info;
    Move moveToPlay = static_cast<Move>(entry->move);
    make_move(tempPos, moveToPlay);
    tempThreadInfo.game_ply++;
    maybeDraw = is_draw(tempPos, tempThreadInfo);
  }

  return {q / QualityEvalImportanceMax, maybeDraw};
}

inline int qsearch(int alpha, int beta, Position &position, ThreadInfo &thread_info,
            std::vector<TTBucket> &TT) { // Performs a quiescence search on the
                                         // given position.
  if (out_of_time(thread_info)) {
    // return if out of time
    return correct_eval(position, thread_info, eval(position, thread_info));
  }
  int color = position.color;

  GameHistory *ss = &(thread_info.game_hist[thread_info.game_ply]);

  thread_info.nodes++;

  int ply = thread_info.search_ply;
  
  // Crystal: gameCycle detection in qsearch (ONLY when Fortress Detection is enabled)
  bool gameCycle = false;
  
  // ============================================================================
  // ARMAGEDDON: Standalone upcoming repetition check (independent of Fortress)
  // ============================================================================
  if (armageddonMode && !fortressDetectionEnabled) {
    // When Armageddon is ON but Fortress is OFF, still check upcoming repetition
    if (upcoming_repetition(position, thread_info)) {
      int drawValue = get_draw_value_armageddon(ply, false, thread_info.phase);
      if (drawValue >= beta)
        return drawValue;
      alpha = std::max(alpha, drawValue);
    }
  }
  
  // Fortress Detection: gameCycle check (sets gameCycle flag for pruning decisions)
  if (fortressDetectionEnabled) {
    if (upcoming_repetition(position, thread_info)) {
      int drawValue = get_draw_value_armageddon(ply, false, thread_info.phase);
      if (drawValue >= beta)
        return drawValue;
      
      gameCycle = true;
      alpha = std::max(alpha, drawValue);
    }
  }
  
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

  // Skip TT cutoff if gameCycle (only when Fortress Detection enabled)
  if (tt_score != ScoreNone && !(fortressDetectionEnabled && gameCycle)) {
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
    if (!is_legal(position, move)) {
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
      return ply - ScoreMate;  // Checkmate
    }
    // Stalemate - apply Armageddon penalty if enabled (FIX 2)
    if (armageddonMode) {
      return get_draw_value_armageddon(ply, false, thread_info.phase);
    }
    return 0;  // Normal stalemate = draw
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

  // ============================================================================
  // GAME CYCLE DETECTION (Crystal port - ONLY when Fortress Detection enabled)
  // ============================================================================
  bool gameCycle = false;
  bool kingDanger = false;
  bool kingDangerThem = false;
  
  // Check for king danger (Fortress Detection)
  // IMPROVEMENT 2: Check both our king and opponent's king danger
  if (fortressDetectionEnabled && !root) {
    bool ourMove = !(ply & 1);  // Even ply = our move (root side)
    // Check our king danger when it's opponent's move
    kingDanger = !ourMove && king_danger(position, color);
    // Check opponent's king danger when it's our move
    kingDangerThem = ourMove && king_danger(position, color ^ 1);
  }

  if (ply > thread_info.seldepth) {
    thread_info.seldepth = ply;
  }

  if (out_of_time(thread_info) || ply >= MaxSearchDepth - 1) {
    // check for timeout
    return correct_eval(position, thread_info, eval(position, thread_info));
  }

  // ============================================================================
  // ARMAGEDDON: Standalone upcoming repetition check (independent of Fortress)
  // ============================================================================
  if (armageddonMode && !fortressDetectionEnabled && !root) {
    // When Armageddon is ON but Fortress is OFF, still check upcoming repetition
    if (upcoming_repetition(position, thread_info)) {
      int drawValue = get_draw_value_armageddon(ply, false, thread_info.phase);
      if (drawValue >= beta)
        return drawValue;
      alpha = std::max(alpha, drawValue);
    }
  }

  // ============================================================================
  // UPCOMING REPETITION CHECK (Crystal gameCycle - ONLY when Fortress Detection enabled)
  // ============================================================================
  if (fortressDetectionEnabled && !root && upcoming_repetition(position, thread_info)) {
    int drawValue = get_draw_value_armageddon(ply, false, thread_info.phase);
    if (drawValue >= beta)
      return drawValue;
    
    gameCycle = true;
    alpha = std::max(alpha, drawValue);
  }

  // Standard draw detection (50-move, material, repetition)
  if (ply && is_draw(position, thread_info)) {
    // Armageddon mode: heavy penalty for draws (perspective-aware)
    // At even ply (our turn): draw is bad for us (-ArmageddonPenalty)
    // At odd ply (opponent's turn): draw is good for them (+ArmageddonPenalty)
    // This ensures draws always propagate as bad for root side
    if (armageddonMode) {
      return get_draw_value_armageddon(ply, false, thread_info.phase);
    }
    
    // Normal draw score with small randomization
    int draw_score = 1 - (thread_info.nodes & 3);
    int material = material_eval(position, thread_info);

    if (material < 0) {
      draw_score += 50;
    } else if (material > 0) {
      draw_score -= 50;
    }

    return draw_score;
  }

  if (depth <= 0) {
    return qsearch(alpha, beta, position, thread_info,
                   TT); // drop into qsearch if depth is too low.
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
      return beta;
    }
  }

  bool tt_hit;
  TTEntry &entry = probe_entry(hash, tt_hit, thread_info.searches, TT);

  if (singular_search) {
    tt_hit = false;
  }

  int entry_type = EntryTypes::None, tt_static_eval = ScoreNone,
      tt_score = ScoreNone, tt_move = MoveNone, tt_depth = 0;

  if (tt_hit) { // TT probe
    entry_type = entry.get_type();
    tt_static_eval = entry.static_eval;
    tt_score = score_from_tt(entry.score, ply);
    tt_move = entry.best_move;
    tt_depth = entry.depth;
  }

  // Skip TT cutoff if gameCycle is detected (only when Fortress Detection enabled)
  if (tt_score != ScoreNone && !is_pv && !(fortressDetectionEnabled && gameCycle) && tt_depth >= depth) {
    // If we get a useful score from the TT and it's
    // searched to at least the same depth we would
    // have searched, then we can return
    if ((entry_type == EntryTypes::Exact) ||
        (entry_type == EntryTypes::LBound && tt_score >= beta) ||
        (entry_type == EntryTypes::UBound && tt_score <= alpha)) {
      return tt_score;
    }
  }

  // Probe experience data
  const Learning::ExpEntryEx* expEx =
      (!singular_search && Learning::enabled())
          ? Learning::probe(position.zobrist_key)
          : nullptr;
  const Learning::ExpEntryEx* tempExp = expEx;
  const Learning::ExpEntryEx* bestExp = nullptr;

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
        entry_type = (tt_score >= beta ? EntryTypes::LBound : EntryTypes::Exact);

        // Write to TT
        insert_entry(entry, hash, static_cast<int>(tt_depth), tt_move,
                     ScoreNone, score_to_tt(tt_score, ply), entry_type,
                     thread_info.searches);
      }

      // Update history for experience moves (bonus for good moves, malus for bad)
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

  // Experience lookup with priority if deeper than TT
  if (!singular_search && Learning::enabled()) {
    const auto* bestExpEntry = Learning::find_best_entry(position.zobrist_key);
    if (bestExpEntry && (!tt_hit || bestExpEntry->depth > tt_depth)) {
      const uint8_t expDepth = static_cast<uint8_t>(bestExpEntry->depth);
      const Move expMove = static_cast<Move>(bestExpEntry->move);
      const int32_t expValue = bestExpEntry->value;

      // Early cutoff with experience depth
      if (!is_pv && expDepth > static_cast<uint8_t>(depth - (expValue <= beta ? 1 : 0)) &&
          expValue != ScoreNone &&
          (tt_score == ScoreNone ||
           (expValue >= beta ? (entry_type & EntryTypes::LBound)
                             : (entry_type & EntryTypes::UBound))) &&
          (cutnode == (expValue >= beta) || depth > 5) &&
          (depth > 8 || expMove == MoveNone || position.halfmoves < 80 ||
           (!is_cap(position, expMove) &&
            get_piece_type(position.board[extract_from(expMove)]) !=
                PieceTypes::Pawn))) {
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
      // If Armageddon mode and TB says DRAW but DTZ > 80, encourage prolonging game
      if (armageddonMode && tb_wdl == TB_DRAW) {
        uint32_t dtz = TB_GET_DTZ(tb_result);
        if (dtz > 80) {
          // Game is far from ending - return small advantage to encourage continuation
          tb_score = 10;  // Small advantage instead of 0
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
  // ARMAGEDDON: Rule 50 scaling - penalize positions approaching 50-move draw (FIX 4)
  // OPTIMIZED: Early exit if halfmoves < 60 (function returns eval unchanged)
  // ============================================================================
  if (armageddonMode && !in_check && static_eval != ScoreNone && position.halfmoves >= 60) {
    static_eval = apply_armageddon_rule50_scaling(static_eval, position.halfmoves, ply, thread_info.phase);
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

  if (!is_pv && !in_check && !singular_search) {

    // Skip pruning if gameCycle is detected (only when Fortress Detection enabled)
    bool skipForGameCycle = fortressDetectionEnabled && gameCycle;
    
    if (!skipForGameCycle && alpha < 2000 && depth < 5 && static_eval + 400 * depth < alpha) {
      score = qsearch(alpha, beta, position, thread_info, TT);
      if (score <= alpha) {
        return score;
      }
    }

    // Reverse Futility Pruning (RFP): If our position is way better than beta,
    // we're likely good to stop searching the node.
    // Skip if gameCycle or kingDanger (only when Fortress Detection enabled)

    if (!skipForGameCycle && !kingDanger && depth <= RFPMaxDepth &&
        static_eval - RFPMargin * (depth - improving) >= beta) {
      return (static_eval + beta) / 2;
    }

      // Null Move Pruning (NMP): If we can give our opponent a free move and
      // still beat beta on a reduced search, we can prune the node.
    // Skip if gameCycle (only when Fortress Detection enabled)
    
    if (!skipForGameCycle && static_eval >= beta && depth >= NMPMinDepth &&
        has_non_pawn_material(position, color) &&
        (ss - 1)->played_move != MoveNone &&
        !kingDanger) {

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
        return score;
      }
    }
  }

    // Internal Iterative Reduction: If we are in a PV node and have no TT move,
    // reduce the depth.
  // Skip if gameCycle (only when Fortress Detection enabled)
  bool skipForGameCycleIIR = fortressDetectionEnabled && gameCycle;
  if ((is_pv || cutnode) && tt_move == MoveNone && depth > IIRMinDepth && !skipForGameCycleIIR) {
    depth--;
  }

  // ProbCut: Skip if gameCycle (only when Fortress Detection enabled)
  int p_beta = beta + ProbcutMargin;
  if (!in_check && !skipForGameCycleIIR && depth >= 5 && abs(beta) < ScoreWin &&
      (!tt_hit || tt_depth + 4 <= depth || tt_score >= p_beta)) {

    int threshold = p_beta - static_eval;
    MovePicker probcut_p;
    init_picker(probcut_p, position, threshold, in_check, ss);
    // Original Patricia uses SEE check; Crystal removes it (only when Fortress Detection enabled)
    Move p_tt_move;
    if (fortressDetectionEnabled) {
      // Crystal commit af181: Simplified - just check if tt_move is capture
      p_tt_move = (tt_move != MoveNone && is_cap(position, tt_move)) ? tt_move : MoveNone;
    } else {
      // Original Patricia: use SEE check (CACHED)
      p_tt_move = (tt_move != MoveNone && SEE_cached(position, tt_move, threshold, thread_info)) ? tt_move : MoveNone;
    }

    while (Move move =
               next_move(probcut_p, position, thread_info, p_tt_move, true)) {

      if (probcut_p.stage > Stages::Captures) {
        break;
      }
      if (move == excluded_move || !is_legal(position, move)) {
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

    if (root) {
      bool pv_skip = false;
      for (int i = 0; i < thread_info.multipv_index; i++) {
        if (thread_info.best_moves[i] == move) {
          pv_skip = true;
          break;
        }
      }
      if (pv_skip) {
        continue;
      }
    }

    if (move == excluded_move) {
      continue;
    }
    if (!is_legal(position, move)) {
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
        hist_score -= 3000;  // Heavy penalty in move ordering for repetition moves
      }
    }

    is_capture = is_cap(position, move);
    
    // ============================================================================
    // ARMAGEDDON PSYCHOLOGY: Quiet Forcing Moves (Opponent Mobility Squeeze) - MOVE ORDERING
    // ============================================================================
    // NEW APPROACH: Reward quiet moves that restrict opponent's mobility in move ordering
    // Only applies to QUIET moves (not captures) - these are "forcing" in a positional sense
    // Goal: Force opponent into "only-move" situations through space restriction, not tactics
    // NOTE: This is Psychology feature, not base Armageddon (draw penalties)
    // OPTIMIZATION: Only calculate for quiet moves when Psychology is enabled
    if (armageddonPsychology && !is_capture) {
      // Create temporary position to check opponent's mobility after this move
      Position temp_position = position;
      make_move(temp_position, move);
      
      // Check opponent's mobility squeeze level after our quiet move (fast bitboard calculation)
      // estimate_sharpness_bitboard returns 0-5: higher = more restricted opponent
      int opp_squeeze = estimate_sharpness_bitboard(temp_position);
      
      // Bonus: The more we restrict opponent's mobility, the better
      // This encourages quiet moves that force opponent into limited options
      // Applied to hist_score for better move ordering
      if (opp_squeeze >= 4) {
        hist_score += 50;  // Strong bonus - opponent extremely restricted (EXTREME squeeze)
      } else if (opp_squeeze >= 3) {
        hist_score += 35;  // Good bonus - opponent highly restricted (HIGH squeeze)
      } else if (opp_squeeze >= 2) {
        hist_score += 20;  // Moderate bonus - opponent moderately restricted
      } else if (opp_squeeze >= 1) {
        hist_score += 10;  // Small bonus - opponent slightly restricted
      }
    }
    
    // Crystal commit 901ad: quietMoveStreak - reset only on capture (ONLY when Fortress Detection enabled)
    if (fortressDetectionEnabled) {
      ss->quietMoveStreak = is_capture ? 0 : (ss - 1)->quietMoveStreak + 1;
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

      if (!is_pv && !is_capture && lmr_depth < HistPruningDepth &&
          hist_score < -4096 * lmr_depth) {
        skip = true;
      }
    }

    if (!root && best_score > ScoreLost && depth < SeePruningDepth) {

      int margin = is_capture ? SeePruningQuietMargin : SeePruningNoisyMargin;

      if (!SEE_cached(position, move, depth * margin, thread_info)) {
        // SEE pruning: if we are hanging material, prune under certain
        // conditions.
        continue;
      }
    }

    int extension = 0;
    
    // Crystal: gameCycleExtension - extend when in game cycle to find escape (ONLY when Fortress Detection enabled)
    if (fortressDetectionEnabled && gameCycle) {
      int drawValue = get_draw_value_armageddon(ply, false, thread_info.phase);
      bool gameCycleExtension = is_pv || (ply > 2 && drawValue < 0);
      if (gameCycleExtension) {
        extension = 2;
      }
    }
    
    // ============================================================================
    // IMPROVEMENT 7: Armageddon Extension for Draw-Avoiding Moves (OPTIMIZED)
    // ============================================================================
    // When in Armageddon mode and position is drawish, extend to find winning resources
    // OPTIMIZATION: Use key_after_move instead of Position copy to avoid expensive copy
    if (armageddonMode && !fortressDetectionEnabled && extension == 0) {
      // Check if position is drawish (high halfmoves or upcoming repetition)
      bool drawish = position.halfmoves > 50;
      if (!drawish && !root) {
        // Check if this move avoids a repetition using key_after_move (avoids position copy)
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
          // Fallback: complex move (castling, en passant, promotion) - use would_cause_repetition_after_move
          // OPTIMIZATION: This function already uses key_after_move internally and falls back to make_move if needed
          if (!would_cause_repetition_after_move(position, move, thread_info)) {
            // This move doesn't cause repetition - might be an escape
            if (position.halfmoves > 30 || material_draw(position)) {
              drawish = true;
            }
          }
        }
      }
      if (drawish && depth < 6 && !is_capture) {
        extension = 1;  // Extend to find winning resources
      }
    }

    // Singular Extensions (SE): If a search finds that the TT move is way
    // better than all other moves, extend it under certain conditions.
    // Skip singular extension if gameCycle (only when Fortress Detection enabled)
    // BRANCH PREDICTION: Combined check - both conditions are typically false
    if (!root && ply < thread_info.current_iter * 2 && !(fortressDetectionEnabled && gameCycle)) {
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

    // Original Patricia LMR condition: depth >= LMRMinDepth && moves_played > is_pv
    // Crystal adds: skip when gameCycle, extra (!is_pv || ply > 1) condition
    // IMPROVEMENT 2: Also consider opponent's king danger in LMR allowance
    bool allowLMR;
    if (fortressDetectionEnabled) {
      bool skipLMRForGameCycle = gameCycle;
      // Skip LMR if opponent's king is in danger (unless at high ply)
      allowLMR = depth >= LMRMinDepth && !skipLMRForGameCycle && 
                 (!kingDangerThem || ply > 6) && (!is_pv || ply > 1);
    } else {
      // Original Patricia: just depth check
      allowLMR = depth >= LMRMinDepth;
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

      // Increase reduction if not in pv
      R -= is_pv;

      R -= (tt_hit && tt_depth >= depth);

      // Increase reduction if not improving
      R += !improving;

      R += cutnode;

      R -= (attacks_square(moved_position, get_king_pos(position, color ^ 1),
                           color) != 0);

      R += (thread_info.FailHighCount[ply + 1] > 4);

      R = std::clamp(R, 0, newdepth - 1);
      
      // LMR depth formula: Crystal uses updated formula only when Fortress Detection enabled
      int lmr_depth;
      if (fortressDetectionEnabled) {
        // Crystal commit 678d5: Updated LMR depth formula
        lmr_depth = std::max(1, std::min(newdepth - R, newdepth + 2)) + is_pv;
      } else {
        // Original Patricia LMR depth
        lmr_depth = newdepth - R;
      }

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
      find_root_move(thread_info, move)->nodes +=
          (thread_info.nodes - curr_nodes);
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
  }

  if (best_score == ScoreNone) { // handle no legal moves (stalemate/checkmate)
    if (singular_search) return alpha;
    if (in_check) return ply - ScoreMate;  // Checkmate
    // Stalemate - apply Armageddon penalty if enabled (FIX 2)
    if (armageddonMode) {
      return get_draw_value_armageddon(ply, false, thread_info.phase);
    }
    return 0;  // Normal stalemate = draw
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

    if ((ss-1)->played_move && (ss-2)->played_move){
      update_corrhist(thread_info.ContCorrHist[(ss - 2)->piece_moved][extract_to(
        (ss - 2)->played_move)][(ss - 1)->piece_moved]
                                    [extract_to((ss - 2)->played_move)],
        bonus);
    }
  }

  // Add the search results to the TT, accounting for mate scores
  if (!singular_search) {
    insert_entry(entry, hash, depth, best_move, raw_eval,
                 score_to_tt(best_score, ply), entry_type,
                 thread_info.searches);
  }

  return best_score;
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

void iterative_deepen(
    Position &position, ThreadInfo &thread_info,
    std::vector<TTBucket> &TT) { // Performs an iterative deepening search.

  thread_info.original_opt = thread_info.opt_time;
  thread_info.datagen_stop = false;
  thread_info.nnue_state.reset_nnue(position, total_mat(position, thread_info) < PhaseBound);
  calculate(position);
  thread_info.nodes = 0;
  thread_info.tb_hits = 0;
  thread_info.time_checks = 0;
  thread_info.phase = total_mat(position, thread_info) < PhaseBound ? PhaseTypes::Endgame : PhaseTypes::Sacrifice;
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
      thread_info.root_moves.push_back({raw_root_moves[i], 0});
    }
  }

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
    
    const auto* exp = Learning::probe(position.zobrist_key);
    
    if (exp) {
      // Collect all entries that meet min depth requirement with their quality
      // Using positional quality calculation with lookahead (like HypnoS)
      std::vector<std::pair<const Learning::ExpEntryEx*, int>> qualityList;
      const auto* temp = exp;
      
      while (temp) {
        if (temp->depth >= static_cast<int32_t>(expBookMinDepth)) {
          // Use positional quality with lookahead
          auto [q, maybeDraw] = temp->quality_positional(position, evalImportance, 
            [](Position& pos, uint16_t mv) { make_move(pos, static_cast<Move>(mv)); });
          if (q > 0 && !maybeDraw) {
            qualityList.emplace_back(temp, q);
          }
        }
        temp = temp->next;
      }
      
      if (!qualityList.empty()) {
        // Sort by quality (highest first)
        std::sort(qualityList.begin(), qualityList.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        
        // Select move based on width
        if (expBookWidth > 1 && qualityList.size() > 1) {
          // Random selection from top 'width' moves
          static std::mt19937 rng(static_cast<unsigned>(
              std::chrono::steady_clock::now().time_since_epoch().count()));
          int maxIndex = std::min<int>(expBookWidth, static_cast<int>(qualityList.size()));
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
  
  // If experience enabled but not book mode, still use experience for move ordering
  if (Learning::enabled() && !thread_info.doing_datagen && !useExpBookMove) {
    const auto* bestExp = Learning::find_best_entry(position.zobrist_key);
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

  Move prev_best = MoveNone;
  int alpha = ScoreNone, beta = -ScoreNone;
  int bm_stability = 0;

  for (int depth = 1; depth <= thread_info.max_iter_depth; depth++) {

    int real_multi_pv =
        std::min<int>(thread_info.multipv, thread_info.root_moves.size());
    for (thread_info.multipv_index = 0;
         thread_info.multipv_index < real_multi_pv;
         thread_info.multipv_index++) {

      int temp_depth = depth;

      int score, delta = AspStartWindow;

      score =
          search<true>(alpha, beta, depth, false, position, thread_info, TT);

      // Aspiration Windows: We search the position with a narrow window around
      // the last search score in order to get cutoffs faster. If our search
      // lands outside the bounds, expand them and try again.
      // CRITICAL FIX: Add iteration limit to prevent infinite loops (especially in Armageddon mode)

      int asp_iterations = 0;
      const int max_asp_iterations = 50;  // Prevent infinite loops

      while ((score <= alpha || score >= beta || thread_data.stop ||
             thread_info.datagen_stop) && asp_iterations < max_asp_iterations) {

        asp_iterations++;

        if (thread_data.stop || thread_info.datagen_stop) {
          goto finish;
        }
        
        // CRITICAL FIX: If we've expanded the window too much, reset to full window
        // This prevents infinite loops when Armageddon mode returns extreme scores
        if (asp_iterations > 20) {
          alpha = ScoreNone;
          beta = -ScoreNone;
          temp_depth = depth;
          delta = AspStartWindow;
        }

        if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
            !(thread_info.is_human && thread_info.multipv_index)) {
          std::string bound_string;
          if (score >= beta) {
            bound_string = "lowerbound";
          } else {
            bound_string = "upperbound";
          }

          uint64_t nodes = thread_info.nodes;
          for (auto &td : thread_data.thread_infos) {
            nodes += td.nodes;
          }
          int64_t search_time = time_elapsed(thread_info.start_time);
          int64_t nps = search_time
                            ? static_cast<int64_t>(nodes) * 1000 / search_time
                            : 123456789;

          Move move = score <= alpha
                          ? prev_best
                          : thread_info.best_moves[thread_info.multipv_index];

          printf("info multipv %i depth %i seldepth %i score cp %i %s nodes "
                 "%" PRIu64 " nps %" PRIi64 " time %" PRIi64 " pv %s\n",
                 thread_info.multipv_index + 1, depth, thread_info.seldepth,
                 score * 100 / NormalizationFactor, bound_string.c_str(), nodes,
                 nps, search_time, internal_to_uci(position, move).c_str());
        }

        if (score <= alpha) {
          beta = (alpha + beta) / 2;
          alpha -= delta;
          temp_depth = depth;
        } else if (score >= beta) {
          beta += delta;
          temp_depth = std::max(temp_depth - 1, 1);
        }
        delta += delta / 3;

        score = search<true>(alpha, beta, temp_depth, false, position,
                             thread_info, TT);
      }

      if (score == ScoreNone) {
        break;
      }

      std::string eval_string;

      if (abs(score) <= ScoreTbWin) {
        eval_string = "cp " + std::to_string(score * 100 / NormalizationFactor);
      } else if (score > 0) {
        eval_string = "mate " + std::to_string((ScoreMate - score + 1) / 2);
      } else {
        eval_string = "mate " + std::to_string((-ScoreMate - score) / 2);
      }

      thread_info.best_moves[thread_info.multipv_index] = thread_info.pv[0];

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

        if (!thread_info.doing_datagen /*&&
            !(thread_info.is_human && thread_info.multipv_index)*/) {
          printf("info multipv %i depth %i seldepth %i score %s nodes %" PRIu64
                 " nps %" PRIi64 " tbhits %" PRIu64 " time %" PRIi64 " pv ",
                 thread_info.multipv_index + 1, depth, thread_info.seldepth,
                 eval_string.c_str(), nodes, nps, tb_hits, search_time);
          print_pv(position, thread_info);
        }

        else {
          thread_info.best_scores[0] = score * 100 / NormalizationFactor;
        }

        if (search_time > thread_info.opt_time ||
            nodes > thread_info.opt_nodes_searched) {

          if (thread_info.doing_datagen) {
            thread_info.datagen_stop = true;
          } else {
            thread_data.stop = true;
          }
        }

        else if (thread_info.multipv == 1 && depth > 6) {
          if (thread_info.best_moves[0] == prev_best) {
            bm_stability = std::min(bm_stability + 1, 8);
          } else {
            bm_stability = 0;
          }

          adjust_soft_limit(
              thread_info,
              find_root_move(thread_info, thread_info.best_moves[0])->nodes,
              bm_stability);
        }

        if (depth == 6){
          if (thread_info.best_scores[0] < 0){
            thread_info.phase = PhaseTypes::Endgame;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          }
          else if (thread_info.best_scores[0] > 400){
            thread_info.phase = PhaseTypes::Sacrifice;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          }
          else{
            thread_info.phase = PhaseTypes::Middlegame;
            thread_info.nnue_state.reset_nnue(position, thread_info.phase);
          }
        }
      }

      if (thread_data.stop || thread_info.datagen_stop) {
        goto finish;
      }

      prev_best = thread_info.best_moves[0];

      if (depth > 6 && thread_info.multipv_index == 0) {
        alpha = score - 20, beta = score + 20;
      } else {
        alpha = ScoreNone, beta = -ScoreNone;
      }
    }
  }

finish:
  // Save experience data (PV and MultiPV)
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
      !Learning::is_learning_paused() && Learning::enabled()) {
    // Save PV experience
    if (!thread_info.best_moves.empty() &&
        thread_info.best_moves[0] != MoveNone &&
        thread_info.current_iter >= static_cast<int>(Learning::MinDepth)) {
      uint8_t effDepth = static_cast<uint8_t>(thread_info.current_iter);
      Learning::add_pv_experience(position.zobrist_key,
                                  thread_info.best_moves[0],
                                  thread_info.best_scores[0],
                                  effDepth);
    }

    // Save MultiPV experience (collect from all threads)
    if (thread_info.multipv > 1) {
      // Collect alternative moves from all threads
      struct UniqueMoveInfo {
        Move move;
        uint8_t depth;
        int32_t scoreSum;
        int count;
      };

      std::vector<UniqueMoveInfo> uniqueMoves;
      Move bestMv = thread_info.best_moves[0];

      // Collect from all threads
      for (auto& td : thread_data.thread_infos) {
        if (td.best_moves.empty() || td.best_moves[0] == MoveNone ||
            td.best_moves[0] == bestMv)
          continue;

        const Move m0 = td.best_moves[0];
        UniqueMoveInfo thisMove{m0, static_cast<uint8_t>(td.current_iter),
                                td.best_scores[0], 1};

        bool merged = false;
        for (auto& um : uniqueMoves) {
          if (um.move == thisMove.move) {
            if (thisMove.depth > um.depth)
              um = thisMove;
            else if (thisMove.depth == um.depth) {
              um.scoreSum += thisMove.scoreSum;
              um.count++;
            }
            merged = true;
            break;
          }
        }
        if (!merged)
          uniqueMoves.push_back(thisMove);
      }

      // Commit MultiPV entries
      for (const auto& info : uniqueMoves) {
        if (info.depth >= Learning::MinDepth) {
          Learning::add_multipv_experience(
              position.zobrist_key, info.move,
              info.scoreSum / info.count, info.depth);
        }
      }
    }

    // Save experience to file
    Learning::save();
  }

  // wait for all threads to finish searching
  // printf("%i\n", thread_info.thread_id);
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen) {
    thread_data.stop = true;
  }
  search_end_barrier.arrive_and_wait();
  if (thread_info.thread_id == 0 && !thread_info.doing_datagen &&
      !thread_info.is_human) {
    printf("bestmove %s\n",
           internal_to_uci(position, thread_info.best_moves[0]).c_str());
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
    thread_data.thread_infos[i] = thread_info;
    thread_data.thread_infos[i].thread_id = i + 1;
  }

  // Tell threads to start
  idle_barrier.arrive_and_wait();

  thread_data.stop = false;
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
