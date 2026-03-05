import React, { useState, useEffect } from "react";
import { BrowserRouter, Routes, Route } from "react-router-dom";
import { useMqttState } from "./hooks/useMqttState";
import { QRCode } from "./components/QRCode";
import { WaitingScreen } from "./components/WaitingScreen";
import { PaymentScreen } from "./components/PaymentScreen";
import { ScoreTable } from "./components/ScoreTable";
import { OnboardingPage } from "./pages/OnboardingPage";
import { getNetworkIP, getQRCodeURL } from "./utils/network";
import bowlingImage from "./assets/bowling-icon-isolated-red-ball.png";

function WebApp() {
  const { state, isConnected, error, publish } = useMqttState();
  const [networkIP, setNetworkIP] = useState<string | null>(null);
  const [qrCodeUrl, setQrCodeUrl] = useState<string>("");

  useEffect(() => {
    // Get network IP from backend
    getNetworkIP()
      .then((ip) => {
        setNetworkIP(ip);
        const port = window.location.port || "5173";
        setQrCodeUrl(getQRCodeURL(ip, port));
      })
      .catch((err) => {
        console.error("Error getting network IP:", err);
        // Fallback to localhost if backend is not available
        const port = window.location.port || "5173";
        setQrCodeUrl(getQRCodeURL(null, port));
      });
  }, []);

  return (
    <div className="min-h-screen wood-background">
      <main className="max-w-7xl mx-auto p-6 relative">
        {/* Reset Button - Top Right */}
        {state.status !== "waiting" && (
          <button
            onClick={() => publish("reset")}
            className="fixed top-6 right-6 w-12 h-12 rounded-full wood-panel border-2 border-gray-600/40 text-gray-300 hover:border-gray-500/60 hover:text-white transition-colors flex items-center justify-center z-50"
            title="Reset & Show QR Code"
          >
            <svg
              className="w-5 h-5"
              fill="none"
              stroke="currentColor"
              viewBox="0 0 24 24"
            >
              <path
                strokeLinecap="round"
                strokeLinejoin="round"
                strokeWidth={2}
                d="M4 4v5h.582m15.356 2A8.001 8.001 0 004.582 9m0 0H9m11 11v-5h-.581m0 0a8.003 8.003 0 01-15.357-2m15.357 2H15"
              />
            </svg>
          </button>
        )}

        {state.status === "waiting" && (
          <div className="min-h-[calc(100vh-120px)] flex items-center justify-center py-12 px-6">
            <div className="w-full max-w-6xl">
              <div className="flex flex-col lg:flex-row items-center justify-center">
                {/* Left side - QR Code */}
                <div className="flex justify-center lg:justify-start mr-20">
                  <div className="wood-panel rounded-lg p-8 border-2 border-gray-600/40 relative overflow-hidden">
                    <QRCode data={qrCodeUrl} size={280} />

                    {/* URL Display */}
                    <div className="mt-6 p-4 bg-black/40 rounded-lg border border-gray-600/30">
                      <p className="text-xs text-gray-400 font-semibold mb-2 text-center uppercase tracking-wider">
                        Scan to Connect
                      </p>
                      <p className="text-sm text-gray-300 text-center break-all font-mono bg-black/50 px-3 py-2 rounded border border-gray-600/30">
                        {qrCodeUrl}
                      </p>
                    </div>
                  </div>
                </div>

                {/* Right side - Bowling Illustration & Branding */}
                <div className="flex flex-col items-center  ml-20">
                  {/* Bowling Illustration */}
                  <div className="flex">
                    <img
                      src={bowlingImage}
                      alt="Bowling"
                      className="w-64 h-64 lg:w-100 lg:h-100 object-cover opacity-90"
                    />
                  </div>

                  {/* Branding Text */}
                  <div className="text-center space-y-4">
                    <h2 className="font-heading-primary text-6xl lg:text-7xl text-orange-500 mb-2">
                      Playarka
                    </h2>
                    <h3 className="font-heading-secondary text-xl lg:text-2xl text-gray-300 tracking-tight">
                      Duckpin Bowling
                    </h3>
                  </div>
                </div>
              </div>
            </div>
          </div>
        )}

        {state.status === "onboarding" && (
          <div>
            <WaitingScreen
              players={state.players.map((p) =>
                typeof p === "string" ? p : p.name,
              )}
              status={state.status}
            />
          </div>
        )}

        {state.status === "payment" && (
          <div>
            <PaymentScreen
              paymentQr={state.paymentQr || ""}
              amount={state.amount}
              paymentStatus={state.paymentStatus}
              onSimulatePayment={() =>
                publish("paymentStatus", { status: "success" })
              }
            />
          </div>
        )}

        {state.status === "active" && (
          <div className="min-h-[calc(100vh-120px)] py-6">
            <ScoreTable
              players={state.players.map((p) =>
                typeof p === "string" ? p : p.name,
              )}
              currentPlayer={state.currentPlayer}
              currentFrame={state.frame}
              currentRoll={state.roll}
              scores={state.scores as any}
            />
          </div>
        )}

        {state.status === "completed" && (
          <div className="flex flex-col items-center justify-center min-h-[calc(100vh-120px)]">
            <h2 className="text-3xl font-bold text-white mb-8 text-center">
              Game Completed!
            </h2>
          </div>
        )}
      </main>
    </div>
  );
}

function App() {
  return (
    <BrowserRouter>
      <Routes>
        <Route path="/" element={<WebApp />} />
        <Route path="/onboarding" element={<OnboardingPage />} />
      </Routes>
    </BrowserRouter>
  );
}

// Error Boundary Component
class ErrorBoundary extends React.Component<
  { children: React.ReactNode },
  { hasError: boolean; error: Error | null }
> {
  constructor(props: { children: React.ReactNode }) {
    super(props);
    this.state = { hasError: false, error: null };
  }

  static getDerivedStateFromError(error: Error) {
    return { hasError: true, error };
  }

  componentDidCatch(error: Error, errorInfo: React.ErrorInfo) {
    console.error("App Error:", error, errorInfo);
  }

  render() {
    if (this.state.hasError) {
      return (
        <div className="min-h-screen wood-background flex items-center justify-center p-6">
          <div className="wood-panel border-2 border-gray-600/40 rounded-lg p-8 text-center relative overflow-hidden max-w-md">
            <h1 className="text-2xl font-bold text-red-400 mb-4">
              Something went wrong
            </h1>
            <p className="text-gray-400 mb-4">{this.state.error?.message}</p>
            <button
              onClick={() => window.location.reload()}
              className="wood-panel border-2 border-gray-600/40 text-gray-300 px-6 py-3 rounded-lg font-semibold hover:border-gray-500/60 hover:text-white transition-colors relative overflow-hidden"
            >
              Reload Page
            </button>
          </div>
        </div>
      );
    }

    return this.props.children;
  }
}

export default App;
