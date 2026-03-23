import "./env.js";
import fs from "fs";
import path from "path";
import { fileURLToPath } from "url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function elevenLabsKey() {
  return (process.env.ELEVENLABS_API_KEY || "").trim();
}

// Mood → Voice mapping for dynamic, context-aware announcing
const VOICE_MAP = {
  strike:     "TX3LPaxmHKxFdv7VOQHJ", // Liam - Energetic, celebrating
  spare:      "TX3LPaxmHKxFdv7VOQHJ", // Liam - Energetic
  high:       "TX3LPaxmHKxFdv7VOQHJ", // Liam - Energetic
  medium:     "iP95p4xoKVk53GoZ742B", // Chris - Charming, casual
  low:        "cgSgspJ2msm6clMCkdW9", // Jessica - Warm, encouraging
  gutter:     "cgSgspJ2msm6clMCkdW9", // Jessica - Warm, encouraging
  gameStart:  "IKne3meq5aSn9XLyUdCD", // Charlie - Confident announcer
  gameOver:   "IKne3meq5aSn9XLyUdCD", // Charlie - Confident announcer
  nextPlayer: "iP95p4xoKVk53GoZ742B", // Chris - Casual transition
};

// Style intensity varies by mood — celebration is more expressive
const STYLE_MAP = {
  strike: 0.85,
  spare: 0.75,
  high: 0.65,
  medium: 0.45,
  low: 0.4,
  gutter: 0.5,
  gameStart: 0.7,
  gameOver: 0.8,
  nextPlayer: 0.35,
};

export const AUDIO_DIR = path.join(__dirname, "audio");

if (!fs.existsSync(AUDIO_DIR)) {
  fs.mkdirSync(AUDIO_DIR, { recursive: true });
}

/**
 * Generate speech audio via ElevenLabs and save to disk.
 * @param {string} text - The commentary text to speak
 * @param {string} mood - The mood key (strike, gutter, etc.)
 * @returns {Promise<string>} filename of the generated MP3
 */
export async function generateSpeech(text, mood) {
  const voiceId = VOICE_MAP[mood] || VOICE_MAP.medium;
  const style = STYLE_MAP[mood] ?? 0.5;

  const res = await fetch(
    `https://api.elevenlabs.io/v1/text-to-speech/${voiceId}`,
    {
      method: "POST",
      headers: {
        "xi-api-key": elevenLabsKey(),
        "Content-Type": "application/json",
        Accept: "audio/mpeg",
      },
      body: JSON.stringify({
        text,
        model_id: "eleven_turbo_v2_5",
        voice_settings: {
          stability: 0.35,
          similarity_boost: 0.8,
          style,
          use_speaker_boost: true,
        },
      }),
    },
  );

  if (!res.ok) {
    const body = await res.text().catch(() => "");
    throw new Error(`ElevenLabs ${res.status}: ${body}`);
  }

  const buf = Buffer.from(await res.arrayBuffer());
  const filename = `announce_${Date.now()}_${Math.random().toString(36).slice(2, 8)}.mp3`;
  fs.writeFileSync(path.join(AUDIO_DIR, filename), buf);

  scheduleCleanup();
  return filename;
}

// Purge audio files older than 2 minutes (runs at most once per 30s)
let cleanupScheduled = false;
function scheduleCleanup() {
  if (cleanupScheduled) return;
  cleanupScheduled = true;
  setTimeout(() => {
    cleanupScheduled = false;
    try {
      const now = Date.now();
      for (const f of fs.readdirSync(AUDIO_DIR)) {
        if (!f.endsWith(".mp3")) continue;
        const fp = path.join(AUDIO_DIR, f);
        if (now - fs.statSync(fp).mtimeMs > 120_000) fs.unlinkSync(fp);
      }
    } catch { /* ignore */ }
  }, 30_000);
}
