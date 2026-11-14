#include "replay.h"

#include <spdlog/spdlog.h>

#include <string>
#include <utility>

namespace mahjong {
namespace {

json::Value OptionsOf(const std::string& options_json) {
  if (options_json.empty()) {
    return json::Value::Obj(0);
  }
  const std::optional<json::Value> parsed = json::Value::Parse(options_json, nullptr);
  if (!parsed.has_value() || parsed->IsNull() || !parsed->IsObject()) {
    spdlog::warn("replay: options \"{}\" is not a JSON object; recording {{}}", options_json);
    return json::Value::Obj(0);
  }
  return json::Value::RawJson(options_json);
}

// A comma-joined tile list ("1m,2m,0p"); empty when the hand is empty, which is also how a step
// says "this seat's hand did not change".
json::Value HandsOfStep(const std::array<std::string, kSeatCount>& hands) {
  json::Value array = json::Value::Arr(kSeatCount);
  for (const std::string& hand : hands) {
    array.Append(json::Value::Str(hand));
  }
  return array;
}

json::Value RecipientsOfFrame(const std::vector<int>& recipients) {
  json::Value array = json::Value::Arr(recipients.size());
  for (const int seat : recipients) {
    array.Append(json::Value::Int(seat));
  }
  return array;
}

}  // namespace

ReplayRecorder::ReplayRecorder(ReplayMeta meta) : meta_(std::move(meta)) {}

void ReplayRecorder::AddStep(int64_t t_ms, const std::vector<Outbound>& outbounds,
                             const std::array<std::string, kSeatCount>* hands) {
  Step step;
  bool hands_changed = false;
  if (hands != nullptr) {
    // Only the seats whose hand actually moved. A state_sync step already touches the drawer
    // every draw, so in practice this is "the drawer plus any seat that just melded".
    for (size_t seat = 0; seat < hands->size(); ++seat) {
      if ((*hands)[seat] != last_hands_[seat]) {
        step.hands[seat] = (*hands)[seat];
        last_hands_[seat] = (*hands)[seat];
        hands_changed = true;
      }
    }
  }
  if (outbounds.empty() && !hands_changed) {
    return;  // a step that changed nothing observable is not worth a timeline entry
  }
  step.t_ms = t_ms;
  step.frames.reserve(outbounds.size());
  for (const Outbound& outbound : outbounds) {
    Step::Frame frame;
    // An empty recipient list means "every seat with a wire" (game.h). Recording the expansion
    // keeps the player free of that rule: it just asks "was this frame for my seat?".
    if (outbound.recipients.empty()) {
      for (int seat = 0; seat < kSeatCount; ++seat) {
        frame.recipients.push_back(seat);
      }
    } else {
      frame.recipients = outbound.recipients;
    }
    frame.frame = outbound.frame;
    step.frames.push_back(std::move(frame));
  }
  steps_.push_back(std::move(step));
}

void ReplayRecorder::Finish(const GameResult& result) {
  finished_ = true;
  match_over_ = result.match_over;
  totals_ = result.totals;
  final_rank_ = result.final_rank;
}

bool ReplayRecorder::IncludesAccount(uint64_t account_id) const {
  if (account_id == 0) {
    return false;  // a guest has no history to claim (§4.13)
  }
  for (const ReplaySeat& seat : meta_.seats) {
    if (!seat.is_bot && seat.account_id == account_id) {
      return true;
    }
  }
  return false;
}

std::string ReplayRecorder::FileNameFor(std::string_view room_id) { return std::string(room_id) + ".json"; }

std::string ReplayRecorder::Serialize() const {
  constexpr size_t kMetaMembers = 11;
  json::Value root = json::Value::Obj(kMetaMembers);
  root.Set("format", json::Value::Str("mahjong-replay"));
  root.Set("version", json::Value::Int(kFormatVersion));
  root.Set("room_id", json::Value::Str(meta_.room_id));
  root.Set("game_id", json::Value::Str(meta_.game_id));
  root.Set("options_json", std::move(OptionsOf(meta_.options_json)));
  root.Set("seed", json::Value::Int(static_cast<int64_t>(meta_.seed)));
  root.Set("started_at", json::Value::Int(meta_.started_at_ms));

  json::Value seats = json::Value::Arr(kSeatCount);
  for (const ReplaySeat& seat : meta_.seats) {
    json::Value entry = json::Value::Obj(5);  // seat, player_id, name, is_bot, account_id
    entry.Set("seat", json::Value::Int(seat.seat));
    entry.Set("player_id", json::Value::Str(seat.player_id));
    entry.Set("name", json::Value::Str(seat.name));
    entry.Set("is_bot", json::Value::Bool(seat.is_bot));
    // A guest seat keeps 0: the reader uses it only to decide who may claim the file.
    entry.Set("account_id", json::Value::Int(static_cast<int64_t>(seat.account_id)));
    seats.Append(std::move(entry));
  }
  root.Set("seats", std::move(seats));

  root.Set("finished", json::Value::Bool(finished_));
  root.Set("match_over", json::Value::Bool(match_over_));
  json::Value totals = json::Value::Arr(kSeatCount);
  json::Value ranks = json::Value::Arr(kSeatCount);
  for (size_t seat = 0; seat < static_cast<size_t>(kSeatCount); ++seat) {
    totals.Append(json::Value::Int(totals_[seat]));
    ranks.Append(json::Value::Int(final_rank_[seat]));
  }
  root.Set("totals", std::move(totals));
  root.Set("final_rank", std::move(ranks));

  json::Value steps = json::Value::Arr(steps_.size());
  for (const Step& step : steps_) {
    json::Value entry = json::Value::Obj(3);  // t, h, f
    entry.Set("t", json::Value::Int(step.t_ms));
    entry.Set("h", HandsOfStep(step.hands));
    json::Value frames = json::Value::Arr(step.frames.size());
    for (const Step::Frame& frame : step.frames) {
      // [ [recipients], <frame> ] — the frame is spliced VERBATIM, not stored as a string.
      //
      // It is already JSON (the room recorded the bytes it delivered), so wrapping it in a
      // string would escape every `"` it contains: on a riichi match that is ~50k backslashes
      // for nothing, and it makes the file unreadable by hand. RawJson is exactly the tool for
      // this (state_sync.seats is spliced the same way); the contract is that the caller hands
      // over text that is already valid JSON, which a frame always is because it came out of
      // json::Value::Dump().
      json::Value pair = json::Value::Arr(2);
      pair.Append(RecipientsOfFrame(frame.recipients));
      pair.Append(json::Value::RawJson(frame.frame));
      frames.Append(std::move(pair));
    }
    entry.Set("f", std::move(frames));
    steps.Append(std::move(entry));
  }
  root.Set("steps", std::move(steps));
  return root.Dump();
}

}  // namespace mahjong
