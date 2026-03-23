import { useEffect, useRef } from "react";

interface Announcement {
  audioUrl: string;
  text: string;
  mood: string;
  timestamp: number;
}

let audioContextUnlocked = false;

function unlockAudioContext() {
  if (audioContextUnlocked) return;
  try {
    const AudioCtx = window.AudioContext || (window as any).webkitAudioContext;
    if (!AudioCtx) return;
    const ctx = new AudioCtx();
    const buffer = ctx.createBuffer(1, 1, 22050);
    const source = ctx.createBufferSource();
    source.buffer = buffer;
    source.connect(ctx.destination);
    source.start(0);
    ctx.resume().then(() => {
      audioContextUnlocked = true;
      ctx.close();
    });
  } catch {
    // silently ignore
  }
}

/**
 * Plays TTS audio announcements as they arrive.
 * Queues clips so overlapping announcements play sequentially.
 * Auto-unlocks AudioContext on mount for kiosk/TV environments.
 */
export function useAudioAnnouncer(announce: Announcement | null) {
  const queueRef = useRef<string[]>([]);
  const playingRef = useRef(false);
  const lastTimestampRef = useRef(0);

  useEffect(() => {
    unlockAudioContext();
  }, []);

  useEffect(() => {
    if (!announce || !announce.audioUrl) return;
    if (announce.timestamp <= lastTimestampRef.current) return;
    lastTimestampRef.current = announce.timestamp;

    queueRef.current.push(announce.audioUrl);
    playNext();
  }, [announce]);

  function playNext() {
    if (playingRef.current || queueRef.current.length === 0) return;
    playingRef.current = true;

    const url = queueRef.current.shift()!;
    const audio = new Audio(url);
    audio.volume = 1.0;

    audio.addEventListener("ended", () => {
      playingRef.current = false;
      playNext();
    });

    audio.addEventListener("error", () => {
      console.warn("Audio playback failed for:", url);
      playingRef.current = false;
      playNext();
    });

    audio.play().catch(() => {
      playingRef.current = false;
      playNext();
    });
  }
}
