#pragma once

#include <string>

using CheckFn = void (*)(bool ok, const char* what, const std::string& where);

void RunIconRequestTests(CheckFn check);
