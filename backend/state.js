// In-memory state management (no database needed for local WiFi setup)

export class StateManager {
  constructor() {
    this.state = {
      status: "waiting", // waiting, onboarding, payment, active, completed
      players: [],
      ready: false,
      paymentQr: null,
      paymentStatus: null, // pending, success, failed
      amount: null,
      gameStarted: false,
      currentPlayer: 0,
      frame: 1,
      roll: 1,
      // Simple scoring model, populated when game starts and as ESP sends scores
      scores: [], // [{ name, frames: [{ ball1, ball2, ball3, cumulative }], totalScore, maxScore }]
    };
  }

  getState() {
    return { ...this.state };
  }

  setState(updates) {
    this.state = { ...this.state, ...updates };
  }

  getPlayers() {
    return [...this.state.players];
  }

  addPlayer(name) {
    if (this.state.players.length >= 6) {
      throw new Error("Maximum 6 players allowed");
    }

    const player = {
      id: `player_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`,
      name: name.trim(),
      index: this.state.players.length,
    };

    this.state.players.push(player);

    // Auto-set ready if we have at least 1 player
    if (this.state.players.length >= 1) {
      this.state.ready = true;
    }
  }

  removePlayer(index) {
    if (index >= 0 && index < this.state.players.length) {
      this.state.players.splice(index, 1);
      // Update indices
      this.state.players.forEach((player, i) => {
        player.index = i;
      });

      // Update ready status
      this.state.ready = this.state.players.length >= 1;
    }
  }

  reset() {
    this.state = {
      status: "waiting",
      players: [],
      ready: false,
      paymentQr: null,
      paymentStatus: null,
      amount: null,
      gameStarted: false,
      currentPlayer: 0,
      frame: 1,
      roll: 1,
      scores: [],
    };
  }
}
