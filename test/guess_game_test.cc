#include <iostream>
#include <random>
#include <atomic>
#include <gtest/gtest.h>
#include "ex_actor/api.h"
#include "ex_actor/internal/actor_registry.h"
#include "exec/async_scope.hpp"

namespace ex = stdexec;

class GameManager;

class Player {
 public:
  explicit Player(ex_actor::ActorRef<GameManager> game_ref, int player_id)
      : game_ref_(game_ref), player_id_(player_id), rng_(std::random_device{}()) {}

  stdexec::task<void> MakeGuess();

  int GetPlayerId() const { return player_id_; }

 private:
  ex_actor::ActorRef<GameManager> game_ref_;
  int player_id_;
  std::mt19937 rng_;
};

class GameManager {
 public:
  GameManager() : min_num_(1), max_num_(100), current_player_idx_(0), game_started_(false) {
    std::random_device rd;
    gen_ = std::mt19937(rd());
    secret_number_ = std::uniform_int_distribution<int>(1, 100)(gen_);
    std::cout << "[GameManager] Secret number is: " << secret_number_ << "\n";
  }

  void JoinGame(ex_actor::ActorRef<Player> player_ref) {
    players_.push_back(player_ref);
    std::cout << "[GameManager] Player " << player_ref.GetActorId() << " joined! Total: " << players_.size() << "\n";
  }

  void StartGame() {
    if (players_.size() < 3) {
      std::cout << "[GameManager] Need at least 3 players\n";
      return;
    }
    game_started_ = true;
    std::cout << "[GameManager] Game started! Range: [" << min_num_ << ", " << max_num_ << "]\n";
  }

  stdexec::task<void> ProcessTurn() {
    if (!game_started_) {
      co_return;
    }

    while (game_started_) {
      auto& player = players_[current_player_idx_];
      std::cout << "[GameManager] Player " << current_player_idx_ << "'s turn. Range: [" << min_num_ << ", " << max_num_ << "]\n";

      co_await player.template Send<&Player::MakeGuess>();
    }
    co_return;
  }

  bool Guess(int player_idx, int guess) {
    if (!game_started_) {
      std::cout << "[GameManager] Game not started!\n";
      return false;
    }

    if (player_idx != current_player_idx_) {
      std::cout << "[GameManager] Not player " << player_idx << "'s turn!\n";
      return false;
    }

    std::cout << "[GameManager] Player " << player_idx << " guessed: " << guess << "\n";

    if (guess == secret_number_) {
      std::cout << "[GameManager] Player " << player_idx << " guessed correctly! Player " << player_idx << " LOSES!\n";
      std::cout << "[GameManager] Game Over!\n";
      game_started_ = false;
      return true;
    }

    if (guess < secret_number_) {
      min_num_ = guess + 1;
      std::cout << "[GameManager] Too low! New range: [" << min_num_ << ", " << max_num_ << "]\n";
    } else {
      max_num_ = guess - 1;
      std::cout << "[GameManager] Too high! New range: [" << min_num_ << ", " << max_num_ << "]\n";
    }

    current_player_idx_ = (current_player_idx_ + 1) % players_.size();
    return false;
  }

  int GetMinNum() const { return min_num_; }
  int GetMaxNum() const { return max_num_; }
  bool IsGameStarted() const { return game_started_; }
  int GetCurrentPlayer() const { return current_player_idx_; }
  int GetSecretNumber() const { return secret_number_; }
  size_t GetPlayerCount() const { return players_.size(); }

 private:
  int min_num_;
  int max_num_;
  int secret_number_;
  int current_player_idx_;
  std::vector<ex_actor::ActorRef<Player>> players_;
  std::mt19937 gen_;
  bool game_started_;
};

stdexec::task<void> Player::MakeGuess() {
  auto min_num = co_await game_ref_.template Send<&GameManager::GetMinNum>();
  auto max_num = co_await game_ref_.template Send<&GameManager::GetMaxNum>();

  if (!co_await game_ref_.template Send<&GameManager::IsGameStarted>()) {
    std::cout << "[Player " << player_id_ << "] Game not started yet.\n";
    co_return;
  }

  auto current_player = co_await game_ref_.template Send<&GameManager::GetCurrentPlayer>();
  if (current_player != player_id_) {
    co_return;
  }

  std::uniform_int_distribution<int> dist(min_num, max_num);
  int guess = dist(rng_);

  std::cout << "[Player " << player_id_ << "] Guessing: " << guess << " (range: " << min_num << "-" << max_num << ")\n";

  bool game_over = co_await game_ref_.template Send<&GameManager::Guess>(player_id_, guess);
  if (game_over) {
    std::cout << "[Player " << player_id_ << "] I guessed correctly! I LOSE!\n";
  }
  co_return;
}

TEST(GuessGameTest, ThreePlayersGuessNumber) {
  auto game_coroutine = []() -> stdexec::task<void> {
    auto game_manager = co_await ex_actor::Spawn<GameManager>();

    auto player1 = co_await ex_actor::Spawn<Player>(game_manager, 0);
    auto player2 = co_await ex_actor::Spawn<Player>(game_manager, 1);
    auto player3 = co_await ex_actor::Spawn<Player>(game_manager, 2);

    co_await game_manager.template Send<&GameManager::JoinGame>(player1);
    co_await game_manager.template Send<&GameManager::JoinGame>(player2);
    co_await game_manager.template Send<&GameManager::JoinGame>(player3);

    co_await game_manager.template Send<&GameManager::StartGame>();

    co_await game_manager.template Send<&GameManager::ProcessTurn>();

    co_return;
  };

  ex_actor::Init(10);
  ex::sync_wait(game_coroutine());
  ex_actor::Shutdown();
}
