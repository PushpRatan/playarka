// Utility to get network IP for mobile access

function getBackendUrl(): string {
  const host = window.location.hostname;
  return `http://${host}:3001`;
}

const BACKEND_URL = getBackendUrl();

export async function getNetworkIP(): Promise<string | null> {
  try {
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);

    const response = await fetch(`${BACKEND_URL}/api/info`, {
      signal: controller.signal,
    });

    clearTimeout(timeoutId);

    if (!response.ok) {
      throw new Error("Backend not responding");
    }
    const data = await response.json();
    const networkUrl = data.network || "";
    const match = networkUrl.match(/http:\/\/([^:]+)/);
    return match ? match[1] : null;
  } catch (error) {
    console.warn("Failed to get network IP from backend:", error);
    console.warn("Make sure backend is running on port 3001");
    return null;
  }
}

/** Build onboarding URL for mobile scan; use same device id as this screen so QR is per-lane. */
export function getQRCodeURL(
  networkIP: string | null,
  port: string = "5173",
  deviceId: string = "1",
): string {
  const protocol = window.location.protocol;
  const currentPort = window.location.port || port;

  if (
    networkIP &&
    (window.location.hostname === "localhost" ||
      window.location.hostname === "127.0.0.1")
  ) {
    return `${protocol}//${networkIP}:${currentPort}/onboarding?device=${deviceId}`;
  }

  const hostname = window.location.hostname;
  return `${protocol}//${hostname}${currentPort ? `:${currentPort}` : ""}/onboarding?device=${deviceId}`;
}
