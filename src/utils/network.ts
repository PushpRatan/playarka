// Utility to get network IP for mobile access

const BACKEND_URL = 'http://localhost:3001';

export async function getNetworkIP(): Promise<string | null> {
  try {
    // Create abort controller for timeout
    const controller = new AbortController();
    const timeoutId = setTimeout(() => controller.abort(), 3000);

    const response = await fetch(`${BACKEND_URL}/api/info`, {
      signal: controller.signal,
    });
    
    clearTimeout(timeoutId);
    
    if (!response.ok) {
      throw new Error('Backend not responding');
    }
    const data = await response.json();
    // Extract IP from network URL (e.g., "http://192.168.1.100:3001" -> "192.168.1.100")
    const networkUrl = data.network || '';
    const match = networkUrl.match(/http:\/\/([^:]+)/);
    return match ? match[1] : null;
  } catch (error) {
    console.warn('Failed to get network IP from backend:', error);
    console.warn('Make sure backend is running on port 3001');
    return null;
  }
}

export function getQRCodeURL(networkIP: string | null, port: string = '5173'): string {
  const protocol = window.location.protocol;
  const DEVICE_ID = '1';
  
  // If we have network IP and we're on localhost, use network IP
  if (networkIP && (window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1')) {
    return `${protocol}//${networkIP}:${port}/onboarding?device=${DEVICE_ID}`;
  }
  
  // Otherwise use current hostname
  const hostname = window.location.hostname;
  const currentPort = window.location.port || port;
  return `${protocol}//${hostname}${currentPort ? `:${currentPort}` : ''}/onboarding?device=${DEVICE_ID}`;
}

