interface FrameScore {
  ball1: number | string | null; // number of pins, 'X' for strike, '/' for spare
  ball2: number | string | null;
  ball3: number | string | null; // 3 shots for duckpin bowling
  cumulative: number | null;
}

interface PlayerScore {
  name: string;
  frames: FrameScore[];
  totalScore: number;
  maxScore: number;
}

interface ScoreTableProps {
  players: string[];
  currentPlayer?: number;
  currentFrame?: number;
  currentRoll?: number;
  // When provided, live scores from backend/ESP are used instead of mock data
  scores?: PlayerScore[];
}

// Mock data generator for demonstration
function generateMockScores(players: string[]): PlayerScore[] {
  return players.map((name, index) => {
    const frames: FrameScore[] = [];
    let cumulative = 0;

    // Generate scores for 10 frames (duckpin bowling has 3 shots per frame)
    for (let i = 0; i < 10; i++) {
      let ball1: number | string | null = null;
      let ball2: number | string | null = null;
      let ball3: number | string | null = null;

      if (i < 3) {
        // First few frames - show some scores
        if (i === 0) {
          ball1 = 5;
          ball2 = 3;
          ball3 = "/"; // spare
          cumulative += 14;
        } else if (i === 1) {
          ball1 = 4;
          ball2 = 5;
          ball3 = 1;
          cumulative += 10;
        } else {
          ball1 = 8;
          ball2 = 1;
          ball3 = "/"; // spare
          cumulative += 20;
        }
      } else {
        // Empty frames for now
        cumulative += 0;
      }

      frames.push({
        ball1,
        ball2,
        ball3,
        cumulative: i < 3 ? cumulative : null,
      });
    }

    return {
      name,
      frames,
      totalScore: cumulative,
      maxScore: 300,
    };
  });
}

export function ScoreTable({
  players,
  currentPlayer,
  currentFrame,
  currentRoll,
  scores,
}: ScoreTableProps) {
  // Use real scores when provided from backend; otherwise fall back to mock data
  const hasRealScores = Array.isArray(scores) && scores.length > 0;
  const playerScores: PlayerScore[] = hasRealScores
    ? (scores as PlayerScore[])
    : generateMockScores(players);

  return (
    <div className="w-full max-w-7xl mx-auto p-4">
      {/* Header */}
      <div className="wood-panel border-2 border-gray-600/40 rounded-lg mb-3 relative overflow-hidden">
        <div className="bg-gray-800/90 px-4 py-2 text-center">
          <h2 className="text-xl font-bold text-white uppercase tracking-wider">
            Bowling Scoresheet
          </h2>
        </div>
      </div>

      {/* Scorecards */}
      <div className="space-y-3">
        {playerScores.map((playerScore, playerIndex) => (
          <div
            key={playerIndex}
            className={`wood-panel border-2 ${
              currentPlayer === playerIndex
                ? "border-orange-500/50"
                : "border-gray-600/40"
            } rounded-lg relative overflow-hidden`}
          >
            <table className="w-full border-collapse text-xs">
              <thead>
                <tr>
                  <th className="bg-gray-800/90 text-white p-2 text-left font-semibold border-r-2 border-gray-600/50 w-[120px]">
                    <div className="flex items-center gap-2">
                      <span className="text-xl font-bold text-gray-300">
                        {playerIndex + 1}
                      </span>
                      <span className="text-sm truncate">
                        {playerScore.name}
                      </span>
                    </div>
                  </th>
                  {[1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((frameNum) => (
                    <th
                      key={frameNum}
                      className={`bg-gray-800/90 text-white p-1.5 text-center font-semibold border-r border-gray-600/50 ${
                        currentPlayer === playerIndex &&
                        currentFrame === frameNum
                          ? "bg-orange-600/40"
                          : ""
                      }`}
                    >
                      {frameNum}
                    </th>
                  ))}
                  <th className="bg-gray-800/90 text-white p-1.5 text-center font-semibold w-[70px]">
                    Total
                  </th>
                </tr>
              </thead>
              <tbody>
                {/* Ball Scores Row */}
                <tr>
                  <td className="bg-gray-700/60 p-2 border-r-2 border-gray-600/50"></td>
                  {playerScore.frames.map((frame, frameIndex) => (
                    <td
                      key={frameIndex}
                      className={`bg-gray-700/60 p-1.5 border-r border-gray-600/50 text-center ${
                        currentPlayer === playerIndex &&
                        currentFrame === frameIndex + 1
                          ? "bg-orange-600/20"
                          : ""
                      }`}
                    >
                      <div className="flex items-center justify-center gap-0.5">
                        <span className="w-5 h-5 flex items-center justify-center border border-gray-600/50 rounded bg-black/40 text-white text-xs font-mono font-semibold">
                          {frame.ball1 !== null ? frame.ball1 : ""}
                        </span>
                        <span className="w-5 h-5 flex items-center justify-center border border-gray-600/50 rounded bg-black/40 text-white text-xs font-mono font-semibold">
                          {frame.ball2 !== null ? frame.ball2 : ""}
                        </span>
                        <span className="w-5 h-5 flex items-center justify-center border border-gray-600/50 rounded bg-black/40 text-white text-xs font-mono font-semibold">
                          {frame.ball3 !== null ? frame.ball3 : ""}
                        </span>
                      </div>
                    </td>
                  ))}
                  <td className="bg-gray-700/60 p-1.5 text-center font-bold text-white text-sm">
                    {playerScore.totalScore}
                  </td>
                </tr>
                {/* Cumulative Scores Row */}
                <tr>
                  <td className="bg-gray-800/70 p-2 border-r-2 border-gray-600/50"></td>
                  {playerScore.frames.map((frame, frameIndex) => (
                    <td
                      key={frameIndex}
                      className={`bg-gray-800/70 p-1.5 border-r border-gray-600/50 text-center font-bold text-white text-sm ${
                        currentPlayer === playerIndex &&
                        currentFrame === frameIndex + 1
                          ? "bg-orange-600/30"
                          : ""
                      }`}
                    >
                      {frame.cumulative !== null ? frame.cumulative : ""}
                    </td>
                  ))}
                  <td className="bg-gray-800/70 p-1.5"></td>
                </tr>
              </tbody>
            </table>
          </div>
        ))}
      </div>
    </div>
  );
}
