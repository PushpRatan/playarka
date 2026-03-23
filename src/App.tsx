import React, { useState, useEffect } from "react";
import { BrowserRouter, Routes, Route } from "react-router-dom";
import { useMqttState } from "./hooks/useMqttState";
import { useAudioAnnouncer } from "./hooks/useAudioAnnouncer";
import { QRCode } from "./components/QRCode";
import { WaitingScreen } from "./components/WaitingScreen";
import { PaymentScreen } from "./components/PaymentScreen";
import { ScoreTable } from "./components/ScoreTable";
import { OnboardingPage } from "./pages/OnboardingPage";
import { getNetworkIP, getQRCodeURL } from "./utils/network";
import { PINS_PER_FRAME } from "./constants/game";
import bowlingImage from "./assets/bowling-icon-isolated-red-ball.png";

function WebApp() {
  const { deviceId, state, publish } = useMqttState();
  useAudioAnnouncer(state.announce);
  const [, setNetworkIP] = useState<string | null>(null);
  const [qrCodeUrl, setQrCodeUrl] = useState<string>("");

  useEffect(() => {
    getNetworkIP()
      .then((ip) => {
        setNetworkIP(ip);
        const port = window.location.port || "5173";
        setQrCodeUrl(getQRCodeURL(ip, port, deviceId));
      })
      .catch((err) => {
        console.error("Error getting network IP:", err);
        const port = window.location.port || "5173";
        setQrCodeUrl(getQRCodeURL(null, port, deviceId));
      });
  }, [deviceId]);

  return (
    <div className="min-h-[100dvh] min-h-screen wood-background box-border">
      <main className="w-full max-w-7xl mx-auto px-3 sm:px-4 md:px-6 py-4 sm:py-6 relative box-border">
        {/* Pin error banner */}
        {state.pinError && (
          <div className="fixed top-0 left-0 right-0 z-60 bg-red-600/95 backdrop-blur-sm text-white px-6 py-4 flex items-center justify-between shadow-lg">
            <div className="flex items-center gap-3">
              <svg className="w-6 h-6 shrink-0" fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M12 9v2m0 4h.01m-6.938 4h13.856c1.54 0 2.502-1.667 1.732-2.5L13.732 4c-.77-.833-1.964-.833-2.732 0L4.072 16.5c-.77.833.192 2.5 1.732 2.5z" />
              </svg>
              <span className="font-semibold">{state.pinError.message}</span>
            </div>
            <button
              onClick={() => publish("reset")}
              className="px-4 py-1.5 bg-white/20 rounded hover:bg-white/30 transition-colors text-sm font-medium"
            >
              Reset Game
            </button>
          </div>
        )}

        {/* Lane label */}
        <div className="fixed top-6 left-6 z-50 px-3 py-1.5 rounded-md bg-black/50 border border-gray-600/40 text-gray-400 text-sm font-medium">
          Lane {deviceId}
        </div>
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
          <div className="flex min-h-[calc(100dvh-7rem)] w-full max-w-full items-center justify-center py-4 sm:py-6">
            <div className="flex w-full max-w-full min-w-0 flex-col items-center justify-center gap-6 sm:gap-8 md:flex-row md:items-center md:justify-center md:gap-10 lg:gap-12">
              {/* QR column — min-w-0 lets flex shrink; no huge side margins (was mr-20/ml-20) */}
              <div className="flex w-full min-w-0 max-w-[min(100%,22rem)] shrink-0 flex-col items-center md:max-w-none md:w-auto">
                <div className="wood-panel relative w-full max-w-[min(100%,20rem)] overflow-hidden rounded-lg border-2 border-gray-600/40 p-4 sm:p-5">
                  <div className="flex justify-center [&_canvas]:max-w-full [&_canvas]:h-auto">
                    <QRCode data={qrCodeUrl} size={220} />
                  </div>

                  <div className="mt-4 border border-gray-600/30 bg-black/40 p-3 rounded-lg sm:mt-5 sm:p-4">
                    <p className="mb-1 text-center text-xs font-semibold uppercase tracking-wider text-gray-400 sm:mb-2">
                      Scan to Connect
                    </p>
                    <p className="break-all rounded border border-gray-600/30 bg-black/50 px-2 py-2 text-center font-mono text-xs text-gray-300 sm:text-sm">
                      {qrCodeUrl}
                    </p>
                  </div>
                </div>
              </div>

              {/* Branding column */}
              <div className="flex min-w-0 max-w-full flex-col items-center px-2 text-center md:max-w-[min(100%,24rem)]">
                <img
                  src={bowlingImage}
                  alt="Bowling"
                  className="h-auto max-h-[min(28vh,12rem)] w-auto max-w-[min(70vw,14rem)] object-contain opacity-90 sm:max-h-[min(32vh,14rem)] sm:max-w-[16rem] md:max-h-40 md:max-w-44"
                />
                <div className="mt-3 space-y-1 sm:mt-4 sm:space-y-2">
                  <h2 className="font-heading-primary text-4xl text-orange-500 sm:text-5xl md:text-6xl">
                    Playarka
                  </h2>
                  <h3 className="font-heading-secondary text-lg text-gray-300 tracking-tight sm:text-xl md:text-2xl">
                    {PINS_PER_FRAME}-Pin Duckpin
                  </h3>
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
    <ErrorBoundary>
      <BrowserRouter>
        <Routes>
          <Route path="/" element={<WebApp />} />
          <Route path="/onboarding" element={<OnboardingPage />} />
        </Routes>
      </BrowserRouter>
    </ErrorBoundary>
  );
}

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
