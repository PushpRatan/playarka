interface WaitingScreenProps {
  players: string[];
  status: string;
}

export function WaitingScreen({ players, status }: WaitingScreenProps) {
  return (
    <div className="flex flex-col items-center justify-center min-h-[calc(100vh-120px)] p-6">
      <div className="w-full max-w-md">
        <h2 className="text-3xl font-bold text-white mb-8 text-center">
          Waiting for Players
        </h2>
        
        <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-6 mb-6 relative overflow-hidden">
          <h3 className="text-lg font-semibold text-gray-300 mb-4">
            Players Joined ({players.length})
          </h3>
          
          {players.length === 0 ? (
            <p className="text-gray-400 text-center py-4">
              No players yet. Scan the QR code to join!
            </p>
          ) : (
            <ul className="space-y-2">
              {players.map((player, index) => (
                <li
                  key={index}
                  className="flex items-center justify-between bg-black/30 border border-gray-600/40 rounded px-4 py-2"
                >
                  <span className="text-gray-300 font-medium">
                    {index + 1}. {player}
                  </span>
                </li>
              ))}
            </ul>
          )}
        </div>

        <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-4 text-center relative overflow-hidden">
          <p className="text-sm text-gray-400">
            Status: <span className="font-semibold text-white">{status}</span>
          </p>
        </div>
      </div>
    </div>
  );
}

