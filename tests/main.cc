#if defined(__OpenBSD__)

int main() { return 0; }

#else

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include <doctest/doctest.h>

#include "plugin.inl"
#include "omemo_xep.inl"
#include "unit.inl"
#include "omemo_mechanics.inl"

#endif
