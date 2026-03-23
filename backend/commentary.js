import "./env.js";
import OpenAI from "openai";

function getOpenAI() {
  return new OpenAI({
    apiKey: (process.env.OPENAI_API_KEY || "").trim(),
  });
}

const COMMENTARY_MODEL = (
  process.env.OPENAI_COMMENTARY_MODEL || "gpt-4o-mini"
).trim();

const SYSTEM_PROMPT = `You are an energetic, witty bowling alley announcer for PlayArka duckpin bowling (8 pins per frame, max score 280).

When the user message includes a MATCH CONTEXT section, use it: compare rivals, gaps, comebacks, and how totals built across frames—not just this single roll.
Otherwise use the scoreboard and event details given.

Rules:
- Write exactly ONE short sentence (max 15 words). Never exceed this.
- Be natural, fun, and varied — never repeat yourself.
- React to the CONTEXT: score gaps, streaks, clutch moments, comebacks, blowouts.
- Use the player's name.
- If someone has consecutive strikes, hype the streak ("three in a row!", "unstoppable!").
- If someone is trailing, encourage them ("only 12 points to catch up!").
- If it's the final frames, raise the stakes ("this is it!", "last chance!").
- For gutters, be encouraging not mocking.
- For game start, welcome everyone with energy.
- For game over, celebrate the winner and congratulate everyone.
- Do NOT use hashtags, emojis, or quotation marks.
- Broadcast-style, punchy, conversational.`;

function getMood(event, pinsThisRoll, isStrike, isSpare) {
  if (event === "gameStart") return "gameStart";
  if (event === "gameOver") return "gameOver";
  if (event === "nextPlayer") return "nextPlayer";
  if (isStrike) return "strike";
  if (isSpare) return "spare";
  if (pinsThisRoll === 0) return "gutter";
  if (pinsThisRoll <= 2) return "low";
  if (pinsThisRoll <= 5) return "medium";
  return "high";
}

function appendMatchContext(lines, ctx) {
  if (!ctx.matchContext?.trim()) return;
  lines.push("");
  lines.push("MATCH CONTEXT:");
  lines.push(ctx.matchContext.trim());
}

