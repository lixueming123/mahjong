// The `overloaded` visitor for `std::visit` (seat_endpoint.h / room_contract.h).
//
// Both actor envelopes hold one message struct per kind in a `std::variant`, and each actor's
// single `Dispatch` turns that variant back into a call with `std::visit`. Handling every
// alternative as its own overload (no `default`, no catch-all lambda) is what makes the dispatch
// exhaustive at compile time: add a kind and the build stops until the arm exists.

#pragma once

namespace mahjong {

template <class... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
};

// Explicit guide so `overloaded{l1, l2}` deduces its base classes.
template <class... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

}  // namespace mahjong
