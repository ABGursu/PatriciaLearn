#pragma once
// Armageddon Draw Detection helpers:
// Crystal-style king_danger + Sting BlackHole position signals (no OldSting).
#include "bitboard.h"
#include "position.h"
#include "utils.h"
#include <algorithm>

namespace DrawDetect {

constexpr int PassivePlayMaterialBorder =
    2 * MaterialValues[PieceTypes::Queen] + 2 * MaterialValues[PieceTypes::Rook] +
    2 * MaterialValues[PieceTypes::Bishop];

constexpr int BhDrawBand = 16;

inline int npm_color(const Position &pos, int color) {
  return pos.material_count[2 + color] * MaterialValues[PieceTypes::Knight] +
         pos.material_count[4 + color] * MaterialValues[PieceTypes::Bishop] +
         pos.material_count[6 + color] * MaterialValues[PieceTypes::Rook] +
         pos.material_count[8 + color] * MaterialValues[PieceTypes::Queen];
}

// Crystal king_danger: king ring under real pressure with few escapes.
inline bool king_danger(const Position &pos, int color) {
  const int ksq = get_king_pos(pos, color);
  const uint64_t occ =
      pos.colors_bb[Colors::White] | pos.colors_bb[Colors::Black];
  const uint64_t ourPieces = pos.colors_bb[color];
  const uint64_t enemyPieces = pos.colors_bb[color ^ 1];

  uint64_t kingRing = KingAttacks[ksq];
  uint64_t kingAttackers = 0;
  uint64_t legalKing = 0;
  uint64_t ring = kingRing;

  while (ring) {
    int to = get_lsb(ring);
    ring &= ring - 1;
    uint64_t enemyAttackers = attacks_square(pos, to, occ) & enemyPieces;
    if (!enemyAttackers) {
      if (!(ourPieces & (1ull << to)))
        legalKing |= (1ull << to);
    }
    uint64_t ea = enemyAttackers;
    while (ea) {
      int currentEnemy = get_lsb(ea);
      ea &= ea - 1;
      uint64_t atk = attacks_square(pos, to, occ) & enemyPieces;
      if (!(kingRing & (1ull << currentEnemy)) || pop_count(atk) > 1)
        kingAttackers |= (1ull << currentEnemy);
    }
  }

  const int kingAttackersCount = pop_count(kingAttackers);
  const int legalKingCount = pop_count(legalKing);
  return (kingAttackersCount > 1 && legalKingCount < 2) ||
         (kingAttackersCount > 0 && !legalKingCount);
}

inline int possible_wall(const Position &pos) {
  if (pop_count(pos.colors_bb[Colors::White] & pos.pieces_bb[PieceTypes::Pawn]) < 4 ||
      pop_count(pos.colors_bb[Colors::Black] & pos.pieces_bb[PieceTypes::Pawn]) < 4)
    return 0;

  int wall = 0;
  uint64_t b = ((pos.colors_bb[Colors::White] & pos.pieces_bb[PieceTypes::Pawn]) << 8) &
               (pos.colors_bb[Colors::Black] & pos.pieces_bb[PieceTypes::Pawn]);
  while (b) {
    wall++;
    b &= b - 1;
  }
  return wall;
}

const int WeakMaterialAdvantage = 2 * MaterialValues[PieceTypes::Knight] - MaterialValues[PieceTypes::Rook];

inline bool passive_piece(const ThreadInfo &ti, uint64_t pieces, int ply) {
  if (!pieces)
    return false;

  const int gp = ti.game_ply;
  for (int i = 1; i <= ply; i++) {
    if (gp - i < 0) break;
    const Move move = ti.game_hist[gp - i].played_move;
    if (move != MoveNone && (pieces & (1ULL << extract_to(move)))) {
      pieces &= ~(1ULL << extract_to(move));
      pieces |= (1ULL << extract_from(move));
    }
  }

  for (int i = 1; i <= ply; i++) {
    if (gp - i < 0) break;
    const Move move = ti.game_hist[gp - i].played_move;
    if (move != MoveNone && (pieces & (1ULL << extract_to(move)))) {
      pieces &= ~(1ULL << extract_to(move));
    }
    if (!pieces)
      return false;
  }
  return true;
}

inline bool passive_play(const Position &pos, const ThreadInfo &ti, int ply, int strongerSide) {
  int d = npm_color(pos, strongerSide) - npm_color(pos, strongerSide ^ 1);
  const int rook = MaterialValues[PieceTypes::Rook];
  const int bishop = MaterialValues[PieceTypes::Bishop];
  const int knight = MaterialValues[PieceTypes::Knight];

  if (d >= rook) {
    if (npm_color(pos, Colors::White) > 2 * bishop && npm_color(pos, Colors::Black) > 2 * bishop)
      return false;
    if (d >= rook - WeakMaterialAdvantage + knight && npm_color(pos, Colors::White) > bishop && npm_color(pos, Colors::Black) > bishop)
      return false;
  }

  if (!pos.pieces_bb[PieceTypes::Pawn] && d >= rook - WeakMaterialAdvantage) return false;
  if (ply < 9) return false;
  int wall = possible_wall(pos);
  if (wall < 2) return false;

  // Center pawns check: A fortress cannot exist if the stronger side's
  // central pawns (files C, D, E, F) can freely push forward!
  const uint64_t centerFiles = Files[2] | Files[3] | Files[4] | Files[5];
  const uint64_t strongCenterPawns = pos.colors_bb[strongerSide] & pos.pieces_bb[PieceTypes::Pawn] & centerFiles;
  const uint64_t occ = pos.colors_bb[Colors::White] | pos.colors_bb[Colors::Black];
  if (strongCenterPawns) {
    uint64_t pushes = (strongerSide == Colors::White) ? (strongCenterPawns << 8) : (strongCenterPawns >> 8);
    if (pushes & ~occ)
      return false; // Center pawns can advance, not a closed fortress!
  }

  // Genuine 50-move proximity
  if (pos.halfmoves >= 30) return true;

  // Stronger side minor pieces (Bishops/Knights) must be passive/trapped
  uint64_t strongMinors = pos.colors_bb[strongerSide] & (pos.pieces_bb[PieceTypes::Knight] | pos.pieces_bb[PieceTypes::Bishop]);
  if (strongMinors && !passive_piece(ti, strongMinors, ply))
    return false;

  ply = std::min(ply - 6, std::max(16, ply / 2));

  int pawnCount = pop_count(pos.pieces_bb[PieceTypes::Pawn]);
  const int gp = ti.game_ply;
  int stm = pos.color;
  uint64_t b = pos.colors_bb[strongerSide] & pos.pieces_bb[PieceTypes::Pawn];
  int captures = 0;
  int pawnMoves = 0;
  int stop = std::min(std::max(1, pawnCount / 2), wall >= 4 ? 2 : 3);

  for (int i = 1; i <= ply; i++) {
    if (gp - i - 1 < 0) break;

    Move prevMove = ti.game_hist[gp - i].played_move;
    Move prevPrevMove = ti.game_hist[gp - i - 1].played_move;
    uint8_t capturedPiece = ti.game_hist[gp - i].captured_type; 
    uint8_t prevCapturedPiece = ti.game_hist[gp - i - 1].captured_type;
    
    bool is_promotion = false;
    if (prevPrevMove != MoveNone) is_promotion = extract_promo(prevPrevMove) != 0;

    if (is_promotion) {
      if (prevMove != MoveNone && extract_to(prevMove) != extract_to(prevPrevMove) && d >= rook - WeakMaterialAdvantage)
        return false;
      if (wall >= 5 || d < knight)
        pawnMoves++;
      if (stm == strongerSide)
        b |= (1ULL << extract_from(prevPrevMove));
    }

    // Sting check: only check leaf node's capture of a piece > PAWN!
    const bool leafCapture = (ti.game_ply >= 1 && ti.game_hist[ti.game_ply - 1].captured_type > PieceTypes::Pawn);
    if (leafCapture && stm == (strongerSide ^ 1) && i == 1 && (prevPrevMove == MoveNone || extract_to(prevPrevMove) != extract_to(prevMove)))
      return false;

    if (prevCapturedPiece == PieceTypes::Pawn && stm == strongerSide && (i < 16 || d < rook - WeakMaterialAdvantage) && (prevMove == MoveNone || extract_to(prevPrevMove) != extract_to(prevMove)))
      captures++;

    if (prevMove != MoveNone) {
      if (capturedPiece == PieceTypes::Pawn && stm == strongerSide) {
         b |= (1ULL << extract_to(prevMove));
         if ((wall >= 5) && i <= 16) captures++;
      }
      if (b & (1ULL << extract_to(prevMove))) {
        if (capturedPiece == PieceTypes::PieceNone) {
          pawnMoves++;
          if (wall >= 5 || d < knight) b |= (1ULL << extract_from(prevMove));
        }
        b &= ~(1ULL << extract_to(prevMove));
      }
    }

    if (pawnMoves + captures >= stop && (captures || !pop_count(pos.colors_bb[strongerSide ^ 1] & pos.pieces_bb[PieceTypes::Pawn]) || (pop_count(pos.colors_bb[strongerSide ^ 1] & pos.pieces_bb[PieceTypes::Pawn]) <= 1 && d >= knight) || wall >= 6))
      return false;

    stm ^= 1;
  }
  return true;
}

inline bool passive_play_ext(const Position &pos, const ThreadInfo &ti, int ply, int strongerSide, bool overZero) {
  if (ply < 9) return false;
  if (npm_color(pos, strongerSide) < npm_color(pos, strongerSide ^ 1)) return false;
  int wall = possible_wall(pos);
  if (wall < 2) return false;
  if (overZero && pos.halfmoves >= 30) return true;
  ply = std::min(ply - 6, std::max(16, ply / 2));

  if ((npm_color(pos, strongerSide) + WeakMaterialAdvantage) / 2 < npm_color(pos, strongerSide ^ 1)) {
    if (passive_piece(ti, pos.colors_bb[strongerSide] & (pos.pieces_bb[PieceTypes::Knight] | pos.pieces_bb[PieceTypes::Bishop]), ply))
      return true;
    return false;
  }

  if (wall >= 6) return true;

  if (!pop_count(pos.colors_bb[strongerSide] & pos.pieces_bb[PieceTypes::Pawn]) && pop_count(pos.colors_bb[strongerSide ^ 1] & pos.pieces_bb[PieceTypes::Pawn]) <= 2)
    return false;

  return true;
}

// Oscillating quiet rook / major-piece shuttle suggestive of perpetual checks.
inline bool possible_perpetual_checks(const Position &pos, ThreadInfo &ti) {
  if (ti.game_ply < 12)
    return false;

  const int gp = ti.game_ply;
  const Move moc = ti.game_hist[gp - 1].played_move;
  const Move mc = ti.game_hist[gp - 2].played_move;
  if (moc == MoveNone || mc == MoveNone)
    return false;
  if (ti.game_hist[gp - 1].is_cap || ti.game_hist[gp - 2].is_cap)
    return false;

  const int piece = ti.game_hist[gp - 2].piece_moved;
  if (get_piece_type(piece) != PieceTypes::Rook)
    return false;
  if (extract_to(mc) == extract_to(moc))
    return false;

  // Require current king under attack (check) to treat as perpetual-ish.
  const int us = pos.color;
  if (!attacks_square(pos, get_king_pos(pos, us), us ^ 1))
    return false;

  int matched = 0;
  for (int i = 0; i < 6 && gp - 1 - i >= 0; i++) {
    const auto &h = ti.game_hist[gp - 1 - i];
    if (h.played_move == MoveNone || h.is_cap)
      break;
    if (i % 2 == 0) {
      if (extract_from(h.played_move) == extract_to(moc) ||
          extract_to(h.played_move) == extract_from(moc))
        matched++;
    } else {
      if (extract_from(h.played_move) == extract_to(mc) ||
          extract_to(h.played_move) == extract_from(mc))
        matched++;
    }
  }
  return matched >= 5;
}

// True when ADD should treat this shallow node as a drawish branch.
// Scores via current_draw_score at the call site (no match-ending side effects).
inline bool drawish_branch(const Position &pos, ThreadInfo &ti, int ply,
                           bool blackHole, int strongerSide,
                           bool testPassiveNonZero) {
  if (ply >= 12 && possible_perpetual_checks(pos, ti) &&
      (!blackHole || testPassiveNonZero))
    return true;

  // In Sting, passive_play is ONLY evaluated when TestPassivePlay is active.
  // BlackHole (0.00 repulsion) alone uses beta-tweaking to avoid draws; it DOES NOT
  // forcefully return VALUE_ZERO via drawish branches unless TestPassivePlay is active!
  if (testPassiveNonZero && !attacks_square(pos, get_king_pos(pos, pos.color), pos.color ^ 1)) {
    const GameHistory *ss = &(ti.game_hist[ti.game_ply]);
    int eval = (ss - 1)->static_eval != ScoreNone ? -(ss - 1)->static_eval : (ss - 2)->static_eval;
    bool evalOk = eval != ScoreNone
                  ? ((eval >= 0 && pos.color == strongerSide) || (eval <= 0 && pos.color == (strongerSide ^ 1)))
                  : true;
    if (evalOk && (!blackHole || passive_play_ext(pos, ti, ply, strongerSide, true))) {
      if (passive_play(pos, ti, ply, strongerSide))
        return true;
    }
  }

  return false;
}

} // namespace DrawDetect