function buildPrompt(ctx) {
  const lines = [];

  if (ctx.event === "gameStart") {
    const names = ctx.allPlayers?.map((p) => p.name).join(", ") || ctx.player;
    lines.push(`EVENT: Game is starting!`);
    lines.push(`PLAYERS: ${names}`);
    lines.push(`Announce the start and get the crowd hyped.`);
    return lines.join("\n");
  }

  if (ctx.event === "gameOver") {
    lines.push(`EVENT: Game over!`);
    lines.push(`WINNER: ${ctx.player} with ${ctx.score} points`);
    if (ctx.allPlayers) {
      const scoreboard = ctx.allPlayers
        .map((p) => `${p.name}: ${p.totalScore}`)
        .join(", ");
      lines.push(`FINAL SCORES: ${scoreboard}`);
    }
    appendMatchContext(lines, ctx);
    lines.push(`Celebrate the winner and wrap up.`);
    return lines.join("\n");
  }

  if (ctx.event === "nextPlayer") {
    lines.push(`EVENT: Next player's turn`);
    lines.push(`NOW UP: ${ctx.player}`);
    if (ctx.frame) lines.push(`FRAME: ${ctx.frame}/10`);
    if (ctx.allPlayers) {
      const scoreboard = ctx.allPlayers
        .map((p) => `${p.name}: ${p.totalScore}`)
        .join(", ");
      lines.push(`SCORES: ${scoreboard}`);
    }
    appendMatchContext(lines, ctx);
    lines.push(`Announce the next bowler briefly.`);
    return lines.join("\n");
  }

  // Score event
  lines.push(`PLAYER: ${ctx.player}`);
  lines.push(`FRAME: ${ctx.frame}/10, ROLL: ${ctx.roll}`);
  lines.push(`PINS THIS ROLL: ${ctx.pins}`);

  if (ctx.isStrike)
    lines.push(`RESULT: STRIKE! (all 8 pins down on first roll)`);
  else if (ctx.isSpare)
    lines.push(`RESULT: SPARE! (cleared remaining pins on second roll)`);
  else if (ctx.pins === 0) lines.push(`RESULT: Gutter ball, zero pins`);
  else lines.push(`RESULT: ${ctx.pins} pins knocked down`);

  if (ctx.streak && ctx.streak > 1) {
    lines.push(`STREAK: ${ctx.player} has ${ctx.streak} strikes in a row!`);
  }
  if (ctx.spareStreak && ctx.spareStreak > 1) {
    lines.push(
      `SPARE STREAK: ${ctx.player} has ${ctx.spareStreak} spares in a row!`,
    );
  }

  if (ctx.allPlayers && ctx.allPlayers.length > 1) {
    const scoreboard = ctx.allPlayers
      .map((p) => `${p.name}: ${p.totalScore}`)
      .join(", ");
    lines.push(`SCOREBOARD: ${scoreboard}`);

    const sorted = [...ctx.allPlayers].sort(
      (a, b) => b.totalScore - a.totalScore,
    );
    const leader = sorted[0];
    const current = ctx.allPlayers.find((p) => p.name === ctx.player);
    if (current && leader && leader.name !== current.name) {
      const gap = leader.totalScore - current.totalScore;
      lines.push(`${ctx.player} is ${gap} points behind ${leader.name}.`);
    } else if (
      current &&
      leader &&
      leader.name === current.name &&
      sorted.length > 1
    ) {
      const gap = current.totalScore - sorted[1].totalScore;
      lines.push(
        `${ctx.player} leads by ${gap} points over ${sorted[1].name}.`,
      );
    }
  }

  if (ctx.frame >= 8) {
    lines.push(`PRESSURE: Final frames — every pin matters!`);
  }

  appendMatchContext(lines, ctx);
  lines.push(`React to this moment; use match context when relevant.`);
  return lines.join("\n");
}

/**
 * Generate AI commentary for a bowling event.
 * @param {object} ctx - Full game context
 * @returns {Promise<{ text: string, mood: string }>}
 */
export async function generateCommentary(ctx) {
  const mood = getMood(ctx.event, ctx.pins ?? 0, ctx.isStrike, ctx.isSpare);
  const userPrompt = buildPrompt(ctx);

  try {
    const response = await getOpenAI().chat.completions.create({
      model: COMMENTARY_MODEL,
      messages: [
        { role: "system", content: SYSTEM_PROMPT },
        { role: "user", content: userPrompt },
      ],
      max_tokens: 60,
      temperature: 0.9,
    });

    let text = response.choices[0]?.message?.content?.trim() || "";
    text = text.replace(/^["']|["']$/g, "");

    if (!text) {
      return { text: "", mood };
    }

    return { text, mood };
  } catch (err) {
    console.error("OpenAI commentary generation failed:", err.message);
    return { text: fallback(ctx), mood };
  }
}

function fallback(ctx) {
  if (ctx.isStrike)
    return `STRIKE for ${ctx.player || "Bowler"}! All 8 pins down!`;
  if (ctx.isSpare) return `SPARE! Great recovery by ${ctx.player || "Bowler"}!`;
  if (ctx.event === "gameStart") return `Welcome to PlayArka! Let's bowl!`;
  if (ctx.event === "gameOver")
    return `Game over! ${ctx.player || "The winner"} takes it with ${ctx.score} points!`;
  if (ctx.event === "nextPlayer")
    return `${ctx.player || "Next bowler"}, you're up!`;
  if (ctx.pins === 0)
    return `Tough luck ${ctx.player || "bowler"}, shake it off!`;
  return `${ctx.pins} pins for ${ctx.player || "Bowler"}!`;
}
