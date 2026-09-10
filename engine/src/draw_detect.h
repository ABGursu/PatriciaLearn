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
  const uint64_t wp =
      pos.colors_bb[Colors::White] & pos.pieces_bb[PieceTypes::Pawn];
  const uint64_t bp =
      pos.colors_bb[Colors::Black] & pos.pieces_bb[PieceTypes::Pawn];
  if (pop_count(wp) < 4 || pop_count(bp) < 4)
    return 0;
  const uint64_t locked = (wp << 8) & bp;
  int c = 0;
  for (int f = 0; f < 8; f++)
    if (Files[f] & locked)
      c++;
  return c;
}

inline bool possible_fortress(const Position &pos, int c) {
  if (!pos.pieces_bb[PieceTypes::Pawn])
    return false;

  const uint64_t ourPawns = pos.colors_bb[c] & pos.pieces_bb[PieceTypes::Pawn];
  if (!ourPawns && npm_color(pos, c ^ 1) > npm_color(pos, c))
    return true;

  if (npm_color(pos, c) > MaterialValues[PieceTypes::Bishop] ||
      npm_color(pos, c ^ 1) - npm_color(pos, c) <
          MaterialValues[PieceTypes::Rook] +
              MaterialValues[PieceTypes::Knight])
    return false;

  const uint64_t oppPawns =
      pos.colors_bb[c ^ 1] & pos.pieces_bb[PieceTypes::Pawn];
  const uint64_t rank27 = Ranks[1] | Ranks[6];
  uint64_t b = oppPawns & rank27;
  if (b & (((b & ~Files[7]) << 1) | ((b & ~Files[0]) >> 1)))
    return false;

  return true;
}

// Simplified Sting passive_play using Patricia history (no captured-piece chain).
inline bool passive_play(const Position &pos, const ThreadInfo &ti, int ply,
                         int strongerSide) {
  const int d = npm_color(pos, strongerSide) - npm_color(pos, strongerSide ^ 1);
  const int rook = MaterialValues[PieceTypes::Rook];
  const int bishop = MaterialValues[PieceTypes::Bishop];
  const int knight = MaterialValues[PieceTypes::Knight];
  const int pawn = MaterialValues[PieceTypes::Pawn];

  if (d >= rook) {
    if (npm_color(pos, Colors::White) > 2 * bishop &&
        npm_color(pos, Colors::Black) > 2 * bishop)
      return false;
    if (d >= rook - pawn + knight && npm_color(pos, Colors::White) > bishop &&
        npm_color(pos, Colors::Black) > bishop)
      return false;
  }

  if (!pos.pieces_bb[PieceTypes::Pawn] && d >= rook - pawn)
    return false;

  if (ply < 9)
    return true;

  const int wall = possible_wall(pos);
  if (wall == 8)
    return true;

  const int thresh = std::min(ply - 6, std::max(16, ply / 2));
  if (pos.halfmoves >= thresh)
    return true;

  if (wall >= 6 && d < rook)
    return true;

  // Quiet stretch in game history: few captures recently.
  int quiet = 0;
  const int start = std::max(0, ti.game_ply - thresh);
  for (int i = ti.game_ply - 1; i >= start; --i) {
    if (!ti.game_hist[i].is_cap)
      quiet++;
    else
      break;
  }
  if (quiet >= thresh && wall >= 4)
    return true;

  return false;
}

inline bool passive_play_ext(const Position &pos, int ply, int strongerSide,
                             bool overZero) {
  if (ply < 9)
    return true;
  if (npm_color(pos, strongerSide) < npm_color(pos, strongerSide ^ 1))
    return true;

  const int thresh = std::min(ply - 6, std::max(16, ply / 2));
  if (overZero && pos.halfmoves >= thresh)
    return true;

  if (possible_wall(pos) >= 6)
    return true;

  if (!pop_count(pos.colors_bb[strongerSide] & pos.pieces_bb[PieceTypes::Pawn]) &&
      pop_count(pos.colors_bb[strongerSide ^ 1] &
                pos.pieces_bb[PieceTypes::Pawn]) <= 2)
    return false;

  return true;
}

// Oscillating quiet rook / major-piece shuttle suggestive of perpetual checks.
inline bool possible_perpetual_checks(const Position &pos, ThreadInfo &ti) {
  if (ti.game_ply < 6)
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
  if (ply < 12)
    return false;

  if (possible_perpetual_checks(pos, ti) &&
      (!blackHole || testPassiveNonZero))
    return true;

  // Fortress and locked wall detection (applicable whenever ADD is active,
  // in endgames / fortress zones, both around 0.00 and in winning positions)
  if (possible_fortress(pos, strongerSide) ||
      possible_fortress(pos, strongerSide ^ 1) ||
      possible_wall(pos) >= 6)
    return true;

  // BlackHole active (0.00 zone): detect general passive play
  if (blackHole) {
    return passive_play(pos, ti, ply, strongerSide);
  }

  // TestPassivePlay path (stuck "winning" claim): detect extended passive play
  if (testPassiveNonZero) {
    if (passive_play_ext(pos, ply, strongerSide, true))
      return passive_play(pos, ti, ply, strongerSide);
  }

  return false;
}

} // namespace DrawDetect
