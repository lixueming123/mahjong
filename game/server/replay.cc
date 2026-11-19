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

json::Value RecipientsOfFrame(const std::vector<int>& recipients) {
  json::Value array = json::Value::Arr(recipients.size());
  for (const int seat : recipients) {
    array.Append(json::Value::Int(seat));
  }
  return array;
}

/** Whether any seat's view moved in this step. */
bool ViewsChanged(const SeatViews& views) {
  for (const std::string& text : views) {
    if (!text.empty()) {
      return true;
    }
  }
  return false;
}

// A seat view is already rendered JSON, so it is spliced verbatim like the frames: wrapping it in
// Str would escape every quote it contains. An empty seat stays an empty string, which the reader
// treats as "unchanged".
json::Value ViewsOfStep(const SeatViews& views) {
  json::Value array = json::Value::Arr(kSeatCount);
  for (const std::string& text : views) {
    if (text.empty()) {
      array.Append(json::Value::Str(std::string()));
    } else {
      array.Append(json::Value::RawJson(text));
    }
  }
  return array;
}

bool IsJsonSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

size_t SkipJsonSpace(const std::string& text, size_t index) {
  while (index < text.size() && IsJsonSpace(text[index])) {
    ++index;
  }
  return index;
}

// Advances |*index| past exactly one JSON value. Structural only: it validates shape (balanced
// brackets, terminated strings, escapes consumed in pairs) and nothing else, because the documents
// it walks were written by json::Value::Dump and are known good — a failure here means the caller
// was handed something that is not a recording, which is answered as "no such file".
bool SkipJsonValue(const std::string& text, size_t* index) {
  size_t i = *index;
  if (i >= text.size()) {
    return false;
  }
  if (text[i] == '"') {
    ++i;
    while (i < text.size()) {
      if (text[i] == '\\') {
        i += 2;
        continue;
      }
      if (text[i] == '"') {
        *index = i + 1;
        return true;
      }
      ++i;
    }
    return false;
  }
  if (text[i] == '{' || text[i] == '[') {
    int depth = 0;
    while (i < text.size()) {
      const char c = text[i];
      if (c == '"') {
        if (!SkipJsonValue(text, &i)) {
          return false;
        }
        continue;
      }
      if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        --depth;
        if (depth == 0) {
          *index = i + 1;
          return true;
        }
      }
      ++i;
    }
    return false;
  }
  const size_t start = i;
  while (i < text.size() && !IsJsonSpace(text[i]) && text[i] != ',' && text[i] != '}' && text[i] != ']') {
    ++i;
  }
  if (i == start) {
    return false;
  }
  *index = i;
  return true;
}

// Where a rendered replay document keeps its timeline. |key_start| is the opening quote of the
// `steps` member (the merge keeps everything before it, comma included), [|list_begin|,
// |list_end|) is the array itself with its brackets.
struct StepsSpan {
  size_t key_start = 0;
  size_t list_begin = 0;
  size_t list_end = 0;
};

// Walks the TOP LEVEL of a rendered replay document looking for `steps`. Depth matters: a key of
// the same name inside a frame would be a different member, and the merge would splice in the
// wrong place if it matched. Nothing is parsed — see MergeReplayParts for why.
bool FindTopLevelSteps(const std::string& text, StepsSpan* span) {
  size_t i = SkipJsonSpace(text, 0);
  if (i >= text.size() || text[i] != '{') {
    return false;
  }
  ++i;
  bool found = false;
  while (true) {
    i = SkipJsonSpace(text, i);
    if (i >= text.size()) {
      return false;
    }
    if (text[i] == '}') {
      return found;
    }
    const size_t key_begin = i;
    size_t after_key = i;
    if (text[i] != '"' || !SkipJsonValue(text, &after_key)) {
      return false;
    }
    i = SkipJsonSpace(text, after_key);
    if (i >= text.size() || text[i] != ':') {
      return false;
    }
    i = SkipJsonSpace(text, i + 1);
    const size_t value_begin = i;
    if (!SkipJsonValue(text, &i)) {
      return false;
    }
    const size_t value_end = i;
    // The key is compared unescaped: it is written by Serialize(), which spells it `steps`.
    if (after_key - key_begin == std::string_view("\"steps\"").size() &&
        text.compare(key_begin, after_key - key_begin, "\"steps\"") == 0) {
      if (found) {
        return false;  // two timelines: refuse rather than pick one
      }
      if (value_end - value_begin < 2 || text[value_begin] != '[' || text[value_end - 1] != ']') {
        return false;
      }
      span->key_start = key_begin;
      span->list_begin = value_begin;
      span->list_end = value_end;
      found = true;
    }
    i = SkipJsonSpace(text, value_end);
    if (i >= text.size()) {
      return false;
    }
    if (text[i] == ',') {
      ++i;
      continue;
    }
    if (text[i] == '}') {
      return found;
    }
    return false;
  }
}

}  // namespace

