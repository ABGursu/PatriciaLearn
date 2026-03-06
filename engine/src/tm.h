#pragma once
#include "defs.h"
#include "utils.h"
#include <cmath>

// HypnoS-identical multi-factor soft limit computation.
// Returns the uncapped totalTime. Caller is responsible for capping at max_time
// and for the increaseDepth decision using the uncapped value.
// outTimeReduction is written with this depth's sigmoid value (persisted at
// search end for cross-search stability memory).
double compute_soft_limit(ThreadInfo &thread_info, int depth, int bestValue,
                          int iterIdx, int lastBestMoveDepth,
                          double totBestMoveChanges, int num_threads,
                          double &outTimeReduction) {

  int prevAvgScore = thread_info.bestPreviousAverageScore != ScoreNone
                         ? thread_info.bestPreviousAverageScore
                         : bestValue;

  double fallingEval =
      (11.85 + 2.24 * (prevAvgScore - bestValue) +
       0.93 * (thread_info.iterValue[iterIdx] - bestValue)) /
      100.0;
  fallingEval = std::clamp(fallingEval, 0.57, 1.70);

  double k = 0.51;
  double center = lastBestMoveDepth + 12.15;
  outTimeReduction =
      0.66 + 0.85 / (0.98 + std::exp(-k * (depth - center)));

  double reduction =
      (1.43 + thread_info.previousTimeReduction) / (2.28 * outTimeReduction);

  double bestMoveInstability =
      1.02 + 2.14 * totBestMoveChanges / std::max(1, num_threads);

  uint64_t totalNodes = std::max<uint64_t>(1, thread_info.nodes);
  uint64_t nodesEffort =
      thread_info.root_moves[0].effort * 100000 / totalNodes;
  double highBestMoveEffort = nodesEffort >= 93340 ? 0.76 : 1.0;

  double totalTime = thread_info.original_opt * fallingEval * reduction *
                     bestMoveInstability * highBestMoveEffort;

  if (thread_info.root_moves.size() == 1)
    totalTime = std::min(502.0, totalTime);

  return totalTime;
}
