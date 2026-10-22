// The `overloaded` visitor for `std::visit` (seat_endpoint.h / room.h).
//
// Both actor envelopes hold one message struct per kind in a `std::variant`, and each actor's
// single `Dispatch` entry point turns that variant back into a call with `std::visit`. The
// alternatives therefore have to be handled by a set of lambdas; this is the standard way to
// pack them into one visitor object.
//
// Keeping every kind as its own overload (no `default`, no catch-all generic lambda) is what
// makes the dispatch exhaustive at compile time: add a kind to the variant and the build stops
// until the arm exists. The old `Kind` switch could not promise that — this tree compiles with
// no `-Wswitch`.

#pragma once

namespace mahjong {

template <class... Fs>
struct overloaded : Fs... {
  using Fs::operator()...;
};

// Explicit guide so `overloaded{l1, l2}` deduces its base classes (the implicit one for an
// aggregate with a pack of bases is not something to rely on).
template <class... Fs>
overloaded(Fs...) -> overloaded<Fs...>;

}  // namespace mahjong