ReplayRecorder::ReplayRecorder(ReplayMeta meta) : meta_(std::move(meta)) {}

void ReplayRecorder::AddStep(int64_t t_ms, const std::vector<Outbound>& outbounds, const SeatViews* views) {
  Step step;
  bool views_changed = false;
  if (views != nullptr) {
    // Only the seats whose view actually moved. A state_sync step already touches the drawer every
    // draw, so in practice this is "the drawer plus any seat whose table just changed".
    for (size_t seat = 0; seat < views->size(); ++seat) {
      if ((*views)[seat] != last_views_[seat]) {
        step.views[seat] = (*views)[seat];
        last_views_[seat] = (*views)[seat];
        views_changed = true;
      }
    }
  }
  if (outbounds.empty() && !views_changed) {
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

std::string ReplayRecorder::FileNameFor(std::string_view room_id) { return std::string(room_id) + ".json.gz"; }

std::string ReplayRecorder::PartFileNameFor(std::string_view room_id, int part_index) {
  // Two digits cover the 4..8 hands a match can have (max_rounds + max_renchan)
  // with room to spare; a longer match still sorts correctly because the padding only fixes the
  // width up to 99 and the merge sorts by the index it reads back anyway.
  std::string index = std::to_string(part_index);
  if (index.size() < 2) {
    index.insert(index.begin(), 2 - index.size(), '0');
  }
  return std::string(room_id) + ".h" + index + ".json.gz";
}

std::string ReplayRecorder::LegacyFileNameFor(std::string_view room_id) { return std::string(room_id) + ".json"; }

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
    json::Value entry = json::Value::Obj(3);  // t, v, f
    entry.Set("t", json::Value::Int(step.t_ms));
    // `v` only when some seat's view moved: a whole-table snapshot is the bulk of a step's cost,
    // and most steps (a discard_event, a deadline) change no seat's own view.
    if (ViewsChanged(step.views)) {
      entry.Set("v", ViewsOfStep(step.views));
    }
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

std::string MergeReplayParts(const std::vector<std::string>& parts) {
  if (parts.empty()) {
    return {};
  }
  std::vector<StepsSpan> spans;
  spans.reserve(parts.size());
  for (const std::string& part : parts) {
    StepsSpan span;
    if (!FindTopLevelSteps(part, &span)) {
      spdlog::warn("replay: a hand part is not a readable recording; answering not-found");
      return {};
    }
    spans.push_back(span);
  }
  // Size up front: the result is the whole document, and growing it a hand at a time would copy
  // the accumulated prefix once per part.
  size_t total = spans.back().key_start + (parts.back().size() - spans.back().list_end);
  for (size_t index = 0; index < parts.size(); ++index) {
    total += spans[index].list_end - spans[index].list_begin;
  }
  std::string merged;
  merged.reserve(total + 1);
  // The head (format … seats, and the finished/match_over/totals/final_rank block that precedes
  // the timeline) comes from the LAST part: it is the only one that knows whether the match
  // ended, it carries the final cumulative totals, and one step can settle two rounds, in which
  // case the second one lands in a part with no steps of its own.
  const StepsSpan& last = spans.back();
  merged.append(parts.back(), 0, last.key_start);
  merged.append("\"steps\":[");
  bool wrote_step = false;
  for (size_t index = 0; index < parts.size(); ++index) {
    const StepsSpan& span = spans[index];
    // The inner text between the brackets, verbatim: the parts were rendered by the same
    // serializer, so their steps are already in the one spelling this file has.
    const size_t inner_begin = span.list_begin + 1;
    const size_t inner_end = span.list_end - 1;
    if (inner_end <= inner_begin) {
      continue;  // an empty part: a hand that produced no step of its own
    }
    if (wrote_step) {
      merged.push_back(',');
    }
    merged.append(parts[index], inner_begin, inner_end - inner_begin);
    wrote_step = true;
  }
  merged.push_back(']');
  merged.append(parts.back(), last.list_end, std::string::npos);
  return merged;
}

}  // namespace mahjong
