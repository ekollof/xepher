#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>
#include <algorithm>
#include <ranges>
#include <utility>

#include "timeout_listener.hh"
#include "plugin.inl"
#include "omemo_xep.inl"
#include "connection/strophe_stream_features.hh"
#include "unit.inl"
#include "omemo_mechanics.inl"
